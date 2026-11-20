/*
  AlertaCheia - Sensor de nível de arroio com ESP32 (simulação no Wokwi)
  FIAP - Global Solution 2026.2 - Edge Computing & Computer Systems

  O que o dispositivo faz:
  1. Mede o nível da água a cada 1 segundo (sensor ultrassônico HC-SR04).
  2. Decide sozinho, na borda, o nível de risco e liga a sirene (buzzer).
  3. Mostra nível, risco, sirene e conexões no display LCD 20x4.
  4. Publica os dados no ThingSpeak por MQTT a cada 20 segundos.
  5. Recebe por MQTT o limite de alerta e o comando remoto da sirene.

  Diferenciais:
  - Filtro de mediana: usa o valor do meio das 5 últimas medições, então
    uma leitura errada isolada (um galho, um respingo) não dispara o alarme.
  - Histerese: o risco sobe na hora, mas só desce quando a água baixa 10 cm
    além do limite. Evita a sirene ligando e desligando com a água na marca.
  - Barra de nível no display, com a marca do transbordamento.

  Bibliotecas: PubSubClient e LiquidCrystal I2C (arquivo libraries.txt).
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <LiquidCrystal_I2C.h>

// ---------- Rede e ThingSpeak (preencha com os dados da SUA conta) ----------
const char* WIFI_NOME  = "Wokwi-GUEST";
const char* WIFI_SENHA = "";

const char* MQTT_SERVIDOR = "mqtt3.thingspeak.com";
const int   MQTT_PORTA    = 1883;
// Dados do "MQTT Device" criado em ThingSpeak > Devices > MQTT
const char* MQTT_CLIENT_ID = "COLE_AQUI_O_CLIENT_ID";
const char* MQTT_USUARIO   = "COLE_AQUI_O_USERNAME";
const char* MQTT_SENHA     = "COLE_AQUI_A_SENHA";
// Canal 1: medições (o ESP32 publica).  Canal 2: comandos (o ESP32 escuta).
const char* CANAL_MEDICOES = "0000000";
const char* CANAL_COMANDOS = "0000000";

// ---------- Pinos ----------
const int PINO_TRIG     = 5;
const int PINO_ECHO     = 18;
const int PINO_LED_VERDE    = 25;
const int PINO_LED_AMARELO  = 26;
const int PINO_LED_VERMELHO = 27;
const int PINO_SIRENE   = 14;
const int PINO_BOTAO    = 4;

// ---------- Regras (as mesmas do programa em Python e do site) ----------
const float ALTURA_SENSOR_CM = 400.0;  // distância do sensor até o fundo do arroio
const float LIMITE_ATENCAO   = 120.0;
const float LIMITE_EMERGENCIA = 240.0;
const float HISTERESE_CM = 10.0;       // folga para o risco descer
const int   TAMANHO_FILTRO = 5;        // medições usadas na mediana
const float NIVEL_MAX_BARRA = 300.0;   // nível que enche a barra do display
float limiteAlerta = 180.0;            // pode ser alterado remotamente (canal 2, field1)

// ---------- Tempos ----------
const unsigned long PERIODO_AMOSTRA_MS = 1000;  // uma medição por segundo
const int AMOSTRAS_POR_ENVIO = 20;              // envia a média a cada 20 s
const unsigned long PERIODO_RECONEXAO_MS = 5000;
const unsigned long PERIODO_BIPE_MS = 300;

// ---------- Estado do dispositivo ----------
enum Risco { NORMAL, ATENCAO, ALERTA, EMERGENCIA };
const char* NOME_RISCO[] = { "NORMAL    ", "ATENCAO   ", "ALERTA    ", "EMERGENCIA" };

// Modo de teste escolhido pelo botão, para demonstrar as quedas de conexão
enum ModoTeste { CONECTADO, SEM_MQTT, SEM_WIFI };
ModoTeste modoTeste = CONECTADO;

float nivelAtual = 0;          // nível filtrado pela mediana (cm)
float ultimasMedicoes[TAMANHO_FILTRO];  // guarda as medições mais recentes
int   medicoesGuardadas = 0;
int   posicaoFiltro = 0;
float somaNiveis = 0;          // soma das medições desde o último envio
int   amostrasValidas = 0;
int   amostrasNoCiclo = 0;
Risco risco = NORMAL;
bool  sireneLigada = false;
bool  sireneRemota = false;    // comando recebido por MQTT (canal 2, field2)
bool  somDoBipe = false;

unsigned long proximaAmostra = 0;
unsigned long ultimaTentativa = 0;
unsigned long ultimoBipe = 0;
bool botaoAnterior = HIGH;

WiFiClient redeWifi;
PubSubClient mqtt(redeWifi);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ---------- Sensor ----------
// Devolve o nível da água em cm, ou -1 se a leitura falhou.
float medirNivel() {
  digitalWrite(PINO_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PINO_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PINO_TRIG, LOW);

  unsigned long duracao = pulseIn(PINO_ECHO, HIGH, 30000);  // espera no máximo 30 ms
  if (duracao == 0) return -1;

  float distancia = duracao * 0.0343 / 2.0;   // velocidade do som: 0,0343 cm por microssegundo
  float nivel = ALTURA_SENSOR_CM - distancia; // quanto mais perto a água, maior o nível
  if (nivel < 0) nivel = 0;
  return nivel;
}

// Guarda a nova medição e devolve a mediana das últimas 5.
float filtrarPorMediana(float novaMedicao) {
  ultimasMedicoes[posicaoFiltro] = novaMedicao;
  posicaoFiltro = (posicaoFiltro + 1) % TAMANHO_FILTRO;
  if (medicoesGuardadas < TAMANHO_FILTRO) medicoesGuardadas++;

  // Copia e ordena (são só 5 valores, então a ordenação simples basta).
  float ordenadas[TAMANHO_FILTRO];
  for (int i = 0; i < medicoesGuardadas; i++) ordenadas[i] = ultimasMedicoes[i];
  for (int i = 0; i < medicoesGuardadas - 1; i++) {
    for (int j = i + 1; j < medicoesGuardadas; j++) {
      if (ordenadas[j] < ordenadas[i]) {
        float troca = ordenadas[i];
        ordenadas[i] = ordenadas[j];
        ordenadas[j] = troca;
      }
    }
  }
  return ordenadas[medicoesGuardadas / 2];  // valor do meio
}

// ---------- Lógica na borda ----------
Risco riscoPelosLimites(float nivel) {
  if (nivel >= LIMITE_EMERGENCIA) return EMERGENCIA;
  if (nivel >= limiteAlerta)      return ALERTA;
  if (nivel >= LIMITE_ATENCAO)    return ATENCAO;
  return NORMAL;
}

// Com histerese: subir de risco é imediato; para descer, a água precisa
// baixar HISTERESE_CM além do limite.
Risco classificar(float nivel) {
  Risco novo = riscoPelosLimites(nivel);
  if (novo >= risco) return novo;
  Risco comFolga = riscoPelosLimites(nivel + HISTERESE_CM);
  return (comFolga < risco) ? comFolga : risco;
}

void atualizarAtuadores() {
  digitalWrite(PINO_LED_VERDE,    risco == NORMAL);
  digitalWrite(PINO_LED_AMARELO,  risco == ATENCAO || risco == ALERTA);
  digitalWrite(PINO_LED_VERMELHO, risco == ALERTA  || risco == EMERGENCIA);

  // A sirene liga sozinha na emergência, mesmo sem internet, ou por comando remoto.
  sireneLigada = (risco == EMERGENCIA) || sireneRemota;
  if (!sireneLigada) {
    noTone(PINO_SIRENE);
    somDoBipe = false;
  }
}

// Faz a sirene apitar de forma intermitente sem travar o programa.
void tocarSirene() {
  if (!sireneLigada) return;
  if (millis() - ultimoBipe >= PERIODO_BIPE_MS) {
    ultimoBipe = millis();
    somDoBipe = !somDoBipe;
    if (somDoBipe) tone(PINO_SIRENE, 1800);
    else           noTone(PINO_SIRENE);
  }
}

// ---------- Display ----------
void atualizarDisplay() {
  char linha[21];

  lcd.setCursor(0, 0);
  lcd.print("AlertaCheia Sarandi ");

  // Linha 2: nível e barra de 10 casas. O "|" marca o transbordamento.
  snprintf(linha, sizeof(linha), "%3d cm    ", (int)(nivelAtual + 0.5));
  lcd.setCursor(0, 1);
  lcd.print(linha);
  int cheias = (int)(nivelAtual / NIVEL_MAX_BARRA * 10);
  int marca  = (int)(LIMITE_EMERGENCIA / NIVEL_MAX_BARRA * 10);
  lcd.setCursor(10, 1);
  for (int casa = 0; casa < 10; casa++) {
    if (casa < cheias)      lcd.write(255);   // bloco cheio
    else if (casa == marca) lcd.print("|");
    else                    lcd.print("-");
  }

  snprintf(linha, sizeof(linha), "%s Sir:%s", NOME_RISCO[risco], sireneLigada ? "LIG " : "DESL");
  lcd.setCursor(0, 2);
  lcd.print(linha);

  snprintf(linha, sizeof(linha), "WiFi:%s  MQTT:%s   ",
           WiFi.status() == WL_CONNECTED ? "OK " : "OFF",
           mqtt.connected() ? "OK " : "OFF");
  lcd.setCursor(0, 3);
  lcd.print(linha);
}

// ---------- MQTT ----------
// Chamada automaticamente quando chega uma mensagem do canal de comandos.
void aoReceberMensagem(char* topico, byte* dados, unsigned int tamanho) {
  char texto[16];
  if (tamanho == 0 || tamanho >= sizeof(texto)) return;
  memcpy(texto, dados, tamanho);
  texto[tamanho] = '\0';
  float valor = atof(texto);

  String nomeTopico = String(topico);
  if (nomeTopico.endsWith("field1")) {
    // Novo limite de alerta: só aceita valores entre atenção e emergência.
    if (valor > LIMITE_ATENCAO && valor < LIMITE_EMERGENCIA) {
      limiteAlerta = valor;
      Serial.printf("Novo limite de alerta: %.0f cm\n", limiteAlerta);
    }
  } else if (nomeTopico.endsWith("field2")) {
    sireneRemota = (valor >= 1);
    Serial.printf("Sirene remota: %s\n", sireneRemota ? "LIGADA" : "DESLIGADA");
  }
  risco = classificar(nivelAtual);
  atualizarAtuadores();
  atualizarDisplay();
}

// Mantém WiFi e MQTT ligados, tentando de novo a cada 5 s, sem travar as medições.
void cuidarDaConexao() {
  if (modoTeste == SEM_WIFI) return;
  if (millis() - ultimaTentativa < PERIODO_RECONEXAO_MS) return;

  if (WiFi.status() != WL_CONNECTED) {
    ultimaTentativa = millis();
    WiFi.begin(WIFI_NOME, WIFI_SENHA, 6);
    return;
  }
  if (modoTeste == SEM_MQTT || mqtt.connected()) return;

  ultimaTentativa = millis();
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USUARIO, MQTT_SENHA)) {
    String base = String("channels/") + CANAL_COMANDOS + "/subscribe/fields/";
    mqtt.subscribe((base + "field1").c_str());
    mqtt.subscribe((base + "field2").c_str());
    Serial.println("MQTT conectado ao ThingSpeak.");
  } else {
    Serial.printf("Falha no MQTT, codigo %d\n", mqtt.state());
  }
}

void publicarMedia() {
  if (amostrasValidas == 0 || !mqtt.connected()) return;
  float media = somaNiveis / amostrasValidas;

  String topico = String("channels/") + CANAL_MEDICOES + "/publish";
  String dados = "field1=" + String(media, 1) +
                 "&field2=" + String((int)risco) +
                 "&field3=" + String(sireneLigada ? 1 : 0) +
                 "&field4=" + String(limiteAlerta, 0) +
                 "&status=MQTTPUBLISH";
  bool enviado = mqtt.publish(topico.c_str(), dados.c_str());
  Serial.printf("Publicado (%s): %s\n", enviado ? "ok" : "falhou", dados.c_str());
}

// ---------- Botão de teste ----------
// Cada toque muda o modo: conectado -> sem MQTT -> sem WiFi -> conectado.
void lerBotao() {
  bool botaoAgora = digitalRead(PINO_BOTAO);
  if (botaoAnterior == HIGH && botaoAgora == LOW) {
    if (modoTeste == CONECTADO) {
      modoTeste = SEM_MQTT;
      mqtt.disconnect();
      Serial.println("Teste: MQTT desconectado.");
    } else if (modoTeste == SEM_MQTT) {
      modoTeste = SEM_WIFI;
      WiFi.disconnect();
      Serial.println("Teste: WiFi desconectado.");
    } else {
      modoTeste = CONECTADO;
      ultimaTentativa = 0;
      Serial.println("Teste encerrado: reconectando.");
    }
    atualizarDisplay();
    delay(50);  // evita ler o mesmo toque duas vezes
  }
  botaoAnterior = botaoAgora;
}

// ---------- Início ----------
void setup() {
  Serial.begin(115200);
  pinMode(PINO_TRIG, OUTPUT);
  pinMode(PINO_ECHO, INPUT);
  pinMode(PINO_LED_VERDE, OUTPUT);
  pinMode(PINO_LED_AMARELO, OUTPUT);
  pinMode(PINO_LED_VERMELHO, OUTPUT);
  pinMode(PINO_SIRENE, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("AlertaCheia");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NOME, WIFI_SENHA, 6);
  mqtt.setServer(MQTT_SERVIDOR, MQTT_PORTA);
  mqtt.setCallback(aoReceberMensagem);
  mqtt.setSocketTimeout(3);

  delay(1000);
  lcd.clear();
  proximaAmostra = millis();
}

// ---------- Repetição ----------
void loop() {
  lerBotao();
  cuidarDaConexao();
  mqtt.loop();
  tocarSirene();

  // Amostragem com período constante: o próximo horário é calculado a partir
  // do horário previsto (e não do horário real), então os atrasos não se acumulam.
  if ((long)(millis() - proximaAmostra) >= 0) {
    proximaAmostra += PERIODO_AMOSTRA_MS;

    float nivel = medirNivel();
    if (nivel >= 0) {
      nivelAtual = filtrarPorMediana(nivel);
      somaNiveis += nivelAtual;
      amostrasValidas++;
    }
    amostrasNoCiclo++;

    risco = classificar(nivelAtual);
    atualizarAtuadores();
    atualizarDisplay();

    if (amostrasNoCiclo >= AMOSTRAS_POR_ENVIO) {
      publicarMedia();
      somaNiveis = 0;
      amostrasValidas = 0;
      amostrasNoCiclo = 0;
    }
  }
}

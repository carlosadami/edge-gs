# AlertaCheia - dispositivo IoT

Entrega da disciplina **Edge Computing & Computer Systems** (prof. Guilherme Salati).
Atividade: CityzenTech IoT - Conectando o ambiente com o mundo digital.

**Integrante (trabalho individual):** Carlos Alberto Adami da Silva Neto (RM571841)

**Simulação no Wokwi:** https://wokwi.com/projects/481167449591758849

**Gráficos no ThingSpeak (canal de medições):** https://thingspeak.com/channels/3588887

**Vídeo de demonstração:** https://youtube.com/shorts/QMy9KmBCiIU

## Contextualização

Em maio de 2024, o Guaíba chegou a 5,35 m e bairros como o Sarandi ficaram debaixo d'água. Quem mora perto de um arroio só percebe o perigo quando a água chega à porta. O AlertaCheia coloca um sensor de baixo custo sobre o arroio: ele mede o nível, avisa a vizinhança com uma sirene mesmo sem internet e envia os dados para a nuvem, onde o site da equipe mostra o aviso a cada morador.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `sketch.ino` | Código-fonte do ESP32 |
| `diagram.json` | Diagrama da simulação no Wokwi |
| `libraries.txt` | Bibliotecas: PubSubClient e LiquidCrystal I2C |

## Componentes e ligações

| Componente | Função | Pinos do ESP32 |
|---|---|---|
| HC-SR04 | Sensor: mede a distância até a água | TRIG 5, ECHO 18 |
| LCD 20x4 I2C | Interface local | SDA 21, SCL 22 |
| LED verde, amarelo, vermelho | Semáforo de risco | 25, 26, 27 |
| Buzzer | Atuador: sirene comunitária | 14 |
| Botão | Teste de queda de conexão | 4 |

O sensor fica 400 cm acima do fundo do arroio. Nível = 400 - distância medida.

## Como cada requisito é atendido

| Requisito | Como foi feito |
|---|---|
| Leitura de sensor com amostragem precisa e constante | Uma medição a cada 1000 ms. O próximo horário é calculado somando o período ao horário previsto (`proximaAmostra += PERIODO_AMOSTRA_MS`), então atrasos não se acumulam. Nenhum `delay()` no laço principal |
| Atuação com lógica na borda | O ESP32 classifica o risco e liga a sirene em emergência sem depender da internet |
| Display: valor atual | Linha 2: `181 cm` e barra de nível |
| Display: status do atuador | Linha 3: risco e `Sir:LIG` ou `Sir:DESL` |
| Display: status WiFi/MQTT | Linha 4: `WiFi:OK  MQTT:OK` ou `OFF` |
| Envio por MQTT | Publica a média de 20 medições a cada 20 s no canal de medições |
| Persistência | O ThingSpeak guarda o histórico do canal (exportável em CSV e JSON) |
| Recepção de configuração e acionamento remoto | Assina o canal de comandos: novo limite de alerta e sirene remota |
| Dashboard | Feito na disciplina de Front-end: página inicial do site (`web/`), com a opção "Sensor ao vivo" |

## Diferenciais do dispositivo

| Recurso | Para que serve |
|---|---|
| Filtro de mediana (5 medições) | Uma leitura errada isolada, como um galho passando sob o sensor, não dispara a sirene |
| Histerese de 10 cm | O risco sobe na hora, mas só desce quando a água baixa 10 cm além do limite; a sirene não fica ligando e desligando |
| Barra de nível no display | Mostra o nível de relance; o `\|` marca o transbordamento |
| Sirene independente da rede | A decisão é tomada no ESP32; sem WiFi, o aviso local continua |

Para demonstrar a histerese no vídeo: suba o nível até 245 cm (emergência) e desça para 235 cm. O display continua em emergência. Só abaixo de 230 cm ele volta para alerta.

## Plataforma IoT: ThingSpeak

Broker `mqtt3.thingspeak.com`, porta 1883.

### Canal 1 - AlertaCheia Medições (o ESP32 publica)

Tópico: `channels/<ID do canal 1>/publish`

| Campo | Conteúdo | Unidade |
|---|---|---|
| field1 | Nível médio da água | cm |
| field2 | Risco: 0 normal, 1 atenção, 2 alerta, 3 emergência | - |
| field3 | Sirene: 0 desligada, 1 ligada | - |
| field4 | Limite de alerta em uso | cm |

### Canal 2 - AlertaCheia Comandos (o ESP32 escuta)

Tópicos: `channels/<ID do canal 2>/subscribe/fields/field1` e `.../field2`

| Campo | Conteúdo | Valores aceitos |
|---|---|---|
| field1 | Novo limite de alerta | entre 121 e 239 cm |
| field2 | Sirene remota | 1 liga, 0 desliga |

Dois canais são usados porque o ThingSpeak gratuito aceita uma atualização a cada 15 s por canal; com os comandos em outro canal, eles não disputam espaço com as medições.

### Dispositivo MQTT

Em *Devices > MQTT > Add a new device*: autorize o canal 1 para **publicar** e o canal 2 para **assinar**. Copie Client ID, Username e Password para o início do `sketch.ino`.

## Passo a passo

1. Crie uma conta em https://thingspeak.com e os dois canais, com os campos das tabelas.
2. Crie o dispositivo MQTT e copie as três credenciais.
3. Em https://wokwi.com, crie um projeto ESP32 e cole `sketch.ino`, `diagram.json` e `libraries.txt`.
4. Preencha no sketch: `MQTT_CLIENT_ID`, `MQTT_USUARIO`, `MQTT_SENHA`, `CANAL_MEDICOES` e `CANAL_COMANDOS`.
5. Inicie a simulação. Em alguns segundos o display mostra `WiFi:OK  MQTT:OK`.

Para enviar comandos pelo navegador (troque a chave pela *Write API Key* do canal 2):

```
https://api.thingspeak.com/update?api_key=CHAVE_DO_CANAL_2&field1=150   (novo limite de alerta)
https://api.thingspeak.com/update?api_key=CHAVE_DO_CANAL_2&field2=1     (liga a sirene)
https://api.thingspeak.com/update?api_key=CHAVE_DO_CANAL_2&field2=0     (desliga a sirene)
```

## Como usar a tela local

```
AlertaCheia Sarandi
181 cm    ██████--|-
ALERTA     Sir:DESL
WiFi:OK   MQTT:OK
```

- **Mudar o nível:** o valor leva até 3 segundos para mudar no display, por causa do filtro de mediana. Clique no sensor HC-SR04 e mova o controle de distância. Distância 320 cm = nível 80 cm (normal); 250 = 150 cm (atenção); 210 = 190 cm (alerta); 150 = 250 cm (emergência, sirene toca).
- **LEDs:** verde normal; amarelo atenção; amarelo e vermelho alerta; vermelho emergência.
- **Botão azul:** cada toque muda o modo de teste. 1º toque: MQTT desconectado (`MQTT:OFF`). 2º toque: WiFi desconectado (`WiFi:OFF MQTT:OFF`). 3º toque: reconecta. Mesmo desconectado, o dispositivo continua medindo e a sirene continua funcionando.

## Roteiro do vídeo (gravação da tela)

1. Simulação iniciando e conectando (display com `OK` nos dois).
2. Mover o sensor de normal até emergência: display, LEDs e sirene mudando.
3. Abrir o canal 1 no ThingSpeak e o site da equipe mostrando o novo valor.
4. Enviar novo limite de alerta e ligar a sirene pelo canal 2; mostrar o Serial Monitor e o display.
5. Apertar o botão: mostrar `MQTT:OFF`, depois `WiFi:OFF`, depois a reconexão.

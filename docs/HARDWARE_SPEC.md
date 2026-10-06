# Hardware — ESP32-S3 Super Mini OEM

Configuração consolidada em 2026-10-05 com a pinagem e as decisões informadas
pelo usuário. A montagem Teensy está preservada em
[legacy/teensy40/SNAPSHOT.md](../legacy/teensy40/SNAPSHOT.md).

## Placa e memórias

- Placa: OEM ESP32-S3 Super Mini, chip **ESP32-S3FH4R2**.
- CPU: dois núcleos Xtensa LX7, até 240 MHz, instruções SIMD.
- SRAM interna: 512 KiB; flash: **4 MiB Quad SPI**; PSRAM: **2 MiB Quad SPI**.
- GPIOs: lógica de **3,3 V**. Alimentação da placa: USB-C em **5 V**.
- USB nativa: GPIO19/D− e GPIO20/D+, reservados para o conector USB.
- LED RGB WS2812: GPIO48, conforme descrição da placa. É endereçável;
  `digitalWrite(LED_BUILTIN, ...)` do Teensy não serve como driver desse LED.
- Wi-Fi e BLE disponíveis no chip; não ativados no firmware de preparação.

Memórias/CPU confirmadas no [datasheet Espressif](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf).
O [anúncio informado](https://www.mercadolivre.com.br/p/MLB2097816970?pdp_filters=item_id:MLB7578990586)
identifica a placa OEM. A ferramenta de pesquisa não conseguiu abrir o anúncio;
detalhes de layout, antena e circuito de carga seguem a descrição fornecida pelo
usuário e ainda não foram confrontados com esquema elétrico do fabricante.

## Pinagem externa confirmada

Os números abaixo são **GPIOs do ESP32**, não posições físicas no conector.

| Barramento/dispositivo | Sinal | GPIO |
|---|---|---:|
| I2C → PCA9548A | SDA | 1 |
| I2C → PCA9548A | SCL | 2 |
| SPI → microSD | SCK | 7 |
| SPI → microSD | MISO | 6 |
| SPI → microSD | MOSI | 5 |
| SPI → microSD | CS | 4 |

PCA9548A e sensores continuam na distribuição de 3V3/GND, com terra comum
com ESP32 e SD. I2C configurado para 400 kHz. O CS do cartão é colocado em
nível alto antes da inicialização do SPI. GPIO19/20 não serão usados como I2C.
RESET do PCA9548A permanece sem controle por GPIO nesta etapa.

## Multiplexador e sensores

O **PCA9548A permanece**: os dispositivos são acessados após seleção de canal,
evitando conflitos entre sensores com endereços iguais. Endereço do mux: `0x70`.

| Identidade lógica | Canal PCA9548A | Endereço I2C candidato |
|---|---:|---|
| ICM-20948 #0 | 4 | `0x69`, depois `0x68` |
| ICM-20948 #1 | 5 | `0x69`, depois `0x68` |
| ICM-20948 #2 | 6 | `0x69`, depois `0x68` |
| BMP390 #0 | 7 | `0x77`, depois `0x76` |
| BMP390 #1 | 3 | `0x77`, depois `0x76` |

Esse é o mapa da última montagem Teensy, confirmado pelo usuário para a nova
montagem. A `main` continha o mapa anterior; `constants.h` foi alinhado ao mapa
confirmado. Não alterar a identidade lógica dos sensores na leitura dos dados.

Breakouts mantidos: 3× Adafruit ICM-20948 STEMMA QT, 2× Adafruit BMP390
STEMMA QT e Adafruit PCA9548A STEMMA QT. O pacote de sensores mantém três
acelerômetros, três giroscópios, três AK09916 e dois pares pressão/temperatura.

## Alimentação consolidada

Arquitetura confirmada:

```text
Bateria de lítio → conversor regulado para 5 V → USB-C do ESP32
                                                ↓ regulador da placa
                                              3V3/GND
                                                ↓
                                       PCA9548A, sensores e módulo SD
```

O microSD será alimentado pelo próprio ESP32, conforme informado. Módulo
identificado pelo usuário como **"Modulo Adaptador Cartão Micro Sd Tf Card
Arduino Esp32 Pic"**, no
[anúncio Mercado Livre MLBU2296492711 / MLB3617781629](https://www.mercadolivre.com.br/modulo-adaptador-cartao-micro-sd-tf-card-arduino-esp32-pic/up/MLBU2296492711?pdp_filters=item_id%3AMLB3617781629).

A consulta ao anúncio retornou **403 Forbidden** em 2026-10-05. O nome é uma
denominação comercial genérica, sem identificação confirmada do fabricante,
revisão da placa, regulador ou conversor de nível. Posteriormente, o usuário
**confirmou VCC do módulo em 3,3 V, alimentado pela saída 3V3 do ESP32**, e
informou que as conexões de hardware estão concluídas. A pinagem SPI
CS GPIO4, MOSI GPIO5, MISO GPIO6 e SCK GPIO7 permanece confirmada, com terra comum. A tensão deixou de ser
uma dúvida de montagem; estabilidade durante escrita e picos de corrente
ainda dependem do ensaio físico. O ambiente `esp32s3_sd_test` usa essa montagem,
sem acionar sensores, áudio ou o LED RGB; roteiro em [SD_TEST.md](SD_TEST.md).

A célula não será ligada diretamente à USB-C. Pads BATTERY+/BATTERY− não
fazem parte do caminho confirmado de alimentação; não presumir que o carregador
da placa recarregará a bateria externa pelo conversor de 5 V. Proteção,
carregamento da bateria externa, chave e capacidade/corrente do conversor
continuam pendentes de especificação.

Descrição fornecida da placa: pads para célula de lítio, carga de 100 mA e
300 mA com BOOST fechado para baterias acima de 500 mAh. Esses valores são
informações do anúncio, sem validação elétrica independente. O jumper não será
alterado pelo software. A referência antiga de bateria 400 mAh pertence à
montagem Teensy e não confirma a bateria desta montagem.

## USB e montagem

O perfil inicial usa USB nativa Serial/JTAG CDC (`ARDUINO_USB_MODE=1`), com
CDC no boot; não usa adaptador USB-UART externo. HID/OTG poderá exigir outra
configuração USB em etapa posterior. Os botões BOOT/RESET permitem entrada
manual no bootloader se o primeiro upload não funcionar automaticamente.

A descrição OEM informa pads adicionais e furos castelados. A presença de um
pad não garante disponibilidade de todos os GPIOs: conferir restrições de
memória e strapping no datasheet antes de expandir a pinagem. A lista de pads
do anúncio não foi validada contra um esquema do fabricante.

Para bancada, usar cabo USB de dados. Quando a USB-C estiver ocupada pelo
conversor da bateria, definir como reconectar ao computador e evitar conectar
duas fontes de 5 V sem conhecer o circuito de seleção de alimentação.

## Áudio

Áudio permanece fora do build ESP32 de preparação. Não há pinagem I2S atribuída
para a nova montagem. Histórico ICS43434 e roteiro de retomada em
[ESP32_MIGRATION.md](ESP32_MIGRATION.md); não copiar os pinos 21/20/8 do Teensy.

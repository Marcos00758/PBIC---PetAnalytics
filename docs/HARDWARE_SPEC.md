# Hardware
Teensy 4.0

Todo o projeto trabalha em 3,3 V.

A Teensy substitui completamente a antiga Nicla Voice.

Não existem mais:

NDP120
BMI270 interno
BMM150 interno
microfone interno

Todo o código exclusivo do Nicla deverá ser removido e não implementado.

## Pinagem
pinos - portas

- PCA9548A:
Teensy - PCA9548A
18	- SDA
19	- SCL
3V3	- VCC
GND	- GND

RESET do PCA9548A não será utilizado.

- Cartão SD
Teensy - SD
11 - MOSI
12 - MISO
13 - SCK
10 - CS
3V3 - VCC
GND - GND

- Microfone ICS43434
Teensy - Microfone
21 - BCLK
20 - LRCLK (WS)
8 - DOUT
GND - SEL
3V3 - 3V3
GND - GND

SEL ficará permanentemente em GND (canal esquerdo).

- Sensores conectados ao PCA9548A:
Canal 0: ICM20948 #0

Canal 1: ICM20948 #1

Canal 2: BMP390 #0

Canal 3: BMP390 #1

Canal 4: ICM20948 #2


- Alimentação

Existe uma bateria LiPo
3,7 V
400 mAh
modelo DTP502535
com circuito de proteção (PCM).

Arquitetura:

Bateria;
Chave liga/desliga;
VIN da Teensy

- Placa distribuidora

Existe apenas uma pequena placa distribuidora para alimentação.

Ela distribui somente:
3V3
GND

Nenhum sinal digital passa por ela, todos os sinais vão diretamente da Teensy ao dispositivo.

## Modelos dos breakouts

- 3x ICM-20948: Adafruit STEMMA QT, endereço padrão `0x69` e alternativo `0x68`.
- 2x BMP390: Adafruit STEMMA QT.
- Multiplexador: Adafruit PCA9548A STEMMA QT, endereço padrão `0x70`.
- Microfone: Adafruit ICS43434.

O firmware deve acessar cada sensor somente depois de selecionar seu canal no
PCA9548A. O diagnóstico testa primeiro `0x69`, usado pelo hardware anterior, e
depois `0x68`. Para confirmar que o dispositivo é um ICM-20948, o registrador
`WHO_AM_I` deve retornar `0xEA`.

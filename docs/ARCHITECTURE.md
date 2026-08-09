# Arquitetura

## Hardware
Microcontrolador:
Teensy 4.0

Componentes conectados:
- I2C: Um único barramento I2C conecta a Teensy ao multiplexador.

Multiplexador: PCA9548A

Sensores conectados ao PCA9548A:
- Canal 0: ICM20948 #0

- Canal 1: ICM20948 #1

- Canal 2: BMP390 #0

- Canal 3: BMP390 #1

- Canal 4: ICM20948 #2


Todos utilizam 3,3 V.

SPI: Um único módulo microSD.

I2S: Um microfone digital ICS43434

## Software
Mudança importante em relação ao projeto antigo

O firmware antigo da Nicla utilizava:

NDP.begin()
NDP.load()
Nicla_System
BMI270 interno
BMM150 interno

Nada disso deverá existir no novo firmware.

Toda a arquitetura deve ser reescrita considerando exclusivamente a Teensy 4.0

- Organização do código 

PBIC/
│
├── src/
│   ├── main.cpp
│   │
│   ├── drivers/
│   │   ├── pca9548a.cpp
│   │   ├── pca9548a.h
│   │   ├── icm20948.cpp
│   │   ├── icm20948.h
│   │   ├── bmp390.cpp
│   │   ├── bmp390.h
│   │   ├── microphone.cpp
│   │   ├── microphone.h
│   │   ├── sd_logger.cpp
│   │   └── sd_logger.h
│   │
│   ├── utils/
│   │   ├── crc.cpp
│   │   ├── crc.h
│   │   ├── packet.cpp
│   │   └── packet.h
│   │
│   └── config/
│       ├── pins.h
│       └── constants.h
│
├── python/
│   └── parse_data.py
│
├── docs/
│   ├── ARCHITECTURE.md
│   ├── HARDWARE_SPEC.md
│   └── FIRMWARE_GUIDELINES.md
│
└── README.md

- Estrutura do cartão SD
/session.txt

/S001/
├── imu.bin
├── audio.raw
├── meta.txt
└── status.txt

/S002/
├── imu.bin
├── audio.raw
├── meta.txt
└── status.txt

session.txt: Fica na raiz do cartão e guarda o número da última sessão
imu.bin: Contém os pacotes binários das IMUs e barômetros.
audio.raw: Contém apenas amostras do microfone
meta.txt: Descreve a sessão e a configuração.

"Exemplo:

session=1
firmware_version=0.1.0
board=Teensy 4.0

imu_sample_rate_hz=100
bmp_sample_rate_hz=25

audio_enabled=true
audio_sample_rate_hz=16000
audio_channels=1
audio_bits_per_sample=16

imu_packet_size=0
imu_magic=0xAA55
crc=CRC-8 polynomial 0x07

icm0_channel=0
icm1_channel=1
bmp0_channel=2
bmp1_channel=3
icm2_channel=4

status.txt: Registra se os componentes inicializaram corretamente e se a gravação funcionou 

## Aquisição inicial dos ICM-20948

O firmware usa `Adafruit ICM20X 2.0.7` para os breakouts ICM-20948 e um driver
próprio baseado em `Wire` para o PCA9548A. Os canais ICM são `0`, `1` e `4`.

Configuração inicial de bancada:

- acelerômetro: faixa de `+/-2 g`, divisor 10, ODR aproximado de 102,3 Hz;
- giroscópio: faixa de `+/-250 graus/s`, divisor 10, ODR de 100 Hz;
- filtros digitais passa-baixas desativados;
- magnetômetro AK09916 configurado a 20 Hz apenas para diagnóstico no boot;
- rodada de leitura dos três ICMs a 100 Hz, agendada com `micros()`;
- saída USB binária não bloqueante em pacotes de 45 bytes.

A biblioteca Adafruit inicializa o AK09916 durante `begin_I2C()`. O driver o
desliga logo depois da inicialização porque o magnetômetro não faz parte desta
etapa. As faixas de `+/-2 g` e `+/-250 graus/s` priorizam resolução no teste de
bancada e devem ser revistas após testes de saturação com movimento real.

Depois de selecionar um canal, o driver aguarda 80 microssegundos antes de
acessar o sensor. Esse tempo veio da experiência com o hardware legado e ainda
precisa ser validado na montagem Teensy 4.0 + breakouts Adafruit.

O driver lê diretamente o bloco de 12 bytes de acelerômetro e giroscópio a
partir do registrador `0x2D`, em big-endian. A biblioteca Adafruit continua
responsável pela inicialização e configuração do ICM. A leitura direta permite
validar os retornos de `Wire`, transmitir os valores `int16` sem conversão no
microcontrolador e contabilizar falhas I2C por sensor.

O serviço `src/services/imu_acquisition` contém o agendador anti-rajada,
sequência e contadores. `src/data/imu_packet.h` define o pacote, enquanto
`src/utils/packet` e `src/utils/crc8` fazem sua montagem e validação. O contrato
com o Python está documentado em `docs/DATA_FORMAT.md`.

## Diagnóstico inicial dos AK09916

A inicialização oficial da `Adafruit ICM20X 2.0.7` configura o controlador I2C
auxiliar de cada ICM-20948, valida o AK09916 pelo registrador `WIA2=0x09` e
mantém um proxy de nove bytes de `ST1` a `ST2`. O driver confirma novamente o
`WIA2` por uma transação de um byte via `I2C_SLV4`, configura o magnetômetro a
20 Hz e lê no boot os três eixos crus little-endian, `ST1` e `ST2`.

O diagnóstico informa `DRDY` e overflow, mas os magnetômetros ainda não fazem
parte do serviço de aquisição nem do pacote binário. O formato permanece com
45 bytes e a aquisição principal continua em 100 Hz.

## Aquisição inicial dos BMP390

Os dois BMP390 permanecem definidos nos canais 2 e 3 do PCA9548A. Durante o
`setup`, o firmware testa os endereços `0x77` e `0x76` e valida o `CHIP_ID`
esperado `0x60` por meio da biblioteca `Adafruit BMP3XX 2.1.6`.

Os canais 2 e 3, ambos no endereço `0x77`, foram confirmados na montagem física.
Após o diagnóstico, o driver coloca os BMP390 em modo normal contínuo a 25 Hz.
O serviço de aquisição lê diretamente os seis bytes crus de pressão e
temperatura, em little-endian, a cada quatro rodadas das IMUs e mantém o último
valor válido de cada sensor em cache. Falhas BMP possuem contadores próprios e
não descartam a rodada das IMUs.

A API pública `Adafruit BMP3XX 2.1.6` fornece somente valores compensados e não
expõe as contagens Bosch cruas. Por isso, a biblioteca permanece responsável
pela inicialização e validação, enquanto a aquisição contínua usa os
registradores oficiais `PWR_CTRL` (`0x1B`), `OSR` (`0x1C`), `ODR` (`0x1D`) e o
bloco de dados `0x04` a `0x09`. Não há `float` nesse caminho de aquisição.

O cache ainda não participa do pacote binário de 45 bytes. Portanto,
`docs/DATA_FORMAT.md` e o parser Python permanecem inalterados até a próxima
mudança formal de formato.

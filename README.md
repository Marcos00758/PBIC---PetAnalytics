# Pet Analytics Firmware

## Visão Geral

Este repositório contém o firmware do wearable **Pet Analytics**
desenvolvido para pesquisa PBIC. O foco desta etapa é coleta robusta de
dados, gravação em cartão SD e posterior processamento em Python/IA. Não
há inferência embarcada nesta fase.
Objetivo

Este projeto é um wearable para cães destinado à coleta de dados para pesquisa.
Nesta etapa não existe classificação em tempo real. O objetivo é coletar dados
sincronizados dos sensores em Arduino/C++ na Teensy 4.0 e interpretá-los em
Python. O firmware grava de forma autônoma no cartão SD. O stream binário USB
é opcional para diagnóstico de bancada e fica desabilitado por padrão durante
a etapa de gravação no SD.
## Hardware

-   Teensy 4.0
-   PCA9548A
-   3×ICM-20948
-   2×BMP390
-   ICS43434
-   microSD
-   LiPo 3.7V

## Estado atual

Os três ICM-20948 nos canais 0, 1 e 4 do PCA9548A são lidos a 100 Hz. O firmware
transmite pela USB pacotes binários de 79 bytes com timestamp, sequência, 27
valores crus `int16` dos ICMs, quatro valores `uint32` crus dos BMP390 e CRC-8.
O mesmo fluxo é gravado em `/Sxxx/imu.bin`; `meta.txt` preserva a configuração
e os 21 bytes NVM individuais de cada BMP390, enquanto `status.txt` registra
contadores da aquisição e do SD.
O formato completo está em
`docs/DATA_FORMAT.md`.

## Sessões no cartão SD

Com um cartão FAT ou exFAT conectado nos pinos documentados, cada boot cria uma
nova pasta `/Sxxx`. O firmware não espera a USB e continua adquirindo sem
computador. O arquivo `imu.bin` permanece aberto, recebe escritas em blocos a
partir de um buffer RAM e faz flush periódico.

Se o SD parar de aceitar escritas por uma janela completa, o firmware emite
`SD_ERROR_CONFIRMED`, desativa a gravação até o reboot e, após cinco segundos,
pisca duas vezes o LED laranja integrado. O LED compartilha o pino do clock SPI
e só é controlado depois que o SPI foi encerrado com segurança.

Depois de desligar a Teensy e remover o cartão, analise a sessão diretamente:

```powershell
python python/parse_data.py E:/S001/imu.bin
python python/analyze_imu.py E:/S001/imu.bin --no-show
```

Quando `meta.txt` acompanha `imu.bin`, o analisador aplica automaticamente a
compensação Bosch e gera o gráfico BMP em Pa e graus Celsius.

Para uma captura USB de bancada, altere temporariamente
`kUsbBinaryStreamEnabled` para `true` em `src/config/constants.h`, recompile e
use `python/capture_serial.py`. Com o valor padrão `false`, o monitor serial
permanece textual e a aquisição continua sendo gravada somente no SD.

## Captura de bancada

Instale a única dependência Python desta etapa:

```powershell
python -m pip install -r python/requirements.txt
```

Com a Teensy programada e conectada, a porta é detectada automaticamente e uma
janela exata de 10 segundos do relógio do sensor é salva em `data/`:

```powershell
python python/capture_serial.py
```

Também é possível informar a porta e o arquivo:

```powershell
python python/capture_serial.py --port COM3 --output data/teste_icm.bin
python python/parse_data.py data/teste_icm.bin
```

Para validar CRC, sequencia, frequencia, periodo, jitter e perdas, e gerar o
graficos separados para acelerometro/giroscopio, magnetometros e BMP390 crus:

```powershell
python python/analyze_imu.py data/teste_icm.bin
```

O grafico e salvo ao lado da captura como `.png` e tambem e aberto na tela. Em
ambientes sem interface grafica, use `--no-show`; `--output` permite escolher o
caminho da imagem.

Para estimar uma calibracao magnetica inicial depois de uma rotacao 3D ampla:

```powershell
python python/calibrate_magnetometer.py data/rotacao_3d.bin
```

## Diagnostico do microfone

Na branch de audio, `kMicrophoneDiagnosticEnabled=true` inicia somente o
ICS43434 em RAM. SD, PCA9548A e sensores nao sao inicializados nesse modo. O
monitor serial informa, a cada dois segundos, taxa efetiva, perdas da fila,
uso de memoria, DC, RMS, clipping e atividade dos dois canais.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload --target monitor --upload-port COM3
```

O teste deve incluir alguns segundos em silencio, fala em nivel normal e sons
fortes sem encostar no microfone. O canal esperado e o esquerdo porque `SEL`
esta ligado ao GND.

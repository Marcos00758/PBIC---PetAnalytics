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
│   │   └── microphone.h
│   │
│   ├── services/
│   │   ├── imu_acquisition.cpp
│   │   ├── imu_acquisition.h
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

## Gravação no cartão SD

O serviço `src/services/sd_logger` usa a biblioteca `SD` fornecida pelo core da
Teensy, que por sua vez utiliza SdFat. Nenhuma dependência externa foi
adicionada ao `platformio.ini`. O cartão é inicializado antes do barramento I2C
com `CS=10` e passa por um teste curto de escrita e leitura.

O firmware não espera uma conexão USB durante o boot. O stream binário USB é
controlado por `kUsbBinaryStreamEnabled` e fica desabilitado por padrão na fase
de gravação autônoma, mantendo o monitor serial legível. Ele pode ser reativado
em builds de bancada. Se o SD estiver ausente ou falhar, o diagnóstico textual
continua disponível, mas não há persistência dos pacotes enquanto o stream
binário permanecer desabilitado. Com SD válido, o contador persistente
`/session.txt` escolhe uma pasta nova `/Sxxx`, e `imu.bin` permanece aberto
durante a sessão.

Os pacotes v4 de 79 bytes entram em uma fila circular de 8192 bytes. O logger
escreve `imu.bin` e `audio.raw` em blocos completos de 512 bytes durante a
gravacao normal; blocos parciais sao permitidos somente ao encerrar uma sessao.
Em cada passagem do loop ocorre no maximo uma operacao de SD. Quando as duas
filas estao prontas, o logger escolhe a proporcionalmente mais cheia e forca a
prioridade do audio quando sua fila atinge 50%. O SPI opera a 12 MHz.

Flush nao drena mais as filas. `imu.bin` e `audio.raw` possuem estados de flush
independentes, executados em passagens diferentes do loop quando nao ha bloco
pronto para escrita. O journal publica somente os bytes confirmados pelo ultimo
`sync()` bem-sucedido de cada arquivo. Isso evita as escritas repetidas de 79
bytes que anteriormente surgiam depois do primeiro flush.

A recuperação é medida separadamente por arquivo. Uma escrita sem nenhum byte
de progresso inicia um período de dois segundos; qualquer escrita posterior
com progresso cancela esse período. Somente dois segundos completos sem
progresso confirmam a falha. Isso evita que uma única falha após uma operação
longa seja interpretada como uma janela inteira de cartão indisponível.

Uma falha definitiva emite `SD_ERROR_CONFIRMED` uma única vez pela Serial e
desativa a gravação até o próximo reboot. Cinco segundos depois, o firmware
mantém `CS` alto, encerra o SPI e passa a usar o LED laranja integrado para dois
pulsos curtos repetidos. O LED compartilha o pino 13 com `SCK` e nunca é
controlado enquanto o SPI do SD estiver ativo. Não há tentativa de reinserção
automática do cartão.

O logger mede separadamente a maior duração de escrita, flush e atualização de
status, além de contar operações de cada tipo com duração igual ou superior a
10 ms. Esses valores permitem localizar a origem dos deadlines perdidos sem
alterar o pacote dos sensores. A remoção física ainda precisa ser validada no
hardware real.

Durante o setup, cada BMP390 fornece os 21 bytes dos registradores NVM `0x31` a
`0x45`. Eles são gravados em hexadecimal no `meta.txt`; o Python faz a
compensação Bosch, sem uso de `float` no caminho de aquisição do firmware.

## Aquisição inicial dos ICM-20948

O firmware usa `Adafruit ICM20X 2.0.7` para os breakouts ICM-20948 e um driver
próprio baseado em `Wire` para o PCA9548A. Os canais ICM são `0`, `1` e `4`.

Configuração inicial de bancada:

- acelerômetro: faixa de `+/-8 g`, divisor 10, ODR aproximado de 102,3 Hz;
- giroscópio: faixa de `+/-2000 graus/s`, divisor 10, ODR de 100 Hz;
- filtros digitais passa-baixas desativados;
- magnetômetro AK09916 configurado e adquirido a 20 Hz com cache;
- rodada de leitura dos três ICMs a 100 Hz, agendada com `micros()`;
- saída USB binária não bloqueante em pacotes v4 de 79 bytes.

A biblioteca Adafruit inicializa o AK09916 durante `begin_I2C()`. O driver o
mantém em modo contínuo a 20 Hz. As faixas foram ampliadas para `+/-8 g` e
`+/-2000 graus/s` depois que movimentos fortes saturaram as configurações de
bancada anteriores. A nova configuração ainda deve ser validada no uso real.

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

O serviço de aquisição consulta o proxy de cada magnetômetro a 25 Hz, em fases
diferentes, mantendo os últimos eixos crus válidos









em cache. Como a leitura
automática do proxy inclui `ST2` e pode limpar `DRDY` antes da consulta da
Teensy, uma amostra é considerada nova quando `DRDY` foi capturado ou quando o
trio cru mudou em relação ao cache. Leituras com overflow são rejeitadas.

Existem contadores separados por magnetômetro para falha I2C, atualização
aceita, ausência de novidade, overrun (`DOR`) e overflow (`HOFL`). Os nove
valores crus entram no pacote v4. As consultas dos três
AK09916 são distribuídas em fases diferentes das rodadas de 100 Hz, enquanto o
último valor válido de cada sensor é repetido a partir do cache.

## Aquisição inicial dos BMP390

Os dois BMP390 permanecem definidos nos canais 2 e 3 do PCA9548A. Durante o
`setup`, o firmware testa os endereços `0x77` e `0x76` e valida o `CHIP_ID`
esperado `0x60` por meio da biblioteca `Adafruit BMP3XX 2.1.6`.

Os canais 2 e 3, ambos no endereço `0x77`, foram confirmados na montagem física.
Após o diagnóstico, o driver coloca os BMP390 em modo normal contínuo a 25 Hz.
O serviço de aquisição lê diretamente os seis bytes crus de pressão e
temperatura, em little-endian, a cada quatro rodadas das IMUs, com uma fase por
BMP, e mantém o último
valor válido de cada sensor em cache. Falhas BMP possuem contadores próprios e
não descartam a rodada das IMUs.

A API pública `Adafruit BMP3XX 2.1.6` fornece somente valores compensados e não
expõe as contagens Bosch cruas. Por isso, a biblioteca permanece responsável
pela inicialização e validação, enquanto a aquisição contínua usa os
registradores oficiais `PWR_CTRL` (`0x1B`), `OSR` (`0x1C`), `ODR` (`0x1D`) e o
bloco de dados `0x04` a `0x09`. Não há `float` nesse caminho de aquisição.

Os quatro valores crus em cache participam do pacote v4 como `uint32`, na ordem
pressão/temperatura do canal 2 e pressão/temperatura do canal 3. O pacote possui
79 bytes e o parser gera um gráfico separado com contagens cruas. A compensação
em Pa e graus Celsius depende dos coeficientes NVM individuais.
A etapa do cartão SD registrará esses coeficientes em `meta.txt`.

## Ferramentas Python para sessões longas

O parser de arquivo trabalha em blocos de 64 KiB, preservando busca de magic,
CRC e ressincronização entre blocos. O analisador calcula timing e diagnósticos
sobre toda a sessão, mas limita os pontos mantidos para gráficos. A ferramenta
`python/calibrate_magnetometer.py` produz uma estimativa inicial de hard-iron e
soft-iron diagonal somente quando a captura cobre rotação suficiente nos três
eixos.

## Diagnostico inicial do ICS43434

O diagnostico usa `AudioInputI2S` da Audio Library 1.3 fornecida pelo core
Teensyduino 1.62, sem dependencia externa no `platformio.ini`. A implementacao
oficial para Teensy 4.x fixa `BCLK=21`, `LRCLK=20` e `RX=8`; o breakout mantem
`SEL=GND`, portanto o sinal deve aparecer na porta esquerda. Fontes primarias:

- https://www.pjrc.com/teensy/gui/index.html?info=AudioInputI2S
- https://github.com/PaulStoffregen/Audio
- https://cdn-shop.adafruit.com/product-files/6049/6049_DS-000069-ICS-43434-v1.2.pdf

A biblioteca trabalha a 44100 Hz, em blocos de 128 amostras `int16`. O SAI
recebe slots I2S de 32 bits, mas o DMA oficial copia apenas os 16 bits mais
significativos de cada canal. Assim, os oito bits menos significativos da
amostra nativa de 24 bits do ICS43434 nao ficam disponiveis nesse caminho.

O modo `kMicrophoneDiagnosticEnabled=true` e isolado: nao inicializa SD, I2C ou
os demais sensores e nao cria arquivos. Um destino `AudioStream` proprio copia
os canais esquerdo e direito para uma fila circular em RAM com 16 blocos e
conta explicitamente overflow e blocos incompletos. O loop calcula taxa real,
DC, RMS AC, faixa, clipping, bits ocupados no container PCM16, atividade do LSB,
uso de memoria e CPU. A escolha entre armazenar PCM16 ou implementar um caminho
DMA de 24/32 bits sera feita depois dos resultados do hardware.

O diagnostico em hardware confirmou cerca de 44100 amostras/s, sinal somente
no canal esquerdo, DC proximo de zero e nenhuma perda da fila em 40 segundos.
Sons usuais ocuparam ate 14 bits do PCM16; por isso o formato inicial de coleta
e PCM mono `int16` little-endian. Os 24 bits nativos continuam sendo uma opcao
futura, mas nao justificam neste momento substituir o DMA oficial e estavel.

## Captura e gravacao de audio

`src/services/audio_capture` usa `AudioInputI2S`, cujo recebimento no Teensy
4.0 e feito por DMA, e conecta apenas a porta esquerda a um `AudioStream`
proprio. O callback copia blocos de 128 amostras para uma fila circular mono e
conta blocos recebidos, overflow, blocos incompletos e maior ocupacao. O
processamento nao usa `float` nem grava no SD dentro da interrupcao.

O loop transfere os blocos para uma segunda fila de 32768 bytes pertencente ao
`sd_logger`. A fila de captura tem 511 posicoes uteis, aproximadamente 132 KiB;
juntas oferecem aproximadamente 1,85 s de reserva a 44100 Hz. O logger mantem
`imu.bin` e `audio.raw` abertos ao mesmo tempo e escreve o audio em blocos de
ate 512 bytes. Flush, escrita e falha do audio possuem contadores e latencias
separados em `status.txt`. A janela de saude acompanha sucesso de cada arquivo
separadamente, evitando que `imu.bin` mascare uma falha de `audio.raw` ou o
inverso.

Antes de abrir a sessao, a captura mede dois segundos de sinal em RAM com o
ambiente em silencio. Media DC, RMS, pico e clipping sao registrados em
`meta.txt`. Valores acima dos limites de silencio emitem
`AUDIO_PREFLIGHT_WARNING`, mas nao bloqueiam a captura: eles podem refletir
fala ou ruido ambiental no boot e o projeto precisa preservar o sinal cru.
Somente ausencia de blocos validos emite `AUDIO_CAPTURE_REJECTED`. O firmware
nao tenta corrigir saturacao com ganho ou filtro digital.

Cada bloco de audio recebe um timestamp apenas na fila RAM. O valor nao altera
o PCM em `audio.raw`, mas permite registrar no journal o primeiro bloco exato
de cada sessao rotacionada. `meta.txt` registra formato, taxa e configuracao;
`journal.txt` e a referencia mais atual para timestamp e tamanhos validos.

O logger usa `FsFile::preAllocate()` para reservar `imu.bin` e `audio.raw`.
No teste atual, cada pasta cobre cinco minutos mais um segundo de margem. Ao
atingir cinco minutos, o firmware deixa de aceitar novos blocos, drena as duas
filas, faz flush, trunca os arquivos, marca o journal como
`completed_duration` e cria automaticamente a proxima pasta. A prealocacao da
proxima pasta pode causar um pico isolado; sua duracao e registrada em
`sd_preallocation_duration_us`.

Cada callback DMA atribui uma sequencia ao bloco, inclusive quando nao ha bloco
valido disponivel. Se a fila de captura transbordar ou um callback ficar
incompleto, o logger detecta o salto e insere a quantidade equivalente de
blocos zerados antes do proximo bloco real. O PCM preserva duracao e
alinhamento, mas o silencio sintetico nao recupera o sinal perdido. Os totais,
eventos e maior gap ficam em `journal.txt` e `status.txt` e sao apresentados por
`python/export_audio.py`.

No primeiro boot, a prealocacao ocorre antes de ativar o I2S. Assim, o
preflight mede o microfone depois do maior pico inicial de atividade do SD.
Durante uma rotacao, o primeiro bloco real retirado da fila permanece pendente
ate que todo silencio necessario caiba no buffer do SD. O inicio logico da nova
sessao e o instante em que a anterior atingiu seu limite, nao o final da
prealocacao. Flush, truncamento, journal e status finais avancam como etapas
separadas, uma por passagem do loop. A prealocacao SdFat ainda e uma chamada
sincrona; a fila DMA cobre essa pausa e qualquer excesso e representado por
silencio, sem encurtar a linha do tempo do audio.

O journal e escrito apos o primeiro flush, por volta de dez segundos, e depois
a cada 3000 pacotes, aproximadamente 30 segundos. Uma sessao interrompida pode
manter a cauda fisica prealocada, mas `parse_data.py`, `analyze_imu.py` e
`export_audio.py` limitam a leitura aos tamanhos confirmados.

Capturas reais mostraram dois estados distintos do sinal. S010 ficou em cerca
de `-51,7 dBFS` RMS, sem ruido aparente; S013 iniciou e permaneceu saturado,
com `-13,3 dBFS` RMS e picos em escala completa. Como a saturacao ja existia
nos primeiros segundos, ela nao foi causada pela falha posterior do SD. O
firmware nao aplica ganho digital; alimentacao, GND, DOUT e sincronismo de boot
do breakout ainda precisam ser validados antes de aceitar uma sessao de audio.

O S017 confirmou uma segunda causa independente: o volume terminou com apenas
1024 bytes livres. O logger mede os clusters no boot e reserva 4 MiB. Antes de
cada rotacao, verifica se ha espaco para a proxima prealocacao completa; falta
de espaco e reportada antes de iniciar novos fluxos.

No teste S007, o preflight anterior a prealocacao estava limpo, mas a gravacao
degradou de aproximadamente `-65 dBFS` no primeiro meio segundo para ruido com
DC e clipping durante a atividade do SD. A sessao tambem descartou 18764
pacotes IMU porque ambas as filas ficaram cheias. A versao 0.4.1 altera a ordem
de boot e a prioridade das filas; se o novo preflight ou a gravacao continuarem
ruidosos, a causa restante e eletrica entre SD e I2S e exige validacao de
alimentacao, GND e roteamento dos fios.

## Diagnostico isolado de audio no SD

O servico `src/services/audio_sd_diagnostic` existe para separar o caminho
ICS43434/DMA/SD da aquisicao I2C. Com `kAudioSdDiagnosticEnabled=true`, `main`
retorna antes de inicializar `Wire`, PCA9548A, ICM-20948 e BMP390. O teste cria
uma pasta `/Mxxx`, prealoca uma unica janela de cinco minutos e nao rotaciona
arquivos. Assim, nenhuma prealocacao ocorre durante a captura.

O caminho isolado preserva PCM16 mono a 44100 Hz, blocos SD de 512 bytes,
sequencia DMA e zero-fill de gaps. Faz `sync()` a cada dez segundos e atualiza
o journal a cada trinta segundos. No limite de cinco minutos, desliga a origem
I2S, drena a fila, sincroniza, trunca e grava o status final. Esse modo e
temporario e mutuamente exclusivo com o diagnostico I2S somente em RAM.

S012 demonstrou que a continuidade de audio foi preservada por zero-fill:
300,008 s, 76 blocos perdidos em 50 eventos e maior gap de quatro blocos. No
entanto, a politica integrada priorizou audio em 51567 escritas e descartou
23551 pacotes IMU. Portanto, essa perda IMU ocorreu por starvation no
agendador integrado, nao por falta de espaco ou apenas pela prealocacao da
sessao seguinte. O teste `/Mxxx` deve determinar separadamente se ainda existem
perdas ou ruido sem qualquer transacao I2C.

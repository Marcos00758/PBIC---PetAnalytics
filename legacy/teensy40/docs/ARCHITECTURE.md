# Arquitetura

## Hardware
Microcontrolador:
Teensy 4.0

Componentes conectados:
- I2C: Um único barramento I2C conecta a Teensy ao multiplexador.

Multiplexador: PCA9548A

Sensores conectados ao PCA9548A:
- Canal 4: ICM20948 #0

- Canal 5: ICM20948 #1

- Canal 7: BMP390 #0

- Canal 3: BMP390 #1

- Canal 6: ICM20948 #2


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
├── meta.txt
├── journal.bin
└── status.txt

session.txt: Fica na raiz do cartão e guarda o número da última sessão
imu.bin: Contém os pacotes binários das IMUs e barômetros.
meta.txt: Descreve a sessão e a configuração.
journal.bin: Guarda checkpoints binarios append-only com CRC.
status.txt: Guarda contadores e latências do firmware.

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

icm0_channel=4
icm1_channel=5
bmp0_channel=7
bmp1_channel=3
icm2_channel=6

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
escreve somente `imu.bin`, em blocos completos de 512 bytes, e executa no
maximo uma operacao de SD por passagem do loop. O SPI opera a 12 MHz. O
microfone esta desabilitado na configuracao normal: I2S nao e inicializado,
`audio.raw` nao e criado e a arbitragem entre IMU e audio permanece apenas no
codigo experimental.

`main` consulta a aquisicao imediatamente antes de chamar o logger. Assim, uma
rodada ja vencida e executada antes de qualquer nova operacao SD. Uma chamada
sincrona ja iniciada no SdFat nao pode ser interrompida, portanto as latencias
maximas do cartao continuam sendo medidas.

Flush nao drena a fila. `imu.bin` recebe `sync()` a cada 1000 pacotes, cerca de
dez segundos, somente quando ha no maximo 1024 bytes aguardando. O journal e
atualizado logo depois e publica apenas os bytes confirmados pelo ultimo
`sync()` bem-sucedido. `journal.bin` permanece aberto e recebe registros fixos
de 32 bytes por append, sem criar, remover ou renomear arquivos durante a
aquisicao. Um CRC-8 permite ao Python ignorar um ultimo registro incompleto ou
corrompido. `status.txt` registra as escolhas do caminho IMU e as operacoes de
manutencao.

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
próprio baseado em `Wire` para o PCA9548A. Os canais ICM são `4`, `5` e `6`.

Configuração inicial de bancada:

- acelerômetro: faixa de `+/-8 g`, divisor 10, ODR aproximado de 102,3 Hz;
- giroscópio: faixa de `+/-2000 graus/s`, divisor 10, ODR de 100 Hz;
- filtros digitais passa-baixas ativos, `kIcmDlpfEnabled`;
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

`src/utils/time_utils.h` centraliza a aritmetica modular de `micros()` e inclui
`static_assert` cobrindo deadlines antes e depois do retorno de `uint32` a
zero. As ferramentas Python acumulam deltas consecutivos para representar
sessoes que atravessam mais de um rollover de aproximadamente 71,6 minutos.

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

Os dois BMP390 permanecem definidos nos canais 7 e 3 do PCA9548A. Durante o
`setup`, o firmware testa os endereços `0x77` e `0x76` e valida o `CHIP_ID`
esperado `0x60` por meio da biblioteca `Adafruit BMP3XX 2.1.6`.

Os canais 7 e 3, ambos no endereço `0x77`, foram confirmados na montagem física.
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
pressão/temperatura do canal 7 e pressão/temperatura do canal 3. O pacote possui
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

## Captura e gravacao de audio experimental

Esta implementacao esta preservada para ensaios futuros, mas nao participa do
firmware normal IMU-only. Com `kMicrophoneRecordingEnabled=false`, o I2S nao e
inicializado e nenhum arquivo de audio e aberto ou prealocado em `/Sxxx`.

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

No firmware normal, `FsFile::preAllocate()` reserva inicialmente 4.747.900
bytes para `imu.bin`, equivalentes a dez minutos mais um segundo de margem. A
sessao nao termina ao atingir essa reserva: o mesmo arquivo cresce e permanece
aberto ate reboot ou falha confirmada do cartao. Depois da validacao, a reserva
planejada de quatro horas sera 113.767.900 bytes; essa mudanca exige apenas
alterar `kSdPreallocationSeconds`, sem criar rotacao automatica.

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
uma pasta `/Mxxx`, prealoca uma unica janela de dez minutos e nao rotaciona
arquivos. Assim, nenhuma prealocacao ocorre durante a captura.

O caminho isolado preserva PCM16 mono a 44100 Hz, sequencia DMA e zero-fill de
gaps. A mesma captura continua por duas fases de cinco minutos: 256 bytes por
escrita na primeira e 512 bytes na segunda. Nao ha flush, reinicio de I2S ou
prealocacao na transicao. Faz `sync()` a cada dez segundos e atualiza o journal
a cada trinta segundos. No limite de dez minutos, desliga a origem I2S, drena a
fila, sincroniza, trunca e grava o status final. Esse modo e temporario e
mutuamente exclusivo com o diagnostico I2S somente em RAM.

Cada fase possui contadores proprios de escritas, falhas, gaps, silencio
inserido, ocupacao do buffer e histograma de latencia nas faixas `<1`, `1-2`,
`2-5`, `5-10`, `10-20`, `20-50`, `50-100` e `>=100 ms`. O script
`python/analyze_sd_blocks.py` compara as fases, analisa RMS, picos e ocorrencias
proximas de `+/-16384` e `+/-32768`, e indica provisoriamente o menor bloco sem
falhas, gaps, escritas de pelo menos 100 ms ou assinatura PCM suspeita. O
resultado em hardware decide o bloco do logger integrado. O M002 rejeitou 1024
e 2048 bytes por corrupcao PCM, apesar de zero falhas e gaps no transporte.
O M004 validou audio inteligivel com 256 e 512 bytes. A fase de 512 bytes
reduziu pela metade as chamadas ao SD e teve menor latencia maxima; por isso o
logger integrado permanece em 512 bytes. Dois gaps isolados de um bloco nessa
fase continuam sendo tratados como contingencia.

S012 demonstrou que a continuidade de audio foi preservada por zero-fill:
300,008 s, 76 blocos perdidos em 50 eventos e maior gap de quatro blocos. No
entanto, a politica integrada priorizou audio em 51567 escritas e descartou
23551 pacotes IMU. Portanto, essa perda IMU ocorreu por starvation no
agendador integrado, nao por falta de espaco ou apenas pela prealocacao da
sessao seguinte. O teste `/Mxxx` deve determinar separadamente se ainda existem
perdas ou ruido sem qualquer transacao I2C.

## Modo de apresentacao

O servico `src/services/presentation_stream` existe para demonstracoes ao vivo
e para verificar rapidamente se os sensores estao funcionando. Com
`kPresentationStreamEnabled=true`, `main` desvia antes de qualquer chamada ao
`sd_logger`: o cartao nao e inicializado, nenhuma pasta e criada e a
apresentacao nao pode ser interrompida por falha do SD.

O servico recebe por referencia o PCA9548A, os tres ICM-20948, os dois BMP390 e
o proprio `ImuAcquisition` ja construidos em `main`. Assim o agendador
anti-rajada, a fase dos magnetometros, a fase dos BMP390 e o pacote v4 sao
exatamente os mesmos da gravacao no cartao. O modo nao duplica logica de
aquisicao e nao altera o formato de dados.

A transmissao usa a mesma guarda nao bloqueante do stream USB de bancada:
o pacote so e escrito quando `Serial.availableForWrite()` comporta os 79 bytes,
caso contrario o descarte e contabilizado em `usbDroppedPackets`. O loop
principal nao bloqueia.

Como abrir a porta serial na Teensy nao reinicia a placa, o computador pode se
conectar muito depois do boot e perder o banner inicial. Por isso o banner e
reemitido a cada `kPresentationBannerIntervalMs`, sempre entre pacotes
completos. Quando algum sensor falha na inicializacao, o servico nao transmite
pacotes, mas continua repetindo o banner com `streaming=0` e uma linha
`PRESENTATION_ERROR`, de modo que o operador recebe o diagnostico pela USB.

Os 21 bytes NVM de cada BMP390 sao lidos no boot e publicados no banner. Essa e
a unica fonte de coeficientes nesse modo, porque `meta.txt` pertence a sessao do
cartao. O contrato completo esta em `docs/DATA_FORMAT.md`.

## Verificacao dos sensores no computador

`python/presentation_monitor.py` consome o stream de apresentacao e emite um
veredito por sensor. O leitor separa o texto do banner dos pacotes binarios,
reaproveitando `decode_packet` de `python/parse_data.py` e a deteccao de porta
de `python/capture_serial.py`. A janela analisada e medida pelo relogio do
sensor, como em `capture_serial.py`, e nao pelo relogio do computador.

As verificacoes de gravidade e de giroscopio parado exigem o dispositivo em
repouso durante a janela. O acelerometro tambem e considerado travado quando os
tres eixos repetem exatamente o primeiro valor durante toda a janela, porque um
sensor real sempre apresenta ruido de alguns LSB.

O magnetometro e avaliado pela contagem de mudancas em relacao a ODR anunciada
no banner, nao pela taxa dos pacotes, porque o valor e repetido do cache entre
atualizacoes. O mesmo criterio vale para os BMP390.

## Painel grafico da apresentacao

`python/presentation_panel.py` reaproveita integralmente o leitor da etapa
anterior: `StreamDecoder`, `DeviceInfo`, `SerialSource` e `evaluate_all` vem de
`python/presentation_monitor.py`, e as escalas fisicas vem de
`python/parse_data.py`. O painel nao decodifica pacote nem interpreta banner por
conta propria, entao existe uma unica implementacao do contrato.

`SerialSource` ganhou um modo nao bloqueante. A verificacao de bancada pode
esperar por bytes, mas o painel precisa redesenhar em cadencia fixa e nunca
pode travar aguardando a serial.

`evaluate_icm` e `evaluate_all` ganharam o parametro `at_rest`. Os testes de
gravidade e de giroscopio parado so fazem sentido com o dispositivo imovel;
mante-los ativos durante a apresentacao produziria falha continua enquanto
alguem move a coleira. Com `at_rest=False` permanecem os testes que independem
de movimento: sensor travado, atualizacao do magnetometro, saturacao, CRC,
sequencia e taxa efetiva.

A calibracao inicial mede bias de giroscopio, vetor de gravidade e pressao de
referencia com o dispositivo parado. O bias e sempre subtraido, seguindo a
mesma pratica do firmware legado da Nicla, que calibrava os giroscopios no boot.
A gravidade e subtraida por padrao para centrar a grade em zero, e a pressao de
referencia define o zero da altitude relativa.

A altitude usa a relacao da atmosfera padrao internacional referenciada a
pressao de calibracao, portanto e sempre uma diferenca de altura e nunca uma
altitude absoluta.

O historico vive em um `SampleBuffer` de capacidade fixa sobre `numpy`, que
descarta a metade mais antiga quando enche em vez de realocar. Os valores sao
convertidos para unidades fisicas na entrada, uma vez por pacote, de modo que o
redesenho e apenas uma fatia do buffer. Uma reconversao a cada quadro nao
sustentaria vinte e sete series a 100 Hz.

`pyqtgraph` e o binding Qt sao importados dentro de `main`, com mensagem
propria quando faltam, do mesmo modo que `python/analyze_imu.py` importa
`matplotlib`. Assim a logica pura de calibracao, conversao e buffer permanece
importavel e testavel sem interface grafica.

## Reducao de ruido dos sensores

Ate a versao anterior os dois mecanismos de reducao de ruido do BMP390 estavam
desligados e os filtros passa-baixas dos ICM tambem. Ambos sao controlados por
flags em `src/config/constants.h` e podem voltar ao estado antigo trocando
`true` por `false`.

`kBmpOversamplingEnabled` escreve o registrador OSR `0x1C` com pressao em x8 e
temperatura em x2. Com essa configuracao a conversao leva cerca de 21 ms dos
40 ms disponiveis a 25 Hz. Pressao em x16 precisaria de cerca de 37 ms e nao
deixaria margem, por isso x8 e o limite pratico nessa taxa.

`kBmpIirFilterEnabled` escreve o registrador CONFIG `0x1F` com coeficiente 3,
cerca de quatro amostras de constante de tempo a 25 Hz. Esse filtro e interno
ao sensor e nao pode ser desfeito no dado gravado; e o unico ajuste desta lista
que perde informacao, e existe porque o ruido de pressao domina completamente a
altitude.

`kIcmDlpfEnabled` corrige um problema mais serio que ruido. Com o filtro em
bypass o acelerometro corre a 4500 Hz e o giroscopio a 9000 Hz, e os divisores
de taxa sao ignorados. Amostrando a 100 Hz, tudo acima de 50 Hz era rebatido
para dentro da banda util por aliasing, e as taxas de 102,3 Hz e 100 Hz
documentadas nao valiam. Com os filtros ativos os divisores passam a valer e a
banda fica abaixo de Nyquist.

Todas essas escolhas sao registradas em `meta.txt` e no banner de apresentacao,
de modo que o Python sabe com que configuracao cada captura foi feita.

## Filtragem no painel da apresentacao

O painel nao suaviza nada por conta propria sem dizer. Cada filtro e uma chave
com estado visivel no rodape e tecla para alternar: `B` para o barometro, `F`
para os IMU e `D` para as amostras repetidas.

A ordem importa. O barometro responde a 25 Hz e o magnetometro a 20 Hz, mas
ambos sao repetidos do cache em todos os pacotes de 100 Hz. As repeticoes sao
descartadas primeiro; media-las apenas alargaria a janela sem acrescentar
nenhuma leitura nova. A media e causal, com janela que expande no inicio, e
nunca olha para o futuro.

A amostra mais recente sempre sobrevive a decimacao. Um barometro perfeitamente
estavel colapsaria para um unico ponto antigo e a curva pararia antes da borda
direita, o que se leria como sensor morto em vez de sensor quieto.

A pressao e plotada como desvio da referencia de calibracao, em pascal. Os dois
barometros ficam separados por cerca de dez pascal so por calibracao de
fabrica; plotar o valor absoluto gastava o eixo inteiro nessa diferenca
constante em vez da variacao que interessa.

Cada painel tem um piso de escala vertical, aplicado por `setLimits(minYRange)`.
Sem ele a autoescala de um sensor parado amplia o proprio ruido ate encher o
painel, o que fazia um repouso normal parecer defeito.

## Escala, gravidade e altitude no painel

Tres correcoes vieram de olhar o painel rodando com o hardware real.

A escala vertical de cada painel e controlada por `AxisScaler` e nao mais pela
autoescala do pyqtgraph. Ela cresce imediatamente, para nunca cortar um pico, e
encolhe devagar. A autoescala simetrica fazia um unico pico esticar o eixo e
comprimir todo o resto, e o eixo ficava respirando a cada movimento.

A gravidade deixou de ser um vetor fixo capturado na calibracao. Um vetor fixo
so vale enquanto a orientacao nao muda, e numa apresentacao os sensores sao
levantados e girados o tempo todo; qualquer reposicionamento deixava um degrau
permanente no tracado. Agora um estimador exponencial de constante de tempo
longa acompanha a gravidade, entao reorientar o sensor volta a zero sozinho em
poucos segundos e o movimento rapido continua visivel. O estimador e atualizado
mesmo quando a gravidade esta sendo exibida, para que alternar a tecla nao
produza degrau.

A referencia de pressao passou a ser a mediana da janela de calibracao, nao a
media. O filtro IIR do BMP390 converge ao longo de aproximadamente um segundo
apos a configuracao, e uma referencia capturada no meio dessa subida fica errada
pela sessao inteira: observou-se um erro de cerca de dois hectopascal em um dos
barometros, equivalente a dezoito metros de altitude, que consumia o eixo
inteiro do grafico. A mediana ignora a rampa inicial. Alem disso a calibracao
mede a deriva dentro da propria janela e compara as duas referencias entre si,
avisando no rodape quando algo nao fecha, e a tecla `Z` re-zera a altitude no
ponto atual sem exigir repouso.

## Calibracao magnetica ao vivo

`python/calibrate_magnetometer.py` ganhou `--live`, que grava a rotacao direto do
modo de apresentacao em vez de exigir um arquivo capturado antes. Reaproveita
`StreamDecoder`, `SerialSource` e `wait_for_banner` de
`python/presentation_monitor.py`, entao existe uma unica implementacao da leitura
do fluxo. A janela e medida pelo relogio do sensor, como nas demais ferramentas.

O documento gerado registra o canal do multiplexador de cada sensor, e
`load_magnetometer_calibrations` recusa um arquivo cujo canal nao corresponda ao
firmware em execucao. Essa verificacao existe por causa de um caso concreto: o
mapeamento mudou de 0, 1 e 4 para 4, 5 e 6, e o arquivo antigo em `data/`
aplicaria a correcao de cada sensor no sensor errado sem nenhum aviso.

`wait_for_banner` passou a devolver, quando solicitado, os pacotes que chegaram
junto com o banner. A verificacao textual podia descarta-los sem consequencia,
mas uma captura de calibracao perderia sua abertura.

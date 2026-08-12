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

O ICS43434 e capturado pelo DMA da biblioteca `Audio` oficial da Teensy em
PCM mono `int16`, 44100 Hz, canal esquerdo. Cada sessao mantem `imu.bin` e
`audio.raw` abertos simultaneamente, com filas RAM independentes. A fila DMA
mantem ate 511 blocos uteis e cada bloco possui sequencia interna. Se houver
perda, o firmware insere silencio equivalente no arquivo e registra o evento,
preservando a duracao e o alinhamento das amostras seguintes. O arquivo de
audio nao possui cabecalho; os parametros e o timestamp estimado da primeira
amostra ficam em `meta.txt`. Antes da sessao, dois segundos em silencio sao
avaliados em RAM. Nivel alto, DC ou clipping acima do esperado geram
`AUDIO_PREFLIGHT_WARNING`, mas o audio continua sendo gravado para preservar o
dado cru. Somente a ausencia de blocos validos gera `AUDIO_CAPTURE_REJECTED`.

## Sessões no cartão SD

Com um cartão FAT ou exFAT conectado nos pinos documentados, o firmware cria
pastas `/Sxxx` consecutivas sem depender de reboot. Para o teste atual, cada
sessao dura cinco minutos; ao final, os arquivos sao truncados para o tamanho
real e a proxima pasta e prealocada automaticamente. O firmware nao espera a
USB e continua adquirindo sem computador.

Se um arquivo do SD ficar dois segundos completos sem qualquer progresso de
escrita, o firmware emite
`SD_ERROR_CONFIRMED`, desativa a gravação até o reboot e, após cinco segundos,
pisca duas vezes o LED laranja integrado. O LED compartilha o pino do clock SPI
e só é controlado depois que o SPI foi encerrado com segurança.

Para reduzir a carga e melhorar a margem elétrica, o SD opera a 12 MHz.
`imu.bin` e `audio.raw` usam blocos completos de 512 bytes durante a gravacao;
fragmentos finais sao permitidos somente no fechamento. Flushes independentes
nao drenam as filas, e o audio recebe prioridade quando sua ocupacao passa de
50%. Cada pasta possui
`journal.txt`, com os tamanhos confirmados dos dois fluxos. Uma queda de
energia pode deixar uma cauda prealocada, mas o Python ignora automaticamente
os bytes posteriores ao journal. Para mudar o teste de cinco minutos para uma
hora, altere apenas `kSdSessionDurationSeconds` depois da validacao em hardware.

Depois de desligar a Teensy e remover o cartão, analise a sessão diretamente:
ajuste a letra da unidade caso o Windows monte o cartão em outro caminho.

```powershell
python python/parse_data.py E:/S001/imu.bin
python python/analyze_imu.py E:/S001/imu.bin --no-show
python python/export_audio.py E:/S001 --gain-db 18
ffplay data/S001_audio.wav
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

### Teste isolado microfone + SD

`kAudioSdDiagnosticEnabled=true` ativa temporariamente um teste de dez
minutos que inicializa somente o ICS43434 e o cartao SD. Nesse modo, o firmware
nao inicializa `Wire`, PCA9548A, ICM-20948 ou BMP390 e nao cria pastas `/Sxxx`.
Ele cria uma unica pasta `/Mxxx`, prealoca `audio.raw` antes de iniciar o I2S e
nao faz rotacao automatica. Os primeiros cinco minutos usam escritas de 1024
bytes e os cinco seguintes usam 2048 bytes, sem interrupcao ou nova
prealocacao entre as fases. Ao fim, desliga a captura, drena o buffer, trunca
o arquivo e emite `MIC_SD_TEST_COMPLETED`.

Carregue e monitore:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload --target monitor --upload-port COM3
```

Depois de `MIC_SD_TEST_COMPLETED`, desligue a Teensy, remova o cartao e use a
letra atribuida pelo Windows:

```powershell
Get-Content E:/M001/status.txt
Get-Content E:/M001/journal.txt
python python/analyze_sd_blocks.py E:/M001
python python/export_audio.py E:/M001
ffplay data/M001_audio.wav
```

O resultado esperado e aproximadamente 600 segundos, zero falhas de escrita e,
idealmente, zero blocos perdidos ou preenchidos com silencio. O analisador
mostra histogramas de latencia separados e uma recomendacao provisoria do
menor bloco aceitavel. Para voltar ao firmware completo depois do teste, altere somente
`kAudioSdDiagnosticEnabled=false`.

`kMicrophoneDiagnosticEnabled=true` inicia somente o
ICS43434 em RAM. SD, PCA9548A e sensores nao sao inicializados nesse modo. O
monitor serial informa, a cada dois segundos, taxa efetiva, perdas da fila,
uso de memoria, DC, RMS, clipping e atividade dos dois canais.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload --target monitor --upload-port COM3
```

O teste deve incluir alguns segundos em silencio, fala em nivel normal e sons
fortes sem encostar no microfone. O canal esperado e o esquerdo porque `SEL`
esta ligado ao GND.

Na configuracao normal, `kMicrophoneDiagnosticEnabled=false` e
`kMicrophoneRecordingEnabled=true`. Nesse modo `audio.raw` e gravado junto de
`imu.bin`; o diagnostico isolado nao cria arquivos.

O arquivo cru tambem pode ser ouvido diretamente com FFplay 8 usando
`-ch_layout mono`:

```powershell
ffplay -f s16le -ar 44100 -ch_layout mono "E:/S001/audio.raw"
```

Para audio de baixo nivel, prefira `python/export_audio.py --gain-db 18`.
O ganho afeta somente o WAV de reproducao; `audio.raw` permanece inalterado.
O exportador informa `peak_safe_gain_db` e avisa quando o ganho escolhido
produz clipping; nao se deve amplificar uma captura que ja esteja proxima da
escala completa.

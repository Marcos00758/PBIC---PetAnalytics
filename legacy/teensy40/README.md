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

Os três ICM-20948 nos canais 4, 5 e 6 do PCA9548A são lidos a 100 Hz. O firmware
transmite pela USB pacotes binários de 79 bytes com timestamp, sequência, 27
valores crus `int16` dos ICMs, quatro valores `uint32` crus dos BMP390 e CRC-8.
O mesmo fluxo é gravado em `/Sxxx/imu.bin`; `meta.txt` preserva a configuração
e os 21 bytes NVM individuais de cada BMP390, enquanto `status.txt` registra
contadores da aquisição e do SD.
O formato completo está em
`docs/DATA_FORMAT.md`.

O microfone esta desativado na configuracao normal: I2S nao e inicializado e
nenhum `audio.raw` e criado. O codigo de diagnostico do ICS43434 permanece no
repositorio para retomada futura, sem participar da aquisicao atual.

## Sessões no cartão SD

Com um cartão FAT ou exFAT conectado nos pinos documentados, o firmware cria
uma pasta `/Sxxx` no boot e mantem o mesmo `imu.bin` aberto continuamente ate
o reboot ou uma falha confirmada do SD. Nao existe encerramento por tempo nem
rotacao para outra pasta. O firmware nao espera a USB e funciona sem
computador.

Se um arquivo do SD ficar dois segundos completos sem qualquer progresso de
escrita, o firmware emite
`SD_ERROR_CONFIRMED`, desativa a gravação até o reboot e, após cinco segundos,
pisca duas vezes o LED laranja integrado. O LED compartilha o pino do clock SPI
e só é controlado depois que o SPI foi encerrado com segurança.

Para reduzir a carga e melhorar a margem elétrica, o SD opera a 12 MHz.
`imu.bin` usa escritas completas de 512 bytes. A aquisicao dos sensores e
consultada antes de qualquer operacao do SD. O arquivo recebe no boot uma
prealocacao de dez minutos e cresce normalmente ao ultrapassar essa reserva.
Flush e journal ocorrem a cada 1000 pacotes, aproximadamente dez segundos;
`journal.bin` recebe um registro binario append-only com CRC e guarda o prefixo
confirmado por `sync()`. Em queda de energia,
o Python ignora a cauda prealocada e os dados posteriores ao ultimo journal.

Depois de desligar a Teensy e remover o cartão, analise a sessão diretamente:
ajuste a letra da unidade caso o Windows monte o cartão em outro caminho.

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

Com o firmware em modo de apresentacao, a rotacao pode ser gravada ao vivo em um
unico comando, sem arquivo intermediario:

```powershell
python python/calibrate_magnetometer.py --live
```

## Diagnostico do microfone

### Teste isolado microfone + SD

`kAudioSdDiagnosticEnabled=true` ativa temporariamente um teste de dez
minutos que inicializa somente o ICS43434 e o cartao SD. Nesse modo, o firmware
nao inicializa `Wire`, PCA9548A, ICM-20948 ou BMP390 e nao cria pastas `/Sxxx`.
Ele cria uma unica pasta `/Mxxx`, prealoca `audio.raw` antes de iniciar o I2S e
nao faz rotacao automatica. Os primeiros cinco minutos usam escritas de 256
bytes e os cinco seguintes usam 512 bytes, sem interrupcao ou nova
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
mostra histogramas de latencia separados, inspeciona cada metade do PCM em
busca do padrao de bits altos observado no M002 e apresenta uma recomendacao
provisoria. Para voltar ao firmware completo depois do teste, altere somente
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

Na configuracao normal, `kMicrophoneDiagnosticEnabled=false`,
`kAudioSdDiagnosticEnabled=false` e `kMicrophoneRecordingEnabled=false`.
Os modos de audio devem ser habilitados apenas para diagnosticos dedicados.

O arquivo cru tambem pode ser ouvido diretamente com FFplay 8 usando
`-ch_layout mono`:

```powershell
ffplay -f s16le -ar 44100 -ch_layout mono "E:/M001/audio.raw"
```

Para audio de baixo nivel, prefira `python/export_audio.py --gain-db 18`.
O ganho afeta somente o WAV de reproducao; `audio.raw` permanece inalterado.
O exportador informa `peak_safe_gain_db` e avisa quando o ganho escolhido
produz clipping; nao se deve amplificar uma captura que ja esteja proxima da
escala completa.

## Modo de apresentacao e verificacao dos sensores

`kPresentationStreamEnabled=true` ativa um modo de bancada que transmite pela
USB o mesmo pacote v4 de 79 bytes e nao usa o cartao SD. Nesse modo o firmware
nao inicializa o SD, nao cria pastas `/Sxxx` e nao depende de cartao. Ele
inicializa `Wire`, o PCA9548A, os tres ICM-20948 e os dois BMP390, e imprime um
banner textual antes do fluxo binario. O banner repete a cada cinco segundos,
portanto o computador pode se conectar a qualquer momento e ainda receber a
identificacao dos sensores e os 21 bytes NVM de cada BMP390. Sem esses bytes
nao existe compensacao Bosch, porque nao ha `meta.txt` em um modo sem cartao.

O modo e mutuamente exclusivo com os dois diagnosticos de microfone, com a
gravacao de audio e com `kUsbBinaryStreamEnabled`; `static_assert` impede a
combinacao. Na configuracao normal o valor e `false`.

Carregue e verifique:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload --upload-port COM3
python python/presentation_monitor.py
```

A porta e detectada automaticamente. O programa espera o banner, pede que o
dispositivo fique parado e analisa uma janela de cinco segundos do relogio do
sensor. Use `--port`, `--duration` e `--json` para ajustar.

```powershell
python python/presentation_monitor.py --port COM3 --duration 10 --json data/verificacao.json
```

O relatorio traz uma linha por sensor com veredito `ok`, `warning` ou `fail`. A
verificacao cobre CRC, lacunas de sequencia, taxa efetiva e jitter, modulo do
acelerometro proximo de 9,81 em repouso, giroscopio proximo de zero em repouso,
magnetometro que atualiza e tem modulo plausivel, saturacao proxima do fundo de
escala, e pressao compensada em hectopascal com os dois BMP390. O processo
termina com codigo 1 quando algum veredito e `fail`.

O teste de gravidade e o de giroscopio em repouso exigem o dispositivo parado
durante toda a janela. Movimento durante a coleta produz `gyro_not_at_rest`, que
indica o procedimento e nao uma falha do sensor.

## Painel grafico em tempo real

`python/presentation_panel.py` consome o mesmo fluxo do modo de apresentacao e
desenha nove paineis, uma linha por ICM e uma coluna por grandeza, com os tres
eixos em cada painel. No rodape ficam as duas pressoes em hectopascal e a
altitude relativa em centimetros.

```powershell
python -m pip install -r python/requirements.txt
python python/presentation_panel.py
```

Opcoes: `--port`, `--window` para a janela deslizante, `--calibration` para a
duracao da calibracao inicial e `--mag-calibration` para aplicar o JSON gerado
por `python/calibrate_magnetometer.py`.

```powershell
python python/presentation_panel.py --window 20 --mag-calibration data/rotacao_3d_v4_retry_mag_calibration.json
```

### Calibracao inicial

Ao abrir, o programa pede que o dispositivo fique parado por alguns segundos e
mede tres referencias: o bias de cada eixo de cada giroscopio, o vetor de
gravidade de cada acelerometro e a pressao de referencia de cada BMP390. O bias
do giroscopio passa a ser subtraido sempre. A gravidade e subtraida por padrao,
o que centra os acelerometros em zero e deixa o movimento muito mais legivel na
grade; a tecla `G` alterna e mostra o vetor completo com o modulo proximo de
9,81. A pressao de referencia e o zero da altitude relativa, entao levantar o
dispositivo produz um valor positivo em centimetros.

### Controles

| Tecla | Acao |
|---|---|
| espaco | congela a visualizacao, a leitura da serial continua |
| `G` | alterna entre remover e mostrar a gravidade |
| `L` | trava ou libera a escala vertical |
| `B` | liga e desliga a media movel do barometro |
| `F` | liga e desliga a suavizacao dos IMU |
| `D` | mostra ou oculta as amostras repetidas do cache |
| `Z` | re-zera a altitude no ponto atual, sem exigir repouso |
| `R` | refaz a calibracao inicial |
| setas | aumenta ou reduz a janela deslizante |
| `Q` | sai |

### Filtragem

Nenhuma suavizacao acontece sem estar declarada. O rodape mostra o estado de
cada filtro e as teclas acima alternam cada um. Os padroes estao no inicio de
`python/presentation_panel.py`: `PRESSURE_FILTER_ENABLED`,
`IMU_FILTER_ENABLED` e `DECIMATION_ENABLED`, todos ligados.

A pressao e mostrada como desvio da referencia de calibracao, em pascal, e nao
como valor absoluto: os dois barometros ficam separados por cerca de dez pascal
so por calibracao de fabrica, e plotar o absoluto gastava o eixo nessa diferenca.

O barometro atualiza a 25 Hz e o magnetometro a 20 Hz, mas ambos sao repetidos
do cache nos pacotes de 100 Hz. A decimacao remove essas repeticoes antes de
qualquer media, para que uma media de um segundo cubra vinte e cinco leituras
reais e nao cem valores com repeticao. Medido em bancada, isso leva o ruido de
pressao de cerca de 3 Pa para 0,7 Pa, ou de 26 cm para 6 cm em altitude, apenas
no lado do computador.

No firmware, `kBmpOversamplingEnabled`, `kBmpIirFilterEnabled` e
`kIcmDlpfEnabled` em `src/config/constants.h` atacam o mesmo ruido na origem e
tambem sao chaves de `true` e `false`.

### Faixa de status

A faixa superior traz o veredito ao vivo por sensor, a taxa efetiva, as perdas
de sequencia e as falhas de CRC. Ela usa a mesma avaliacao de
`python/presentation_monitor.py`, porem sem os testes que exigem repouso: com o
dispositivo em movimento, gravidade e giroscopio parado nao sao criterios
validos. Os testes de sensor travado, atualizacao do magnetometro, saturacao e
saude do fluxo continuam ativos. Para o veredito completo em repouso, use
`python/presentation_monitor.py`.

## Calibracao magnetica dos tres ICM

Cada AK09916 tem metal diferente por perto, bateria, cartao e fios, e esse metal
soma um deslocamento constante a leitura. Sem corrigir, os tres sensores medem
intensidades diferentes no mesmo lugar. Medido em bancada: 28,6 uT no ICM0,
5,9 uT no ICM1 e 56,8 uT no ICM2, quando o campo terrestre no sudeste do Brasil
e de cerca de 23 uT. E o que faz `magnetic_norm_out_of_range` aparecer.

Com o modo de apresentacao gravado, rode e gire o conjunto devagar em todas as
direcoes, cobrindo os tres eixos, como se desenhasse uma esfera no ar:

```powershell
python python/calibrate_magnetometer.py --live
python python/presentation_panel.py --mag-calibration data/mag_calibration.json
```

O programa imprime o progresso a cada cinco segundos, mostrando o menor eixo ja
coberto e a meta. Enquanto o menor eixo estiver abaixo da meta, continue
girando; ao final ele recusa a captura em vez de gerar uma calibracao ruim.

O JSON registra em que canal do multiplexador cada sensor foi medido, e o painel
recusa um arquivo cujo canal nao bata com o firmware em execucao. O mapeamento
ja mudou uma vez neste projeto, e sem essa verificacao um arquivo antigo
corrigiria cada sensor com o ferro de outro, silenciosamente.

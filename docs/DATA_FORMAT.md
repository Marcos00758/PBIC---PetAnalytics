# Formato de dados

## Pacote de sensores v4

O firmware transmite pela USB um fluxo binario de tamanho fixo. Cada pacote
representa uma rodada dos tres ICM-20948 e inclui o ultimo valor valido em cache
dos tres AK09916 e dos dois BMP390. Nao ha conversao para `float` no firmware.

O pacote v4 tem 79 bytes, usa little-endian e nao contem padding. Ele nao e
compativel com v1/v2. A estrutura binaria e igual a v3 experimental, mas a
escala do giroscopio mudou de `+/-1000` para `+/-2000 graus/s`; por isso os
metadados devem ser usados para distinguir as duas versoes.
`python/analyze_imu.py` le automaticamente o `.bin.json` ao lado da captura e
preserva a escala de `1000 graus/s` para arquivos v3. Na ausencia de metadados,
assume a configuracao atual v4 de `2000 graus/s`.

| Offset | Tamanho | Tipo | Campo | Descricao |
|---:|---:|---|---|---|
| 0 | 2 | `uint16` | `magic` | Valor fixo `0xAA55`; bytes `55 AA` no fluxo |
| 2 | 4 | `uint32` | `timestamp_us` | `micros()` no inicio da rodada |
| 6 | 2 | `uint16` | `sequence` | Sequencia com retorno a zero apos 65535 |
| 8 | 54 | `27 x int16` | `values` | Accel, gyro e mag dos tres ICMs |
| 62 | 4 | `uint32` | `bmp0_pressure_raw` | Pressao crua BMP390, canal 2 |
| 66 | 4 | `uint32` | `bmp0_temperature_raw` | Temperatura crua BMP390, canal 2 |
| 70 | 4 | `uint32` | `bmp1_pressure_raw` | Pressao crua BMP390, canal 3 |
| 74 | 4 | `uint32` | `bmp1_temperature_raw` | Temperatura crua BMP390, canal 3 |
| 78 | 1 | `uint8` | `crc8` | CRC dos bytes 0 a 77 |

Formato equivalente no Python:

```python
PACKET_FMT = "<HIH27h4IB"
PACKET_SIZE = 79
```

A ordem dos 27 valores `int16` e:

```text
icm0_ax, icm0_ay, icm0_az, icm0_gx, icm0_gy, icm0_gz, icm0_mx, icm0_my, icm0_mz,
icm1_ax, icm1_ay, icm1_az, icm1_gx, icm1_gy, icm1_gz, icm1_mx, icm1_my, icm1_mz,
icm2_ax, icm2_ay, icm2_az, icm2_gx, icm2_gy, icm2_gz, icm2_mx, icm2_my, icm2_mz
```

Mapeamento: ICM0 canal 0, ICM1 canal 1, BMP0 canal 2, BMP1 canal 3 e ICM2
canal 4 do PCA9548A.

## CRC-8

- polinomio: `0x07`;
- valor inicial: `0x00`;
- sem reflexao;
- XOR final: `0x00`;
- cobertura: todos os bytes, exceto o ultimo `crc8`.

O parser procura o `magic`, valida o CRC e avanca um byte quando encontra um
pacote invalido. Isso permite recuperar alinhamento depois de corrupcao ou de
mensagens textuais emitidas no boot.

## Escalas dos ICMs

Depois de saturacao observada em movimentos fortes com as faixas anteriores, os
tres ICM-20948 passaram a usar `+/-8 g` e `+/-2000 graus/s`:

```text
aceleracao_m_s2 = raw * ((8 * 9.80665) / 32767.5)
giroscopio_dps  = raw * (2000 / 32768)
magnetometro_uT = raw * 0.15
```

O analisador conta valores com modulo maior ou igual a 32000 como proximos do
limite. As novas faixas precisam ser revalidadas com movimento representativo
do uso real antes da coleta definitiva.

## Magnetometro e calibracao

Cada AK09916 mede a 20 Hz e seu proxy e consultado a 25 Hz. A taxa de consulta
ligeiramente maior evita perder atualizacoes por desalinhamento de fase. As
consultas dos tres sensores sao distribuidas por fases diferentes das rodadas
de 100 Hz para reduzir jitter. O ultimo valor
valido e repetido no pacote; repeticoes sao esperadas.

`python/analyze_imu.py` informa mudancas observadas, taxa de mudanca dos valores,
intervalos entre mudancas e faixa do modulo magnetico. Essa taxa e um limite
inferior da ODR, pois atualizacoes reais podem repetir a mesma contagem. Para uma calibracao
inicial, grave uma rotacao lenta e ampla nos tres eixos e execute:

```powershell
python python/calibrate_magnetometer.py data/rotacao_3d.bin
```

A ferramenta estima hard-iron por centro dos extremos e soft-iron diagonal por
escala dos tres eixos. Ela rejeita capturas com amplitude insuficiente. O JSON
gerado pode ser aplicado na analise:

```powershell
python python/analyze_imu.py data/teste.bin --mag-calibration data/rotacao_3d_mag_calibration.json
```

O formato aceita uma matriz soft-iron 3 x 3 completa, mas a ferramenta inicial
nao estima termos cruzados. Uma calibracao elipsoidal mais avancada podera ser
adicionada quando houver um conjunto de rotacoes adequado.

## BMP390 bruto

Cada BMP390 mede em modo continuo a 25 Hz. As consultas dos dois sensores sao
colocadas em fases diferentes para evitar concentrar as transacoes I2C na mesma
rodada. Pressao e temperatura sao lidas como
valores crus unsigned de 24 bits, armazenados em `uint32`, e repetidos a partir
do cache nas demais rodadas.

Enquanto um cache ainda nao possui leitura valida, o firmware usa o sentinela
`0xFFFFFFFF`, impossivel em um dado valido de 24 bits. O analisador ignora esse
valor nos graficos e informa quantos pacotes invalidos foram encontrados.

As contagens cruas nao sao Pa nem graus Celsius. A compensacao Bosch depende
dos 21 bytes NVM individuais de cada BMP390. Esses coeficientes serao gravados
em `meta.txt` na etapa do cartao SD. Nesta etapa USB, o Python valida e plota as
contagens cruas sem atribuir unidades fisicas incorretas.

## Temporizacao e perdas

O agendador inicia uma rodada a cada 10000 us, correspondente a 100 Hz. Os
sensores compartilham o PCA9548A e sao lidos sequencialmente; o timestamp e
comum a rodada, mas as leituras nao sao fisicamente simultaneas.

Se o loop perder periodos, nao executa rajadas para recuperar. A sequencia
avanca pelos periodos perdidos. Falha em qualquer ICM descarta a rodada; falha
BMP ou mag preserva o ultimo cache valido e incrementa contador proprio.

Os contadores de aquisicao nao entram no pacote. Eles sao persistidos
periodicamente em `status.txt` pelo logger do SD.

## Volume, captura e arquivos longos

A 100 Hz, o fluxo nominal e de 7900 bytes/s. Uma janela de 10 segundos sem
perdas contem 1000 pacotes e 79000 bytes.

`python/capture_serial.py` salva uma janela semiaberta de tempo do sensor e
preserva os bytes recebidos. A captura valida apenas os novos pacotes recebidos,
sem reprocessar todo o buffer a cada leitura, para sustentar janelas maiores sem
perder bytes por carga excessiva no computador. O `.bin.json` registra versao, tamanho, porta,
horario, SHA-256 e contadores.

`python/parse_data.py` e a entrada principal de `python/analyze_imu.py`
processam arquivos em blocos de 64 KiB. A analise calcula estatisticas sobre
todos os pacotes, mas limita os graficos a aproximadamente 50000 pontos para
evitar uso excessivo de memoria em sessoes longas.

## Sessao no cartao SD

O cartao usa FAT ou exFAT e mantem a seguinte estrutura:

```text
/session.txt
/S001/imu.bin
/S001/audio.raw
/S001/meta.txt
/S001/status.txt
```

`imu.bin` contem exatamente a concatenacao dos mesmos pacotes v4 de 79 bytes
usados no stream USB, sem cabecalho e sem mensagens textuais. O arquivo fica
aberto durante a sessao. Uma fila circular de 8192 bytes desacopla a producao
dos pacotes das escritas de ate 512 bytes; ha flush a cada 1000 pacotes. O
audio possui fila separada de 32768 bytes e e escrito em blocos de ate 4096
bytes. Em cada passagem do loop ocorre no maximo uma escrita ao SD. Um
desligamento abrupto ainda pode perder os dados posteriores ao ultimo flush.

`audio.raw` contem PCM mono assinado de 16 bits, little-endian, sem cabecalho,
capturado do canal esquerdo do ICS43434 a 44100 Hz. Cada amostra ocupa dois
bytes e o volume nominal e 88200 bytes/s, aproximadamente 158,76 MB em 30
minutos. O arquivo fica aberto junto de `imu.bin`. A fila DMA/RAM de captura e
a fila do SD sao independentes das filas dos pacotes IMU.

O timestamp inicial do audio usa a mesma origem `micros()` dos pacotes IMU. Ele
e estimado no recebimento do primeiro bloco DMA, subtraindo a duracao de 128
amostras. A incerteza documentada e de ate um bloco, aproximadamente 2903 us;
nao existe timestamp por bloco dentro de `audio.raw`. O indice de uma amostra
pode ser convertido para a linha de tempo da sessao por:

```text
audio_timestamp_us = audio_start_timestamp_us + sample_index * 1000000 / 44100
```

`meta.txt` e um arquivo ASCII `chave=valor`. Alem de versao, taxas, faixas,
canais, enderecos e status inicial dos sensores, contem:

```text
packet_version=4
packet_size=79
sd_spi_clock_mhz=12
sd_imu_write_block_bytes=512
sd_audio_write_block_bytes=4096
audio_enabled=1
audio_file=audio.raw
audio_format=pcm_s16le
audio_byte_order=little
audio_sample_rate_hz=44100
audio_channels=1
audio_bits_per_sample=16
audio_block_samples=128
audio_start_timestamp_valid=1
audio_start_timestamp_us=<micros estimado da primeira amostra>
bmp0_nvm_valid=1
bmp0_nvm=<42 caracteres hexadecimais>
bmp1_nvm_valid=1
bmp1_nvm=<42 caracteres hexadecimais>
```

Cada NVM possui 21 bytes lidos dos registradores `0x31` a `0x45` do BMP390.
`python/analyze_imu.py` procura automaticamente `meta.txt` na pasta de
`imu.bin`, aplica a compensacao Bosch em `float` e gera pressao em Pa e
temperatura em graus Celsius. Sem NVM valida, preserva o grafico de contagens
cruas e informa `bmp_compensation=unavailable_raw_only`.

A prealocacao SdFat foi avaliada e permanece desativada. `preAllocate()` muda o
tamanho logico do arquivo e requer `truncate()` no encerramento para remover a
area nao escrita. Como o wearable ainda nao possui comando de parada e pode
ser desligado diretamente, habilita-la produziria arquivos com cauda invalida
apos perda de energia. As filas RAM foram dimensionadas para absorver as
pausas de SD observadas sem depender dessa operacao.

`status.txt` e atualizado inicialmente e depois a cada 18000 pacotes, ou tres
minutos a 100 Hz. Ele registra contadores de agendamento, I2C, magnetometros,
BMPs, USB, fila do SD, bytes escritos, tentativas, falhas e flushes. Tambem
registra as duracoes maximas de escrita, flush e atualizacao do proprio status,
em microssegundos, e quantas dessas operacoes levaram pelo menos 10 ms. A
atualizacao usa `status.tmp` e
renomeacao; apos perda fisica do cartao, o ultimo status persistido naturalmente
pode nao conter o evento que impediu a escrita.

Para o audio, `status.txt` registra blocos recebidos pelo DMA, overflow da fila
de captura, blocos incompletos, maior ocupacao da fila, blocos aceitos ou
descartados pelo buffer do SD, bytes escritos, tentativas e falhas. As maiores
latencias e quantidades de escritas e flushes acima de 10 ms sao separadas das
metricas de `imu.bin`.

Quando uma falha e confirmada, a Serial emite `SD_ERROR_STATE` com tentativas,
sucessos, falhas, bytes ainda em cada buffer, idade da falha e maior duracao de
escrita observada para `imu.bin` e `audio.raw`. Esses dados sao mais atuais que
o ultimo `status.txt`, que pode ter sido escrito minutos antes.

## Graficos

`python/analyze_imu.py` gera:

- `.png`: acelerometro e giroscopio em 2 x 3;
- `_mag.png`: magnetometros em `uT`;
- `_bmp.png`: pressao e temperatura compensadas quando ha NVM em `meta.txt`;
- `_bmp_raw.png`: contagens cruas quando a NVM nao esta disponivel.

Somente pacotes aprovados por magic e CRC participam da validacao e dos
graficos.

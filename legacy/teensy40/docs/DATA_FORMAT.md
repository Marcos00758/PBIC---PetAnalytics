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
| 62 | 4 | `uint32` | `bmp0_pressure_raw` | Pressao crua BMP390, canal 7 |
| 66 | 4 | `uint32` | `bmp0_temperature_raw` | Temperatura crua BMP390, canal 7 |
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

Mapeamento: ICM0 canal 4, ICM1 canal 5, BMP0 canal 7, BMP1 canal 3 e ICM2
canal 6 do PCA9548A.

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

`timestamp_us` retorna a zero a cada `2^32` microssegundos, aproximadamente
71,6 minutos. O firmware compara deadlines por subtracao modular de `uint32` e
possui verificacoes de compilacao antes e depois do rollover. O Python nao
subtrai apenas o primeiro timestamp do ultimo: ele acumula os intervalos entre
pacotes consecutivos, preservando a duracao de sessoes com multiplos
rollovers.

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
/S001/meta.txt
/S001/journal.bin
/S001/status.txt
```

`imu.bin` contem exatamente a concatenacao dos mesmos pacotes v4 de 79 bytes
usados no stream USB, sem cabecalho e sem mensagens textuais. O arquivo fica
aberto durante a sessao. Uma fila circular de 8192 bytes desacopla a producao
dos pacotes das escritas. Durante a operacao normal, o arquivo e escrito em
blocos completos de 512 bytes. Em cada passagem do loop ocorre no maximo uma
operacao ao SD e a aquisicao dos sensores e atendida antes dessa operacao.

O microfone esta desativado: I2S nao e inicializado e `/Sxxx/audio.raw` nao e
criado. O fluxo IMU nominal e 7900 bytes/s, aproximadamente 28,44 MB por hora.
O teste atual prealoca 4.747.900 bytes, equivalentes a dez minutos mais um
segundo de margem. O arquivo continua crescendo ao ultrapassar a reserva e
permanece na mesma pasta ate reboot ou falha confirmada do SD. Depois da
validacao, a prealocacao planejada de quatro horas sera 113.767.900 bytes.

`meta.txt` e um arquivo ASCII `chave=valor`. Alem de versao, taxas, faixas,
canais, enderecos e status inicial dos sensores, contem:

```text
packet_version=4
packet_size=79
sd_spi_clock_mhz=12
sd_imu_write_block_bytes=512
sd_free_bytes_at_boot=<bytes livres medidos>
sd_recording_budget_bytes=<bytes livres menos 4 MiB>
sd_estimated_recording_seconds=<estimativa nominal>
sd_continuous_session=1
sd_preallocation_seconds=600
sd_rotate_sessions=0
sd_preallocation_margin_seconds=1
imu_preallocated_bytes=4747900
journal_update_packets=1000
journal_file=journal.bin
journal_record_version=1
journal_record_size=32
journal_magic=0x4A50
journal_crc=CRC-8 polynomial 0x07
audio_enabled=0
preallocation_enabled=1
preallocation_tail_source=journal.bin
bmp0_nvm_valid=1
bmp0_nvm=<42 caracteres hexadecimais>
bmp1_nvm_valid=1
bmp1_nvm=<42 caracteres hexadecimais>
```

Com `sd_continuous_session=1` e `sd_rotate_sessions=0`, o firmware nao fecha a
sessao por tempo nem cria outra `/Sxxx`. O pacote v4 permanece inalterado.

Cada NVM possui 21 bytes lidos dos registradores `0x31` a `0x45` do BMP390.
`python/analyze_imu.py` procura automaticamente `meta.txt` na pasta de
`imu.bin`, aplica a compensacao Bosch em `float` e gera pressao em Pa e
temperatura em graus Celsius. Sem NVM valida, preserva o grafico de contagens
cruas e informa `bmp_compensation=unavailable_raw_only`.

A prealocacao SdFat esta habilitada somente para `imu.bin`. Se houver
desligamento abrupto, o arquivo pode manter a cauda reservada; ela nao deve ser
interpretada como dado real.

`journal.bin` permanece aberto durante a sessao e recebe um registro append-only
de 32 bytes logo apos cada `sync()` bem-sucedido de `imu.bin`. O arquivo nao e
prealocado. Cada registro usa little-endian:

| Offset | Tamanho | Tipo | Campo | Descricao |
|---:|---:|---|---|---|
| 0 | 2 | `uint16` | `magic` | `0x4A50` |
| 2 | 1 | `uint8` | `version` | `1` |
| 3 | 1 | `uint8` | `state` | 1 gravando, 2 parado, 3 completo |
| 4 | 4 | `uint32` | `sequence` | Numero crescente do checkpoint |
| 8 | 4 | `uint32` | `uptime_ms` | `millis()` no checkpoint |
| 12 | 8 | `uint64` | `imu_valid_bytes` | Prefixo confirmado de `imu.bin` |
| 20 | 8 | `uint64` | `audio_valid_bytes` | Zero no firmware IMU-only |
| 28 | 3 | bytes | `reserved` | Reservado, preenchido com zero |
| 31 | 1 | `uint8` | `crc8` | CRC-8 dos bytes 0 a 30 |

Os tamanhos publicados correspondem apenas ao prefixo confirmado por `sync()`;
podem ficar temporariamente atras dos contadores de bytes escritos, mas nunca
apontam deliberadamente para uma fila ainda nao sincronizada. O primeiro
checkpoint util ocorre apos aproximadamente dez segundos e os
seguintes a cada dez segundos. `python/parse_data.py` e
`python/analyze_imu.py` aplicam `imu_valid_bytes` automaticamente.
O parser percorre todos os registros e usa o ultimo com magic, versao e CRC
validos. Sessoes antigas com apenas `journal.txt` continuam suportadas.

`status.txt` e atualizado inicialmente e depois a cada 18000 pacotes, ou tres
minutos a 100 Hz. Ele registra contadores de agendamento, I2C, magnetometros,
BMPs, USB, fila do SD, bytes escritos, tentativas, falhas e flushes. Tambem
registra as duracoes maximas de escrita, flush e atualizacao do proprio status,
em microssegundos, e quantas dessas operacoes levaram pelo menos 10 ms. A
atualizacao usa `status.tmp` e
renomeacao; apos perda fisica do cartao, o ultimo status persistido naturalmente
pode nao conter o evento que impediu a escrita.

`status.txt` tambem registra `sd_partial_writes`, a ocupacao maxima da fila,
`sd_scheduler_imu_selections` e `sd_scheduler_maintenance_operations`. Campos
de audio nao sao emitidos na configuracao normal.

Quando uma falha e confirmada, a Serial emite `SD_ERROR_STATE` com tentativas,
sucessos, falhas, bytes ainda em cada buffer, idade da falha e maior duracao de
escrita observada para `imu.bin`. Esses dados sao mais atuais que
o ultimo `status.txt`, que pode ter sido escrito minutos antes.

## Graficos

`python/analyze_imu.py` gera:

- `.png`: acelerometro e giroscopio em 2 x 3;
- `_mag.png`: magnetometros em `uT`;
- `_bmp.png`: pressao e temperatura compensadas quando ha NVM em `meta.txt`;
- `_bmp_raw.png`: contagens cruas quando a NVM nao esta disponivel.

Somente pacotes aprovados por magic e CRC participam da validacao e dos
graficos.

## Sessao isolada de diagnostico do microfone

O modo `kAudioSdDiagnosticEnabled=true` cria uma estrutura separada para nao
misturar o ensaio com as sessoes completas:

```text
/M001/audio.raw
/M001/meta.txt
/M001/journal.txt
/M001/status.txt
```

`audio.raw` usa o formato experimental PCM mono `int16` little-endian a 44100
Hz. A sessao dura dez minutos, usa uma unica
prealocacao de 53008200 bytes, incluindo um segundo de margem, e nao cria
`imu.bin`. `journal.txt` continua
sendo a fonte de `audio_valid_bytes`, de modo que `python/export_audio.py`
funciona sem alteracoes especificas para `/Mxxx`.

`status.txt` registra apenas o caminho de audio: blocos DMA recebidos e
perdidos, ocupacao maxima, silencio inserido, eventos e maior gap, bytes,
escritas, falhas, flushes e latencias. Os campos prefixados por `phase0_`
descrevem os primeiros cinco minutos com blocos de 256 bytes; `phase1_`
descreve os cinco minutos seguintes com blocos de 512 bytes. Cada fase inclui
um histograma nas faixas `<1`, `1-2`, `2-5`, `5-10`, `10-20`, `20-50`,
`50-100` e `>=100 ms`. O teste e considerado integro quando o
arquivo possui aproximadamente 600 segundos, as falhas de escrita sao zero e
os contadores de perda/zero-fill sao zero ou suficientemente baixos para serem
investigados individualmente.

`python/analyze_sd_blocks.py` usa os tamanhos validos de cada fase para
inspecionar tambem o trecho correspondente de `audio.raw`. A recomendacao e
marcada como inconclusiva quando encontra concentracao suspeita junto aos
valores `+/-16384` e `+/-32768`, mesmo que o transporte SD nao registre perdas.

## Banner do modo de apresentacao

Com `kPresentationStreamEnabled=true` o firmware emite linhas ASCII terminadas
por `\n` antes do fluxo binario e as repete a cada cinco segundos. O pacote v4
permanece inalterado; o banner apenas antecede e intercala o fluxo, sempre
entre pacotes completos. O parser localiza o `magic` e valida o CRC, portanto
o texto intercalado nao quebra o alinhamento.

```text
PRESENTATION_BANNER firmware_version=0.5.1 packet_version=4 packet_size=79
  imu_sample_rate_hz=100 bmp_sample_rate_hz=25 mag_sample_rate_hz=20
  mag_poll_rate_hz=25 accel_range_g=8 gyro_range_dps=2000 streaming=1
PRESENTATION_ICM index=0 channel=4 ready=1 address=0x69 who_am_i=0xEA mag_wia2=0x09
PRESENTATION_BMP index=0 channel=7 ready=1 address=0x77 chip_id=0x60
PRESENTATION_BMP_NVM index=0 valid=1 nvm=<42 caracteres hexadecimais>
PRESENTATION_ERROR reason=<causa>
```

Cada linha e uma sequencia de pares `chave=valor` separados por espaco. A
primeira palavra identifica o registro. `streaming=1` informa que pacotes
binarios seguem o banner; `streaming=0` indica que a inicializacao de algum
sensor falhou e que apenas o banner sera repetido, o que preserva o
diagnostico pela USB mesmo sem fluxo de dados.

`PRESENTATION_BMP_NVM` substitui o papel de `meta.txt` nesse modo. Os mesmos 21
bytes dos registradores `0x31` a `0x45` sao publicados em hexadecimal, o que
permite a `python/presentation_monitor.py` aplicar a compensacao Bosch e
apresentar pressao em hectopascal e temperatura em graus Celsius sem cartao.

`python/presentation_monitor.py` contabiliza separadamente os bytes consumidos
por linhas de banner reconhecidas e os bytes realmente inesperados. Apenas os
inesperados indicam corrupcao do fluxo.

`PRESENTATION_I2C_SCAN` aparece somente quando algum sensor falha na
inicializacao. A varredura percorre os oito canais do multiplexador e lista os
enderecos que responderam, excluindo o proprio PCA9548A. Ela nao roda no
caminho saudavel porque uma varredura completa gera centenas de transacoes sem
resposta e pode travar o periferico I2C; por isso tambem reinicia o barramento
entre canais, garantindo que um canal vazio signifique ausencia de dispositivo
e nao barramento preso.

## Configuracao de filtragem nos metadados

`meta.txt` e o banner de apresentacao passaram a registrar como os sensores
estavam configurados, porque a mesma versao do pacote v4 pode agora ser gravada
com ou sem reducao de ruido:

```text
bmp_oversampling_enabled=1
bmp_pressure_oversampling=8
bmp_temperature_oversampling=2
bmp_iir_filter_enabled=1
bmp_iir_coefficient=3
icm_dlpf_enabled=1
icm_accel_dlpf_hz=50.4
icm_gyro_dlpf_hz=51.2
```

Com `icm_dlpf_enabled=0` as taxas anunciadas de 102,3 Hz e 100 Hz nao valem: o
acelerometro corre a 4500 Hz e o giroscopio a 9000 Hz, os divisores sao
ignorados e a amostragem a 100 Hz sofre aliasing. Capturas antigas, sem esses
campos no `meta.txt`, foram feitas nessa condicao.

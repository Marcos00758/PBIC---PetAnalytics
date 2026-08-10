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

`python/analyze_imu.py` informa mudancas aceitas, taxa efetiva observada,
intervalos entre mudancas e faixa do modulo magnetico. Para uma calibracao
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

Os contadores de aquisicao ainda nao entram no pacote. Eles serao persistidos em
`status.txt` na etapa do SD.

## Volume, captura e arquivos longos

A 100 Hz, o fluxo nominal e de 7900 bytes/s. Uma janela de 10 segundos sem
perdas contem 1000 pacotes e 79000 bytes.

`python/capture_serial.py` salva uma janela semiaberta de tempo do sensor e
preserva os bytes recebidos. O `.bin.json` registra versao, tamanho, porta,
horario, SHA-256 e contadores.

`python/parse_data.py` e a entrada principal de `python/analyze_imu.py`
processam arquivos em blocos de 64 KiB. A analise calcula estatisticas sobre
todos os pacotes, mas limita os graficos a aproximadamente 50000 pontos para
evitar uso excessivo de memoria em sessoes longas.

## Graficos

`python/analyze_imu.py` gera:

- `.png`: acelerometro e giroscopio em 2 x 3;
- `_mag.png`: magnetometros em `uT`;
- `_bmp_raw.png`: contagens cruas de pressao e temperatura dos BMP390.

Somente pacotes aprovados por magic e CRC participam da validacao e dos
graficos.

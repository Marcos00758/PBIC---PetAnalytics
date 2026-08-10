# Formato de dados

## Pacote IMU v2

O firmware transmite pela USB um fluxo de pacotes binarios de tamanho fixo.
Cada pacote representa uma rodada de leitura dos tres ICM-20948. Acelerometro,
giroscopio e magnetometro sao mantidos como contagens cruas `int16`; a conversao
para unidades fisicas e feita no Python.

O pacote v2 tem 63 bytes, usa little-endian e nao contem padding. Ele nao e
compativel com os arquivos v1 de 45 bytes:

| Offset | Tamanho | Tipo | Campo | Descricao |
|---:|---:|---|---|---|
| 0 | 2 | `uint16` | `magic` | Valor fixo `0xAA55`; bytes `55 AA` no fluxo |
| 2 | 4 | `uint32` | `timestamp_us` | `micros()` no inicio da rodada de aquisicao |
| 6 | 2 | `uint16` | `sequence` | Sequencia da rodada, com retorno a zero apos 65535 |
| 8 | 54 | `27 x int16` | `values` | Acelerometro, giroscopio e magnetometro dos tres ICMs |
| 62 | 1 | `uint8` | `crc8` | CRC dos bytes 0 a 61 |

Formato equivalente no Python:

```python
PACKET_FMT = "<HIH27hB"
PACKET_SIZE = 63
```

A ordem dos 27 valores e:

```text
icm0_ax, icm0_ay, icm0_az, icm0_gx, icm0_gy, icm0_gz, icm0_mx, icm0_my, icm0_mz,
icm1_ax, icm1_ay, icm1_az, icm1_gx, icm1_gy, icm1_gz, icm1_mx, icm1_my, icm1_mz,
icm2_ax, icm2_ay, icm2_az, icm2_gx, icm2_gy, icm2_gz, icm2_mx, icm2_my, icm2_mz
```

Mapeamento fisico: ICM0 no canal 0, ICM1 no canal 1 e ICM2 no canal 4 do
PCA9548A.

## CRC-8

- polinomio: `0x07`;
- valor inicial: `0x00`;
- sem reflexao;
- XOR final: `0x00`;
- cobertura: todos os bytes do pacote, exceto o ultimo byte `crc8`.

O parser procura o `magic`, valida o CRC e avanca um byte quando encontra um
pacote invalido. Isso permite recuperar o alinhamento depois de bytes perdidos
ou de mensagens textuais emitidas durante a inicializacao.

## Escalas e calibracao

As faixas configuradas sao `+/-2 g` e `+/-250 graus/s`. O Python aplica:

```text
aceleracao_m_s2 = raw * ((2 * 9.80665) / 32767.5)
giroscopio_dps  = raw * (250 / 32768)
magnetometro_uT = raw * 0.15
```

O Python aceita opcionalmente um arquivo JSON com offset hard-iron em `uT` e
matriz soft-iron 3 x 3 para cada sensor. A correcao aplicada e
`soft_iron_matrix @ (campo_uT - hard_iron_ut)`. Exemplo:

```json
{
  "icm0": {
    "hard_iron_ut": [0.0, 0.0, 0.0],
    "soft_iron_matrix": [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
  }
}
```

Sensores ou campos ausentes usam offset zero e matriz identidade. Os
coeficientes devem ser obtidos por uma rotina de calibracao posterior; o parser
nao os estima automaticamente.

## Temporizacao e cache

O agendador inicia uma rodada a cada 10000 us, correspondente a 100 Hz. Os tres
ICMs compartilham um multiplexador e sao lidos sequencialmente; o timestamp e
comum, mas as leituras nao sao fisicamente simultaneas.

Cada AK09916 produz dados a 20 Hz. O firmware distribui as consultas dos tres
magnetometros por fases diferentes das rodadas de 100 Hz para reduzir jitter. O
pacote repete o ultimo valor magnetico valido em cache entre atualizacoes;
repeticoes sao esperadas e nao representam uma taxa magnetica de 100 Hz. No
primeiro ciclo, os tres caches sao preenchidos.

Se o loop perder um ou mais periodos, nao executa leituras em rajada. A
sequencia avanca pelos periodos perdidos. Se qualquer ICM falhar por I2C, a
rodada inteira e descartada e a sequencia tambem avanca. Uma queda de pacote na
transmissao USB produz o mesmo tipo de gap observavel pelo parser.

O firmware mantem separadamente contadores de periodos perdidos, rodadas
descartadas, falhas I2C, estado dos magnetometros e descartes USB. Esses
contadores ainda nao fazem parte do pacote v2 e serao usados por `status.txt`
quando o cartao SD for implementado.

## Volume e captura

A 100 Hz, o fluxo nominal e de 6300 bytes/s. Uma janela de 10 segundos sem
perdas contem 1000 pacotes e 63000 bytes.

`python/capture_serial.py` sincroniza no primeiro pacote valido e salva uma
janela semiaberta de tempo do sensor: inclui pacotes com timestamp entre `t0` e
`t0 + 10 s`, excluindo o pacote no limite final. O arquivo `.bin` preserva os
bytes recebidos sem conversao. Um arquivo `.bin.json` registra versao e tamanho
do pacote, porta, horario, SHA-256 e contadores do parser.

Os arquivos criados em `data/` sao capturas de bancada no computador e nao
alteram a futura organizacao de sessoes do cartao SD.

## Validacao e visualizacao

`python/analyze_imu.py` usa apenas pacotes que passaram por magic e CRC. A
ferramenta informa pacotes validos, CRCs invalidos, bytes descartados, gaps de
sequencia, frequencia efetiva, periodo, jitter e volume efetivo da USB.

O grafico principal tem duas linhas e tres colunas para acelerometro e
giroscopio. Um segundo arquivo, com sufixo `_mag.png`, mostra os tres eixos
magneticos de cada ICM em `uT`. A opcao `--mag-calibration` aplica um arquivo de
calibracao no formato descrito acima. A ferramenta nao altera o fluxo bruto.

# Formato de dados

## Pacote IMU v1

O firmware transmite pela USB um fluxo de pacotes binarios de tamanho fixo.
Cada pacote representa uma rodada de leitura dos tres ICM-20948. Os dados sao
mantidos como contagens cruas `int16`; a conversao para unidades fisicas e feita
no Python.

O pacote tem 45 bytes, usa little-endian e nao contem padding:

| Offset | Tamanho | Tipo | Campo | Descricao |
|---:|---:|---|---|---|
| 0 | 2 | `uint16` | `magic` | Valor fixo `0xAA55`; bytes `55 AA` no fluxo |
| 2 | 4 | `uint32` | `timestamp_us` | `micros()` no inicio da rodada de aquisicao |
| 6 | 2 | `uint16` | `sequence` | Sequencia da rodada, com retorno a zero apos 65535 |
| 8 | 36 | `18 x int16` | `values` | Acelerometro e giroscopio dos tres ICMs |
| 44 | 1 | `uint8` | `crc8` | CRC dos bytes 0 a 43 |

Formato equivalente no Python:

```python
PACKET_FMT = "<HIH18hB"
PACKET_SIZE = 45
```

A ordem dos 18 valores e:

```text
icm0_ax, icm0_ay, icm0_az, icm0_gx, icm0_gy, icm0_gz,
icm1_ax, icm1_ay, icm1_az, icm1_gx, icm1_gy, icm1_gz,
icm2_ax, icm2_ay, icm2_az, icm2_gx, icm2_gy, icm2_gz
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

## Escalas

As faixas configuradas sao `+/-2 g` e `+/-250 graus/s`. O Python aplica:

```text
aceleracao_m_s2 = raw * ((2 * 9.80665) / 32767.5)
giroscopio_dps  = raw * (250 / 32768)
```

Nao ha magnetometro neste pacote.

## Temporizacao e perdas

O agendador inicia uma rodada a cada 10000 us, correspondente a 100 Hz. Os tres
sensores compartilham um multiplexador e, portanto, sao lidos sequencialmente
dentro da mesma rodada; o timestamp e comum, mas as leituras nao sao fisicamente
simultaneas.

Se o loop perder um ou mais periodos, nao executa leituras em rajada. A sequencia
avanca pelos periodos perdidos. Se qualquer ICM falhar por I2C, a rodada inteira
e descartada e a sequencia tambem avanca. Uma queda de pacote na transmissao USB
produz o mesmo tipo de gap observavel pelo parser.

O firmware mantem separadamente os contadores:

- periodos de aquisicao perdidos;
- rodadas descartadas;
- falhas I2C por ICM;
- pacotes descartados por falta de espaco no buffer USB.

Esses contadores ainda nao fazem parte do pacote v1. Eles serao usados pelo
registro `status.txt` quando a etapa do cartao SD for implementada.

## Volume e captura

A 100 Hz, o fluxo nominal e de 4500 bytes/s. Uma janela de 10 segundos sem
perdas contem 1000 pacotes e 45000 bytes.

`python/capture_serial.py` sincroniza no primeiro pacote valido e salva uma
janela semiaberta de tempo do sensor: inclui pacotes com timestamp entre `t0` e
`t0 + 10 s`, excluindo o pacote no limite final. O arquivo `.bin` preserva os
bytes recebidos sem conversao. Um arquivo `.bin.json` registra porta, horario,
SHA-256 e contadores do parser para rastreabilidade.

Os arquivos criados em `data/` sao capturas de bancada no computador e nao
alteram a futura organizacao de sessoes do cartao SD.

## Validacao e visualizacao

`python/analyze_imu.py` usa apenas pacotes que passaram por magic e CRC. A
ferramenta informa pacotes validos, CRCs invalidos, bytes descartados, gaps de
sequencia, frequencia efetiva, periodo medio e limites dos intervalos. O jitter
e definido como o desvio-padrao populacional dos intervalos entre timestamps,
em milissegundos; ele nao deve ser confundido com o periodo medio de 10 ms.

O grafico gerado tem duas linhas e tres colunas. Cada coluna representa um ICM;
acelerometros aparecem acima e giroscopios abaixo, ja convertidos para unidades
fisicas pelo Python. Essa ferramenta nao altera o fluxo bruto nem o pacote v1.

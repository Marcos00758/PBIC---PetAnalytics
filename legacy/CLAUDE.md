# CLAUDE.md — Coleira Inteligente para Cães (Nicla Voice)

## Objetivo
Wearable para cães: captura contínua de IMU 9-eixos (×3 ICM-20948) + 2× barômetro BMP390 + áudio, para dataset de ML (classificação de comportamentos caninos). TCC/PBIC.

Pipeline (autônomo): `Nicla Voice → grava no SD (/Sxxx/imu.bin) → Python lê o .bin → parse/visualização`
Stream serial existe em paralelo (não-bloqueante), mas a coleta NÃO depende de PC conectado.

**NÃO implementar agora:** FFT embarcada, MFCC, TinyML, classificação real-time. Foco: **coleta robusta de dados**.

## Stack
- Firmware: Arduino C++17, ArduinoCore-mbed, nRF52832
- IA embarcada: Syntiant NDP120 (`NDP.h`)
- Sensores internos: BMI270 + BMM150 (via `NDP.sensorBMI270/BMM150Read/Write()`)
- Externos: 3× ICM-20948 + 2× BMP390 via I2C (Wire) + **PCA9548A** (mux), conectados por **conectores JST SH 4 pinos** (não mais fios soltos)
- Armazenamento: cartão SD no J1 (SPI externo)
- Host: Python 3.x — numpy, matplotlib (pyserial só se for usar o stream ao vivo)
- Build: arduino-cli

## Arquitetura de Hardware

- **nRF52832** (Cortex-M4 64MHz) executa o sketch
- **NDP120**: SPI interno separado (não exposto), conecta BMI270/BMM150/mic PDM — NÃO tocar

### SPI (`SPI_HOWMANY=2`)
- **J1 externo (livre)**: MOSI=8(P0_27) MISO=7(P0_28) SCK=9(P0_11) CS=6(P0_29) → usar para SD
- **Interno NDP120/Flash**: CS Flash=P0_26(pino16) — não exposto, NÃO tocar

### I2C (`WIRE_HOWMANY=2`)
- **Wire (J2, livre)**: SDA=4(P0_22) SCL=3(P0_23) → `Wire.begin()`

### PCA9548A (mux I2C) — endereço 0x70
Idêntico em uso ao TCA9548A. Conexões por JST SH 4 pinos (SDA/SCL/VCC/GND).

| Canal | Sensor | Addr | Status |
|---|---|---|---|
| 0 | ICM-20948 #0 | 0x69 | ✅ |
| 1 | ICM-20948 #1 | 0x69 | ✅ |
| 2 | BMP390 #0 | 0x76/0x77 (autodetect) | ✅ |
| 3 | BMP390 #1 | 0x76/0x77 (autodetect) | ✅ |
| 4 | ICM-20948 #2 | 0x69 | ✅ |

- Trocar canal: `Wire.beginTransmission(0x70); Wire.write(1<<canal); Wire.endTransmission();`
- **Após `select`, dar `delayMicroseconds(80)`** antes de ler — sem isso há leituras de bytes parciais (spikes no gyro). Escrever um canal novo desativa o anterior (não precisa deselect entre leituras consecutivas).

### Mapa de pinos usáveis
| Arduino | nRF | Uso |
|---|---|---|
| 3 | P0_23 | SCL (J2) |
| 4 | P0_22 | SDA (J2) |
| 6 | P0_29 | CS SPI ext (SD) |
| 7 | P0_28 | MISO SPI ext |
| 8 | P0_27 | MOSI SPI ext |
| 9 | P0_11 | SCK SPI ext |
| 10(A0) | P0_2 | ADC1 |
| 11(A1) | P0_30 | ADC2 |
| 0,1,2,5 | — | GPIO livre |
| 12 | P0_19 | INT ESLOV |

**Reservados — NÃO usar:** 13, 14, 15, 16, 17

## Padrões C++

**Pinos**: usar inteiros (`6`,`7`...). NUNCA `D0`/`D1`/`PIN_SPI1_*`.

**LED**:
```cpp
#include "Nicla_System.h"
nicla::begin(); nicla::leds.begin(); nicla::leds.setColor(red);
// NUNCA pinMode(LED_BUILTIN,...)
```

**BMI270/BMM150** — sempre via NDP, nunca `Arduino_BMI270_BMM150` (accelerationAvailable() sempre false):
```cpp
NDP.sensorBMI270Read/Write(reg, len, buf);
NDP.sensorBMM150Read/Write(reg, len, buf);
```

**Init obrigatória no setup()**:
```cpp
NDP.begin("mcu_fw_120_v91.synpkg");
NDP.load("dsp_firmware_v91.synpkg");
// áudio: NDP.load("alexa_334_NDP120_B0_v11_v91.synpkg");
```

**Fatores de escala (não alterar sem datasheet)**:
```cpp
constexpr float BMI_ACCEL_SCALE = (2.0f*9.80665f)/32767.5f;  // ±2g
constexpr float BMI_GYRO_SCALE  = 1000.0f/32768.0f;          // ±1000dps, reg 0x43=0x11
constexpr float MAG_XY_SCALE = 1300.0f/4096.0f;   // >>3
constexpr float MAG_Z_SCALE  = 2500.0f/16384.0f;  // >>1
constexpr float ICM_ACCEL_SCALE = (2.0f*9.80665f)/32767.5f; // ±2g
constexpr float ICM_GYRO_SCALE  = 250.0f/32768.0f;          // ±250dps
// BMP390: pressão em Pa via compensação oficial Bosch (não linear, usa coeficientes NVM)
// temperatura em °C via mesma rotina de compensação
```
Precedência: `(int16_t)(raw >> 3) * SCALE` — NUNCA `raw >> 3 * SCALE`.

**ICM-20948**: addr 0x68/0x69, WHO_AM_I reg 0x00 = 0xEA, dados reg 0x2D (12 bytes, **big-endian**, ao contrário do BMI270).

**BMP390**: addr 0x76/0x77 (pino SDO), CHIP_ID reg 0x00 = 0x60. Soft-reset: `CMD`(0x7E)=0xB6. Config: `PWR_CTRL`(0x1B), `OSR`(0x1C), `ODR`(0x1D). Dados: regs 0x04–0x09 (press 0x04-0x06, temp 0x07-0x09), 24-bit unsigned, little-endian. Coeficientes de calibração em 0x31–0x45 (NVM trim) — aplicar fórmula de compensação Bosch oficial.

**Pacote binário — ATUAL: 79 bytes** (conversão para unidades físicas no Python, não no MCU — reduz throughput USB e tira carga do loop). Gyros já vêm **bias-corrigidos** no MCU; BMM150 já vem **pós-shift** (XY >>3, Z >>1); BMP390 vai **raw 24-bit** (compensação Bosch no Python usando NVM do meta.txt).
```cpp
#pragma pack(push,1)
struct ImuPacket {
  uint16_t magic;            // 0xAA55
  uint32_t timestamp_us;
  int16_t ax_i,ay_i,az_i, gx_i,gy_i,gz_i, mx_i,my_i,mz_i; // BMI270+BMM150
  int16_t ax_0,ay_0,az_0, gx_0,gy_0,gz_0;                 // ICM#0 (canal 0)
  int16_t ax_1,ay_1,az_1, gx_1,gy_1,gz_1;                 // ICM#1 (canal 1)
  int16_t ax_2,ay_2,az_2, gx_2,gy_2,gz_2;                 // ICM#2 (canal 4)
  uint32_t press_0,temp_0;   // BMP390 #0 (canal 2) raw 24-bit
  uint32_t press_1,temp_1;   // BMP390 #1 (canal 3) raw 24-bit
  uint16_t seq;
  uint8_t  crc8;
};
#pragma pack(pop)
static_assert(sizeof(ImuPacket)==79, "");
```
Python: `PACKET_FMT = "<HI" + "h"*27 + "I"*4 + "HB"`, `PACKET_SIZE == 79`.
BMP390 lido a ~25Hz (decimado, cache nos demais ciclos). Coeficientes NVM (21 bytes) gravados em hex no `meta.txt` (`bmp0_nvm=`, `bmp1_nvm=`); `testIMUICM.py` aplica a compensação Bosch float (`bmp390_coeffs` + `bmp390_compensate`).

**CRC-8 (poly 0x07)**:
```cpp
uint8_t crc8_calc(const uint8_t* d, size_t len){
  uint8_t crc=0;
  for(size_t i=0;i<len;i++){ crc^=d[i];
    for(uint8_t b=0;b<8;b++) crc=(crc&0x80)?(uint8_t)((crc<<1)^0x07):(uint8_t)(crc<<1); }
  return crc;
}
// sobre todos os bytes EXCETO o crc8 final
```

**Operação autônoma (sem handshake)**: o sketch grava no SD assim que liga, não espera comando do PC. O handshake `IMU_READY`/`'S'`/`STREAM_BIN_START` foi **removido**. Stream serial é opcional e não-bloqueante:
```cpp
if (Serial && Serial.availableForWrite() >= (int)sizeof(pkt))
  Serial.write((uint8_t*)&pkt, sizeof(pkt));  // pula se buffer cheio (host não lê) — não trava o loop
```

**Ordem crítica no setup()**: `SD.begin(6)` ANTES de `Wire.begin()`/`NDP.begin()`. Inicializar o SD depois deixa o SPI em estado ruim e `SD.begin()` falha silenciosamente.

**SD no J1** (livre, sem conflito): `SD.begin(6)`. Abrir `imu.bin` UMA VEZ no setup, `flush()` periódico — nunca close+open no loop.

**Estrutura de sessão no SD** (contador persistente em `/session.txt`, incrementado a cada boot):
```
/session.txt           ← inteiro, última sessão usada
/Sxxx/imu.bin          ← stream binário de ImuPacket (aberto toda a sessão)
/Sxxx/meta.txt         ← session, boot_micros, packet_size, sample_rate
/Sxxx/audio.g722       ← placeholder vazio (áudio futuro)
```
- Flush a cada `PACKETS_PER_FLUSH`; rotação de pasta a cada `PACKETS_PER_ROTATION` (12h a 100Hz = 4.320.000).
- **Detecção de remoção do SD**: por janela — se em ~2s houve tentativas de escrita e NENHUMA teve sucesso → cartão removido (LED vermelho, fecha arquivo). NÃO usar contagem de falhas consecutivas: o erase normal do cartão retorna 0 transitoriamente e dispara falso-positivo. `SD.exists()` no diretório NÃO detecta remoção (retorna cache).

**Calibração de bias dos gyros** no boot (LED azul, ~2s, sensor parado): média de N amostras em raw counts, subtraída no loop com saturação int16. Manter a coleira parada nesse período.

**Timing — agendador anti-rajada** (essencial com SD): após somar o período, se já passou do deadline (flush do SD bloqueou), ressincronizar em vez de disparar amostras em rajada para "recuperar":
```cpp
nextSampleUs += PERIOD;
if ((int32_t)(now - nextSampleUs) >= 0) nextSampleUs = now + PERIOD;  // sem isto: fs>100Hz e jitter dispara
```

## Padrões Python
`testIMUICM.py` lê o `.bin` direto do SD (não captura serial). Uso: `python testIMUICM.py "E:/S001/imu.bin"`.
```python
MAGIC = 0xAA55
PACKET_FMT = "<HI" + "h"*27 + "HB"  # 63 bytes — atualizar conforme struct C++ atual
PACKET_SIZE = struct.calcsize(PACKET_FMT)

def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc<<1)^0x07)&0xFF if (crc&0x80) else (crc<<1)&0xFF
    return crc

# parse: buscar magic, validar CRC antes de aceitar, avançar 1 byte se inválido
# escalas aplicadas no host (dados chegam raw int16) — ver to_dataframe()
```
**Otimização (só quando sessões longas, milhões de pacotes)**: vetorizar com numpy (reshape (N,63) + CRC por tabela coluna-a-coluna). Para arquivos de teste pequenos, o parser atual em Python puro basta.

## Regras de Comportamento
1. Não sugerir `Arduino_BMI270_BMM150`, `PDMClass`, `pinMode(LED_BUILTIN)`, `D0`/`D1`, `PIN_SPI1_*`
2. SD no J1 NÃO é problema — é SPI externo livre, CS=6
3. Sempre `NDP.begin()`+`NDP.load()` no setup antes de qualquer sensor
4. Sempre `nicla::leds`, nunca `digitalWrite` para LED
5. Não alterar fatores de escala sem base em datasheet
6. Avisar se usar pinos reservados (13-17)
7. Confirmar endereços I2C de cada ICM/BMP390 (canal do mux) se não estiver claro
8. USB-CDC: throughput real ~11KB/s independe do baud — não sugerir aumentar baud para resolver throughput
9. Sempre que mudar a struct do pacote, atualizar `PACKET_FMT`/`PACKET_SIZE`/dtype no Python e o `static_assert` no C++
10. Pedir link do GitHub se faltar info sobre lib/registrador não documentado aqui
11. `SD.begin()` SEMPRE antes de `Wire.begin()`/`NDP.begin()` no setup
12. Detecção de remoção do SD por janela de sucesso, nunca por falhas consecutivas (erase dá falso-positivo)

## Status
| Componente | Status |
|---|---|
| BMI270/BMM150 | ✅ OK |
| 3× ICM-20948 (canais 0,1,4) | ✅ OK (spikes residuais ~±2.5°/s, toleráveis) |
| AK09916 (mag dos ICMs) | ❌ Não implementado (placeholder no plot) |
| 2× BMP390 (canais 2,3) | ✅ OK (raw 24-bit, ~25Hz, compensação no Python) |
| PCA9548A (mux) | ✅ OK |
| SD via J1 (sessões /Sxxx/) | ✅ OK |
| Detecção remoção SD → LED vermelho | ⚠️ A validar no hardware |
| Streaming serial (não-bloqueante) | ✅ OK |
| Anti-rajada + bias gyro | ✅ OK (fs≈100Hz) |
| CRC/seq | ✅ OK |

## Próximos Passos
1. AK09916 (mag interno dos ICMs)
2. Validar/ajustar detecção de remoção do SD (LED vermelho)
3. Áudio G722 + IMU com timestamp sincronizado
4. (Quando sessões longas) parser numpy vetorizado
5. Validar pressão BMP390 contra referência (≈1013 hPa nível do mar)

## Build/Teste
```bash
arduino-cli compile --fqbn arduino:mbed_nicla:nicla_voice .
arduino-cli upload  --fqbn arduino:mbed_nicla:nicla_voice --port /dev/ttyACM0 .
# Linux: ls /dev/ttyACM*  | Windows: ver COMx no Gerenciador de Dispositivos
```
1. Ligar → LED azul (SD + sessão + calibração de bias, manter parado ~2s) → LED verde (gravando)
2. Movimentar / coletar → desligar
3. Tirar o SD, copiar `/Sxxx/imu.bin` (ou apontar direto) → `python testIMUICM.py "E:/Sxxx/imu.bin"`
4. Checklist: pacotes válidos>0, CRC inválido=0, magic inválido=0, sem gaps, fs≈100Hz, jitter baixo
5. Sanity: az≈±9.8 m/s² (todos os 3 ICMs + BMI270), gz≈0; (BMP390 futuro: pressão≈~1013 hPa)

## Armadilhas Críticas
| Erro | Consequência | Correto |
|---|---|---|
| `Arduino_BMI270_BMM150` | accelerationAvailable() sempre false | `NDP.sensorBMI270Read/Write()` |
| `GYRO_SCALE=1/16.4` | 8× errado | `1000.0/32768.0`, reg 0x43=0x11 |
| `pinMode(LED_BUILTIN)` | erro de compilação | `nicla::leds.setColor()` |
| `D0`,`D1`,`PIN_SPI1_*` | não declarado | inteiros / `SPI`,`SPI1` |
| `raw>>3*scale` | precedência errada | `(int16_t)(raw>>3)*scale` |
| "SD no J1 = conflito" | FALSO | J1 é SPI externo livre |
| close+open no loop (SD) | dados com buracos | abrir 1x, flush periódico |
| `SD.begin()` depois de Wire/NDP | SD falha silenciosamente | `SD.begin()` ANTES de Wire/NDP |
| ICM big-endian vs BMI little-endian | dados corrompidos | ICM: `b[0]<<8\|b[1]`, BMI: `b[0]\|b[1]<<8` |
| BMP390 sem compensação NVM | pressão/temp errados | aplicar fórmula oficial Bosch com trim coefs |
| `pca_select` sem `delayMicroseconds(80)` | spikes no gyro (bytes parciais) | settle de 80µs após trocar canal |
| agendador catch-up com SD | fs>100Hz, jitter alto (rajadas) | ressincronizar nextSampleUs (anti-rajada) |
| `Serial.write` bloqueante | trava loop se host não lê | guard com `availableForWrite()` |
| detecção SD por N falhas seguidas | falso-positivo no erase | detecção por janela de sucesso (~2s) |
| pacote float no MCU | throughput USB alto + CPU no loop | raw int16, conversão no Python |
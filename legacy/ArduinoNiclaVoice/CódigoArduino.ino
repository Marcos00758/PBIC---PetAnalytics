#include "BMI270_Init.h"
#include "NDP.h"
#include "Nicla_System.h"
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#define PCA_ADDR 0x70 // PCA9548A I2C mux (idêntico ao TCA9548A)
// ICM-20948: endereço autodetectado por canal (0x69 AD0=VCC ou 0x68 AD0=GND)
#define BMI_ACCEL_REG 0x0C
#define BMM150_DATA_REG 0x42

// ── Canais do PCA9548A ────────────────────────────────────────────────────────
// ICM-20948 → canais 0, 1, 4 | BMP390 → canais 2, 3 (a integrar)
#define ICM0_CH 0
#define ICM1_CH 1
#define ICM2_CH 4
#define BMP0_CH 2 // BMP390 #0 — leitura a implementar
#define BMP1_CH 3 // BMP390 #1 — leitura a implementar

// ── BMP390 ────────────────────────────────────────────────────────────────────
#define BMP_ADDR_HI 0x77    // SDO=VCC
#define BMP_ADDR_LO 0x76    // SDO=GND  (autodetecta entre os dois)
#define BMP_CHIP_ID 0x60    // valor esperado em reg 0x00
#define BMP_REG_CHIPID 0x00
#define BMP_REG_DATA 0x04   // press 0x04-0x06, temp 0x07-0x09 (24-bit LE)
#define BMP_REG_PWR_CTRL 0x1B
#define BMP_REG_OSR 0x1C
#define BMP_REG_ODR 0x1D
#define BMP_REG_CONFIG 0x1F
#define BMP_REG_CMD 0x7E
#define BMP_REG_NVM 0x31    // coeficientes de calibração: 21 bytes (0x31-0x45)
#define BMP_NVM_LEN 21

#define SD_CS_PIN 6                  // J1 SPI externo
#define IMU_SAMPLE_PERIOD_US 10000UL // 100 Hz
#define MAG_DECIMATE 10
#define BMP_DECIMATE 4              // lê BMP390 a cada 4 ciclos (~25Hz)
#define PACKETS_PER_FLUSH 1000UL    // 10s a 100Hz
#define PACKETS_PER_ROTATION 5000UL // 50s a 100Hz
#define PACKETS_PER_SD_CHECK 3000UL // 2s a 100Hz — verifica presença do SD

// ── Pacote binário — 79 bytes (raw int16 + BMP390 raw 24-bit) ────────────────
// press/temp são valores brutos 24-bit do BMP390 (em uint32). A compensação
// Bosch (não-linear, usa coeficientes NVM em meta.txt) é feita no Python.
#pragma pack(push, 1)
struct ImuPacket {
  uint16_t magic;
  uint32_t timestamp_us;
  int16_t ax_i, ay_i, az_i; // BMI270
  int16_t gx_i, gy_i, gz_i;
  int16_t mx_i, my_i, mz_i; // BMM150
  int16_t ax_0, ay_0, az_0; // ICM#0 (canal 0)
  int16_t gx_0, gy_0, gz_0;
  int16_t ax_1, ay_1, az_1; // ICM#1 (canal 1)
  int16_t gx_1, gy_1, gz_1;
  int16_t ax_2, ay_2, az_2; // ICM#2 (canal 4)
  int16_t gx_2, gy_2, gz_2;
  uint32_t press_0, temp_0; // BMP390 #0 (canal 2) raw 24-bit
  uint32_t press_1, temp_1; // BMP390 #1 (canal 3) raw 24-bit
  uint16_t seq;
  uint8_t crc8;
};
#pragma pack(pop)
static_assert(sizeof(ImuPacket) == 79, "Tamanho inesperado do pacote");

constexpr uint16_t PACKET_MAGIC = 0xAA55;
uint16_t seqCounter = 0;
uint8_t magDecCounter = 0;
uint32_t nextSampleUs = 0;

int16_t mx_cache = 0, my_cache = 0, mz_cache = 0;
int16_t gx_bias_i = 0, gy_bias_i = 0, gz_bias_i = 0;
int16_t gx_bias_0 = 0, gy_bias_0 = 0, gz_bias_0 = 0;
int16_t gx_bias_1 = 0, gy_bias_1 = 0, gz_bias_1 = 0;
int16_t gx_bias_2 = 0, gy_bias_2 = 0, gz_bias_2 = 0;

// Endereço I2C de cada ICM (idx 0=canal0, 1=canal1, 2=canal4) — autodetectado
uint8_t icm_addr[3] = {0x69, 0x69, 0x69};

// ── BMP390: endereço detectado, coeficientes NVM e cache (decimado ~25Hz) ─────
uint8_t bmp_addr[2] = {0, 0};           // [0]=canal 2, [1]=canal 3 (0 = ausente)
uint8_t bmp_nvm[2][BMP_NVM_LEN];        // coeficientes brutos, vão p/ meta.txt
uint8_t bmpDecCounter = 0;
uint32_t press0_cache = 0, temp0_cache = 0;
uint32_t press1_cache = 0, temp1_cache = 0;

// ── Estado da sessão SD
// ───────────────────────────────────────────────────────
uint32_t sessionNumber = 0;
char sessionFolder[16] = {0}; // "/Sxxxxx"
File imuFile;
uint32_t packetsInSession = 0;
uint32_t packetsSinceFlush = 0;
uint16_t packetsSinceSdCheck = 0;
bool sdWroteSinceCheck = false; // alguma escrita teve sucesso na janela atual?
bool sdAttemptedWrite = false;  // tentou escrever na janela atual?
bool sdReady = false;

#define CHECK_STATUS(s)                                                        \
  do {                                                                         \
    if (s) {                                                                   \
      Serial.print("SPI error linha ");                                        \
      Serial.println(__LINE__);                                                \
      nicla::leds.setColor(red);                                               \
      while (1)                                                                \
        ;                                                                      \
    }                                                                          \
  } while (0)

uint8_t crc8_calc(const uint8_t *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static inline int16_t read_i16_le(const uint8_t *p) {
  return (int16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
}
static inline int16_t read_i16_be(const uint8_t *p) {
  return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static inline int16_t sub_sat_i16(int16_t a, int16_t b) {
  int32_t r = (int32_t)a - (int32_t)b;
  if (r > 32767)
    return 32767;
  if (r < -32768)
    return -32768;
  return (int16_t)r;
}

// ── PCA9548A (mux I2C, idêntico ao TCA9548A) ──────────────────────────────────
void pca_select(uint8_t ch) {
  Wire.beginTransmission(PCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
  delayMicroseconds(80);
}
void pca_deselect() {
  Wire.beginTransmission(PCA_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

// ── ICM-20948 (endereço por canal — autodetectado 0x69/0x68) ─────────────────
void icm_write(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
uint8_t icm_read(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}
void icm_read_bytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, len);
  for (int i = 0; i < len; i++)
    buf[i] = Wire.available() ? Wire.read() : 0;
}
void icm_select_bank(uint8_t addr, uint8_t bank) {
  icm_write(addr, 0x7F, bank << 4);
  delay(5);
}

void icm_init(uint8_t addr) {
  icm_select_bank(addr, 0);
  icm_write(addr, 0x06, 0x80);
  delay(100);
  icm_write(addr, 0x06, 0x01);
  delay(50);
  icm_write(addr, 0x07, 0x00);
  icm_select_bank(addr, 2);
  // DLPF 51.2Hz para gyro e accel — apropriado para Nyquist de 50Hz (amostragem
  // 100Hz). Reduz spikes residuais do ICM#0 sem alterar sample rate.
  icm_write(addr, 0x01, 0x19); // GYRO_CONFIG_1: DLPFCFG=011 (51.2Hz), ±250dps
  icm_write(addr, 0x00, 0x00);
  icm_write(addr, 0x14, 0x19); // ACCEL_CONFIG: DLPFCFG=011 (~50.4Hz), ±2g
  icm_write(addr, 0x10, 0x00);
  icm_write(addr, 0x11, 0x00);
  icm_select_bank(addr, 0);
  delay(100);
}

// Scan I2C de todos os canais do mux — diagnóstico de fiação/endereços.
void i2c_scan_all() {
  if (!Serial)
    return;
  for (uint8_t ch = 0; ch < 8; ch++) {
    pca_select(ch);
    Serial.print("  canal ");
    Serial.print(ch);
    Serial.print(":");
    bool any = false;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.print(" 0x");
        Serial.print(a, HEX);
        any = true;
      }
    }
    Serial.println(any ? "" : " (vazio)");
  }
  pca_deselect();
}

// Seleciona o canal, autodetecta endereço (0x69/0x68), inicializa e valida
// WHO_AM_I (0xEA). Em falha: imprime scan de todos os canais e halt (LED red).
void icm_init_check(uint8_t ch, uint8_t idx, const char *name) {
  pca_select(ch);

  uint8_t addr = 0;
  if (icm_read(0x69, 0x00) == 0xEA)
    addr = 0x69;
  else if (icm_read(0x68, 0x00) == 0xEA)
    addr = 0x68;

  if (!addr) {
    if (Serial) {
      Serial.print("ERRO: ");
      Serial.print(name);
      Serial.print(" nao detectado no canal ");
      Serial.println(ch);
      Serial.println("Scan I2C de todos os canais:");
      i2c_scan_all();
    }
    nicla::leds.setColor(red);
    while (1)
      ;
  }

  icm_addr[idx] = addr;
  icm_init(addr);
  if (Serial) {
    Serial.print(name);
    Serial.print(" OK addr=0x");
    Serial.println(addr, HEX);
  }
  pca_deselect();
}

// ── I2C genérico (endereço explícito) — usado pelo BMP390 ─────────────────────
void i2c_write8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
uint8_t i2c_read8(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}
void i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, len);
  for (int i = 0; i < len; i++)
    buf[i] = Wire.available() ? Wire.read() : 0;
}

static inline uint32_t read_u24_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

// ── BMP390 ────────────────────────────────────────────────────────────────────
// Seleciona o canal do mux, detecta o endereço (0x77/0x76), faz soft-reset,
// valida CHIP_ID, lê os coeficientes NVM (p/ meta.txt) e configura modo normal.
// Retorna o endereço detectado (0 se ausente). idx: 0=canal2, 1=canal3.
uint8_t bmp_init(uint8_t ch, uint8_t idx, const char *name) {
  pca_select(ch);

  uint8_t addr = 0;
  if (i2c_read8(BMP_ADDR_HI, BMP_REG_CHIPID) == BMP_CHIP_ID)
    addr = BMP_ADDR_HI;
  else if (i2c_read8(BMP_ADDR_LO, BMP_REG_CHIPID) == BMP_CHIP_ID)
    addr = BMP_ADDR_LO;

  if (!addr) {
    if (Serial) {
      Serial.print("ERRO: ");
      Serial.print(name);
      Serial.println(" nao detectado (BMP390)");
    }
    nicla::leds.setColor(red);
    while (1)
      ;
  }

  // soft-reset
  i2c_write8(addr, BMP_REG_CMD, 0xB6);
  delay(10);

  // coeficientes de calibração (brutos → compensação no Python)
  i2c_read_bytes(addr, BMP_REG_NVM, bmp_nvm[idx], BMP_NVM_LEN);

  // config: OSR press ×8 / temp ×1; ODR 25Hz; IIR off (dados crus); modo normal
  i2c_write8(addr, BMP_REG_OSR, 0x03);      // osr_p=011(×8), osr_t=000(×1)
  i2c_write8(addr, BMP_REG_ODR, 0x03);      // ODR 25Hz (período 40ms)
  i2c_write8(addr, BMP_REG_CONFIG, 0x00);   // IIR bypass
  i2c_write8(addr, BMP_REG_PWR_CTRL, 0x33); // press_en+temp_en+modo normal
  delay(50);

  if (Serial) {
    Serial.print(name);
    Serial.print(" OK addr=0x");
    Serial.println(addr, HEX);
  }
  pca_deselect();
  return addr;
}

// Lê press+temp brutos de um BMP390 (canal já selecionado no mux).
void bmp_read(uint8_t addr, uint32_t *press, uint32_t *temp) {
  uint8_t d[6];
  i2c_read_bytes(addr, BMP_REG_DATA, d, 6);
  *press = read_u24_le(&d[0]);
  *temp = read_u24_le(&d[3]);
}

// ── Sessão SD (contador em /session.txt na raiz) ─────────────────────────────
uint32_t load_and_increment_session() {
  uint32_t n = 0;
  if (SD.exists("/session.txt")) {
    File f = SD.open("/session.txt", FILE_READ);
    if (f) {
      char buf[16] = {0};
      int len = f.readBytes(buf, sizeof(buf) - 1);
      (void)len;
      n = (uint32_t)strtoul(buf, nullptr, 10);
      f.close();
    }
  }
  n++;
  SD.remove("/session.txt");
  File f = SD.open("/session.txt", FILE_WRITE);
  if (f) {
    f.print(n);
    f.close();
  }
  return n;
}

void start_session(uint32_t n) {
  snprintf(sessionFolder, sizeof(sessionFolder), "/S%03lu", (unsigned long)n);
  SD.mkdir(sessionFolder); // ignora retorno: já existir é OK

  char path[32];

  // meta.txt
  snprintf(path, sizeof(path), "%s/meta.txt", sessionFolder);
  File meta = SD.open(path, FILE_WRITE);
  if (meta) {
    meta.print("session=");
    meta.println(n);
    meta.print("boot_micros=");
    meta.println(micros());
    meta.print("packet_size=");
    meta.println((int)sizeof(ImuPacket));
    meta.print("sample_rate=");
    meta.println(1000000UL / IMU_SAMPLE_PERIOD_US);
    meta.print("bmp_rate=");
    meta.println((1000000UL / IMU_SAMPLE_PERIOD_US) / BMP_DECIMATE);
    // Coeficientes NVM do BMP390 (hex) — Python aplica a compensação Bosch.
    for (int s = 0; s < 2; s++) {
      meta.print("bmp");
      meta.print(s);
      meta.print("_addr=0x");
      meta.println(bmp_addr[s], HEX);
      meta.print("bmp");
      meta.print(s);
      meta.print("_nvm=");
      for (int i = 0; i < BMP_NVM_LEN; i++) {
        if (bmp_nvm[s][i] < 0x10)
          meta.print('0');
        meta.print(bmp_nvm[s][i], HEX);
      }
      meta.println();
    }
    meta.close();
  } else {
    if (Serial)
      Serial.println("ERRO: nao abriu meta.txt");
  }

  // audio.g722 — placeholder vazio para implementação futura
  snprintf(path, sizeof(path), "%s/audio.g722", sessionFolder);
  File audio = SD.open(path, FILE_WRITE);
  if (audio)
    audio.close();

  // imu.bin — fica aberto durante toda a sessão
  snprintf(path, sizeof(path), "%s/imu.bin", sessionFolder);
  imuFile = SD.open(path, FILE_WRITE);
  if (!imuFile) {
    if (Serial) {
      Serial.print("ERRO: nao abriu ");
      Serial.println(path);
    }
    nicla::leds.setColor(red);
    while (1)
      ;
  }

  packetsInSession = 0;
  packetsSinceFlush = 0;
}

void rotate_session() {
  if (imuFile) {
    imuFile.flush();
    imuFile.close();
  }
  sessionNumber = load_and_increment_session();
  start_session(sessionNumber);
}

// Chamado quando o cartão é removido ou a escrita falha: para de gravar,
// fecha o arquivo e sinaliza erro com LED vermelho.
void handle_sd_lost() {
  sdReady = false;
  if (imuFile)
    imuFile.close();
  nicla::leds.setColor(red);
  if (Serial)
    Serial.println("ERRO: SD removido / falha de escrita");
}

void setup() {
  nicla::begin();
  nicla::leds.begin();
  Serial.begin(115200);
  delay(2000); // tempo para Serial + estabilizar VCC do SD

  // ── SD card PRIMEIRO (antes de NDP/Wire) ─────────────────────────────────
  // sdCard.ino mostra que SD.begin funciona em boot limpo. Inicializar SD
  // depois de NDP.begin/Wire.begin deixava o SPI em estado ruim e SD.begin
  // falhava silenciosamente (ou abria arquivos inválidos sem erro visível).
  nicla::leds.setColor(blue);
  if (Serial)
    Serial.println("Inicializando SD...");
  sdReady = SD.begin(SD_CS_PIN);
  if (!sdReady) {
    if (Serial)
      Serial.println("ERRO: SD card nao detectado");
    nicla::leds.setColor(red);
    while (1)
      ;
  }
  if (Serial)
    Serial.println("SD OK");

  // ── Agora I2C e NDP ──────────────────────────────────────────────────────
  // A sessão (start_session) é aberta MAIS ABAIXO, após inicializar os BMP390,
  // para que os coeficientes NVM já estejam disponíveis ao escrever meta.txt.
  Wire.begin();
  Wire.setClock(400000);
  delay(100);
  pca_deselect();

  NDP.begin("mcu_fw_120_v91.synpkg");
  NDP.load("dsp_firmware_v91.synpkg");

  // ── BMI270 ────────────────────────────────────────────────────────────────
  int status;
  uint8_t __attribute__((aligned(4))) sd[16];

  status = NDP.sensorBMI270Read(0x00, 1, sd);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Read(0x00, 1, sd);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x7E, 0xB6);
  CHECK_STATUS(status);
  delay(20);
  status = NDP.sensorBMI270Read(0x00, 1, sd);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Read(0x00, 1, sd);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x7C, 0x00);
  CHECK_STATUS(status);
  delay(20);
  status = NDP.sensorBMI270Write(0x59, 0x00);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x5E, sizeof(bmi270_maximum_fifo_config_file),
                                 (uint8_t *)bmi270_maximum_fifo_config_file);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x59, 0x01);
  CHECK_STATUS(status);
  delay(200);
  status = NDP.sensorBMI270Read(0x21, 1, sd);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x40, 0xA8);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x41, 0x00);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x42, 0xA9);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x43, 0x11);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x7D, 0x0E);
  CHECK_STATUS(status);
  status = NDP.sensorBMI270Write(0x7C, 0x02);
  CHECK_STATUS(status);

  // ── BMM150 ────────────────────────────────────────────────────────────────
  status = NDP.sensorBMM150Write(0x4B, 0x01);
  CHECK_STATUS(status);
  delay(10);
  status = NDP.sensorBMM150Write(0x4B, 0x83);
  CHECK_STATUS(status);
  delay(10);
  status = NDP.sensorBMM150Write(0x4B, 0x01);
  CHECK_STATUS(status);
  delay(10);
  status = NDP.sensorBMM150Write(0x4C, 0x00);
  CHECK_STATUS(status);
  status = NDP.sensorBMM150Write(0x4E, 0x84);
  CHECK_STATUS(status);
  delay(100);

  // ── ICM-20948 #0, #1, #2 (canais 0, 1, 4) ─────────────────────────────────
  icm_init_check(ICM0_CH, 0, "ICM#0");
  icm_init_check(ICM1_CH, 1, "ICM#1");
  icm_init_check(ICM2_CH, 2, "ICM#2");

  // ── BMP390 #0, #1 (canais 2, 3) ───────────────────────────────────────────
  bmp_addr[0] = bmp_init(BMP0_CH, 0, "BMP#0");
  bmp_addr[1] = bmp_init(BMP1_CH, 1, "BMP#1");

  // ── Calibração de bias dos gyros ──────────────────────────────────────────
  if (Serial)
    Serial.println("CALIBRATING_GYRO_BIAS");
  nicla::leds.setColor(blue);
  delay(500);

  constexpr int CAL_N = 200;
  int32_t sx_i = 0, sy_i = 0, sz_i = 0;
  int32_t sx_0 = 0, sy_0 = 0, sz_0 = 0;
  int32_t sx_1 = 0, sy_1 = 0, sz_1 = 0;
  int32_t sx_2 = 0, sy_2 = 0, sz_2 = 0;

  for (int n = 0; n < CAL_N; n++) {
    uint8_t bmi[12], b0[12], b1[12], b2[12];
    NDP.sensorBMI270Read(BMI_ACCEL_REG, 12, bmi);
    pca_select(ICM0_CH);
    icm_read_bytes(icm_addr[0], 0x2D, b0, 12);
    pca_select(ICM1_CH);
    icm_read_bytes(icm_addr[1], 0x2D, b1, 12);
    pca_select(ICM2_CH);
    icm_read_bytes(icm_addr[2], 0x2D, b2, 12);
    pca_deselect();

    sx_i += read_i16_le(&bmi[6]);
    sy_i += read_i16_le(&bmi[8]);
    sz_i += read_i16_le(&bmi[10]);
    sx_0 += read_i16_be(&b0[6]);
    sy_0 += read_i16_be(&b0[8]);
    sz_0 += read_i16_be(&b0[10]);
    sx_1 += read_i16_be(&b1[6]);
    sy_1 += read_i16_be(&b1[8]);
    sz_1 += read_i16_be(&b1[10]);
    sx_2 += read_i16_be(&b2[6]);
    sy_2 += read_i16_be(&b2[8]);
    sz_2 += read_i16_be(&b2[10]);
    delay(10);
  }

  gx_bias_i = sx_i / CAL_N;
  gy_bias_i = sy_i / CAL_N;
  gz_bias_i = sz_i / CAL_N;
  gx_bias_0 = sx_0 / CAL_N;
  gy_bias_0 = sy_0 / CAL_N;
  gz_bias_0 = sz_0 / CAL_N;
  gx_bias_1 = sx_1 / CAL_N;
  gy_bias_1 = sy_1 / CAL_N;
  gz_bias_1 = sz_1 / CAL_N;
  gx_bias_2 = sx_2 / CAL_N;
  gy_bias_2 = sy_2 / CAL_N;
  gz_bias_2 = sz_2 / CAL_N;

  if (Serial) {
    Serial.print("BIAS_BMI ");
    Serial.print(gx_bias_i);
    Serial.print(" ");
    Serial.print(gy_bias_i);
    Serial.print(" ");
    Serial.println(gz_bias_i);
    Serial.print("BIAS_ICM0 ");
    Serial.print(gx_bias_0);
    Serial.print(" ");
    Serial.print(gy_bias_0);
    Serial.print(" ");
    Serial.println(gz_bias_0);
    Serial.print("BIAS_ICM1 ");
    Serial.print(gx_bias_1);
    Serial.print(" ");
    Serial.print(gy_bias_1);
    Serial.print(" ");
    Serial.println(gz_bias_1);
    Serial.print("BIAS_ICM2 ");
    Serial.print(gx_bias_2);
    Serial.print(" ");
    Serial.print(gy_bias_2);
    Serial.print(" ");
    Serial.println(gz_bias_2);
  }

  // ── Abre a sessão no SD (após sensores → meta.txt já tem NVM do BMP390) ────
  sessionNumber = load_and_increment_session();
  start_session(sessionNumber);
  if (Serial) {
    Serial.print("SESSION_START ");
    Serial.print(sessionFolder);
    Serial.print(" file_ok=");
    Serial.println(imuFile ? "1" : "0");
  }

  nicla::leds.setColor(green);
  nextSampleUs = micros();
}

void loop() {
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0)
    return;
  nextSampleUs += IMU_SAMPLE_PERIOD_US;

  // ── Anti-rajada ──────────────────────────────────────────────────────────
  // Se a iteração anterior atrasou o loop além do próximo deadline (flush/
  // escrita do SD bloqueando), ressincroniza em vez de disparar amostras em
  // rajada para "recuperar". Sem isto: fs > 100Hz e jitter dispara, porque
  // cada spike do SD gera vários dt curtos seguidos.
  if ((int32_t)(now - nextSampleUs) >= 0) {
    nextSampleUs = now + IMU_SAMPLE_PERIOD_US;
  }

  uint8_t __attribute__((aligned(4))) bmi_data[12];
  NDP.sensorBMI270Read(BMI_ACCEL_REG, 12, bmi_data);

  if (++magDecCounter >= MAG_DECIMATE) {
    magDecCounter = 0;
    uint8_t __attribute__((aligned(4))) mag_data[8];
    NDP.sensorBMM150Read(BMM150_DATA_REG, 6, mag_data);
    mx_cache = read_i16_le(&mag_data[0]) >> 3;
    my_cache = read_i16_le(&mag_data[2]) >> 3;
    mz_cache = read_i16_le(&mag_data[4]) >> 1;
  }

  uint8_t icm_buf0[12], icm_buf1[12], icm_buf2[12];
  pca_select(ICM0_CH);
  icm_read_bytes(icm_addr[0], 0x2D, icm_buf0, 12);
  pca_select(ICM1_CH);
  icm_read_bytes(icm_addr[1], 0x2D, icm_buf1, 12);
  pca_select(ICM2_CH);
  icm_read_bytes(icm_addr[2], 0x2D, icm_buf2, 12);
  pca_deselect();

  bool ok0 = false, ok1 = false, ok2 = false;
  for (int i = 0; i < 12; i++) {
    if (icm_buf0[i])
      ok0 = true;
    if (icm_buf1[i])
      ok1 = true;
    if (icm_buf2[i])
      ok2 = true;
  }
  if (!ok0 || !ok1 || !ok2)
    return;

  // ── BMP390: lê 1 a cada BMP_DECIMATE ciclos (~25Hz), cache nos demais ──────
  if (++bmpDecCounter >= BMP_DECIMATE) {
    bmpDecCounter = 0;
    pca_select(BMP0_CH);
    bmp_read(bmp_addr[0], &press0_cache, &temp0_cache);
    pca_select(BMP1_CH);
    bmp_read(bmp_addr[1], &press1_cache, &temp1_cache);
    pca_deselect();
  }

  ImuPacket pkt;
  pkt.magic = PACKET_MAGIC;
  pkt.timestamp_us = micros();

  pkt.ax_i = read_i16_le(&bmi_data[0]);
  pkt.ay_i = read_i16_le(&bmi_data[2]);
  pkt.az_i = read_i16_le(&bmi_data[4]);
  pkt.gx_i = sub_sat_i16(read_i16_le(&bmi_data[6]), gx_bias_i);
  pkt.gy_i = sub_sat_i16(read_i16_le(&bmi_data[8]), gy_bias_i);
  pkt.gz_i = sub_sat_i16(read_i16_le(&bmi_data[10]), gz_bias_i);

  pkt.mx_i = mx_cache;
  pkt.my_i = my_cache;
  pkt.mz_i = mz_cache;

  pkt.ax_0 = read_i16_be(&icm_buf0[0]);
  pkt.ay_0 = read_i16_be(&icm_buf0[2]);
  pkt.az_0 = read_i16_be(&icm_buf0[4]);
  pkt.gx_0 = sub_sat_i16(read_i16_be(&icm_buf0[6]), gx_bias_0);
  pkt.gy_0 = sub_sat_i16(read_i16_be(&icm_buf0[8]), gy_bias_0);
  pkt.gz_0 = sub_sat_i16(read_i16_be(&icm_buf0[10]), gz_bias_0);

  pkt.ax_1 = read_i16_be(&icm_buf1[0]);
  pkt.ay_1 = read_i16_be(&icm_buf1[2]);
  pkt.az_1 = read_i16_be(&icm_buf1[4]);
  pkt.gx_1 = sub_sat_i16(read_i16_be(&icm_buf1[6]), gx_bias_1);
  pkt.gy_1 = sub_sat_i16(read_i16_be(&icm_buf1[8]), gy_bias_1);
  pkt.gz_1 = sub_sat_i16(read_i16_be(&icm_buf1[10]), gz_bias_1);

  pkt.ax_2 = read_i16_be(&icm_buf2[0]);
  pkt.ay_2 = read_i16_be(&icm_buf2[2]);
  pkt.az_2 = read_i16_be(&icm_buf2[4]);
  pkt.gx_2 = sub_sat_i16(read_i16_be(&icm_buf2[6]), gx_bias_2);
  pkt.gy_2 = sub_sat_i16(read_i16_be(&icm_buf2[8]), gy_bias_2);
  pkt.gz_2 = sub_sat_i16(read_i16_be(&icm_buf2[10]), gz_bias_2);

  pkt.press_0 = press0_cache;
  pkt.temp_0 = temp0_cache;
  pkt.press_1 = press1_cache;
  pkt.temp_1 = temp1_cache;

  pkt.seq = seqCounter++;
  pkt.crc8 = crc8_calc(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt) - 1);

  // ── Grava no SD (arquivo permanece aberto) ───────────────────────────────
  if (sdReady && imuFile) {
    sdAttemptedWrite = true;
    size_t w = imuFile.write(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
    if (w == sizeof(pkt))
      sdWroteSinceCheck = true;
  }

  // ── Stream serial — não-bloqueante ───────────────────────────────────────
  // Só escreve se há espaço no buffer TX. Sem isto, um host conectado mas que
  // não está lendo (buffer cheio) bloqueia o loop e arruína o timing do SD.
  if (Serial && Serial.availableForWrite() >= (int)sizeof(pkt)) {
    Serial.write(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
  }

  packetsInSession++;
  if (++packetsSinceFlush >= PACKETS_PER_FLUSH) {
    packetsSinceFlush = 0;
    if (sdReady && imuFile)
      imuFile.flush();
  }

  // ── Detecção de remoção do SD a cada ~2s ─────────────────────────────────
  // Dispara SÓ se houve tentativas de escrita na janela inteira e NENHUMA teve
  // sucesso → cartão removido. Robusto a falhas transitórias durante o erase
  // do cartão (basta 1 escrita bem-sucedida na janela para não disparar).
  // Não dispara se nenhum pacote foi gravado (ex: ICM instável) — isso não é
  // problema de SD.
  if (++packetsSinceSdCheck >= PACKETS_PER_SD_CHECK) {
    packetsSinceSdCheck = 0;
    if (sdReady && sdAttemptedWrite && !sdWroteSinceCheck) {
      handle_sd_lost();
    }
    sdWroteSinceCheck = false;
    sdAttemptedWrite = false;
  }

  // ── Rotação de sessão a cada 12h ─────────────────────────────────────────
  if (sdReady && packetsInSession >= PACKETS_PER_ROTATION) {
    rotate_session();
  }
}

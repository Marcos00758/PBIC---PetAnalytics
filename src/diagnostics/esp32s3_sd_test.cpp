#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
// FS.h owns the generic File alias; this diagnostic always uses FsFile/O_*.
#define DISABLE_FS_H_WARNING
#include <SdFat.h>
#include <Preferences.h>
#include <esp_timer.h>
#include "config/pins.h"
#include "config/storage_config.h"
#include "sd_test_format.h"
#include "utils/esp32_reset_reason.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "The independent SD diagnostic requires ESP32-S3"
#endif

namespace {
namespace fmt = pet::sdtest;
constexpr uint8_t kClocks[] = {4, pet::storage::kSpiClockMHz, 20};
constexpr size_t kBlocks[] = {pet::storage::kWriteBlockBytes, 1024, 4096};
constexpr uint64_t kMaximumBytes = 8ULL * 1024 * 1024;
constexpr uint64_t kPacedBytes = ((7900ULL * 30 + 255) / 256) * 256;
constexpr uint32_t kSyncBytes = 64 * 1024;
constexpr uint32_t kReferenceSpiHz = 4000000;
constexpr uint8_t kSpiOptions = SHARED_SPI | USER_SPI_BEGIN;
constexpr uint32_t kLatencyBounds[] = {1000, 2000, 5000, 10000, 20000,
                                     50000, 100000};
SPIClass sdFspi(FSPI);
SPIClass sdHspi(HSPI);
uint8_t selectedBus = FSPI;
SPIClass& activeSpi() { return selectedBus == HSPI ? sdHspi : sdFspi; }
const char* spiBusName() { return selectedBus == HSPI ? "HSPI" : "FSPI"; }
SdFs sd;
Preferences prefs;
FsFile dirtyData;  // Remains open during intentional reset/power interruption.
FsFile dirtyCheckpoint;
char runPath[48] = {};
char cardId[33] = {};
uint8_t buffer[pet::storage::kBufferBytes];
uint8_t expected[4096];
uint8_t currentMHz = 0;
unsigned activeCase = 999;  // 999 identifies setup/run metadata operations.
size_t activeBlock = 0;
bool armed = false;
bool available = false;
bool suiteOk = true;
const char* bootPath = "SdFat_direct";

uint64_t nowUs() { return esp_timer_get_time(); }
struct Stats {
  uint64_t count = 0, total = 0, maximum = 0, minimum = UINT64_MAX;
  uint64_t buckets[8] = {};
  void add(uint64_t us) {
    ++count; total += us;
    if (us > maximum) maximum = us;
    if (us < minimum) minimum = us;
    unsigned bucket = 0;
    while (bucket < 7 && us >= kLatencyBounds[bucket]) ++bucket;
    ++buckets[bucket];
  }
};
void path(char* out, unsigned id, const char* extension) {
  snprintf(out, 80, "%s/c%03u.%s", runPath, id, extension);
}
bool failure(const char* where) {
  suiteOk = false;
  const uint8_t code = sd.sdErrorCode(), detail = sd.sdErrorData();
  Serial.printf("SD_ERROR case=%u spi_bus=%s spi_mhz=%u block=%u operation=%s code=%u data=%u\n",
                activeCase, spiBusName(), currentMHz, unsigned(activeBlock), where, code, detail);
  // A failed transport may also prevent saving this error on the card.
  char name[80]; snprintf(name, sizeof(name), "%s/errors.txt", runPath);
  FsFile f;
  if (runPath[0] && f.open(name, O_WRONLY | O_CREAT | O_APPEND)) {
    f.printf("case=%u spi_bus=%s spi_mhz=%u block=%u operation=%s code=%u data=%u\n",
             activeCase, spiBusName(), currentMHz, unsigned(activeBlock), where, code, detail);
    f.close();
  }
  return false;
}
void beginSpi(SPIClass& spi) {
  spi.end();
  pinMode(pet::pins::kSdChipSelect, OUTPUT);
  digitalWrite(pet::pins::kSdChipSelect, HIGH);
  spi.begin(pet::pins::kSdSck, pet::pins::kSdMiso, pet::pins::kSdMosi,
            pet::pins::kSdChipSelect);
}
bool mount(uint8_t mhz) {
  sd.end();
  beginSpi(activeSpi());
  currentMHz = mhz;
  // Like SDCard::init(SPIClass&), consume the bus already configured by us.
  // USER_SPI_BEGIN prevents SdFat from calling begin() again without pins.
  return sd.begin(SdSpiConfig(pet::pins::kSdChipSelect, kSpiOptions,
                             uint32_t(mhz) * 1000000, &activeSpi()));
}
void probeReference() {
  sd.end();
  beginSpi(activeSpi());
  // Reproduce SDCard.cpp's SD.begin(cs, spi), with its 4 MHz default explicit.
  // Formatting is disabled; no test files are opened or written through SD.h.
  const bool mounted = SD.begin(pet::pins::kSdChipSelect, activeSpi(),
                               kReferenceSpiHz, "/sd-reference", 5, false);
  Serial.printf("SD_REFERENCE library=Arduino_SD mounted=%u spi_bus=%s spi_hz=%lu "
                "sck=%u miso=%u mosi=%u cs=%u card_type=%u card_bytes=%llu\n",
                mounted, spiBusName(), static_cast<unsigned long>(kReferenceSpiHz),
                pet::pins::kSdSck, pet::pins::kSdMiso, pet::pins::kSdMosi,
                pet::pins::kSdChipSelect, mounted ? unsigned(SD.cardType()) : 0,
                mounted ? SD.cardSize() : 0ULL);
  SD.end();  // The two filesystem drivers must never own the card together.
  activeSpi().end();
  digitalWrite(pet::pins::kSdChipSelect, HIGH);
}
bool mountAtBoot() {
  // Prefer the controller recorded for the last run, including recovery boots.
  const uint8_t preferred = prefs.getUChar("bus", FSPI) == HSPI ? HSPI : FSPI;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    sd.end();
    activeSpi().end();
    selectedBus = attempt == 0 ? preferred : (preferred == FSPI ? HSPI : FSPI);
    Serial.printf("SD_BUS_SELECT spi_bus=%s attempt=%u sck=%u miso=%u mosi=%u cs=%u\n",
                  spiBusName(), attempt + 1, pet::pins::kSdSck, pet::pins::kSdMiso,
                  pet::pins::kSdMosi, pet::pins::kSdChipSelect);
    if (mount(pet::storage::kSpiClockMHz)) return true;
    failure("boot_mount");
    probeReference();
    // SD.h may have initialized SPI even if its filesystem mount failed.
    Serial.printf("SD_INIT_RETRY library=SdFat spi_bus=%s spi_mode=SHARED_SPI user_spi_begin=1\n",
                  spiBusName());
    if (mount(pet::storage::kSpiClockMHz)) {
      bootPath = "SdFat_after_Arduino_SD";
      return true;
    }
    failure("boot_mount_after_reference");
  }
  sd.end();
  activeSpi().end();
  digitalWrite(pet::pins::kSdChipSelect, HIGH);
  Serial.println("SD_INIT_FAILED tested_controllers=FSPI,HSPI acquisition_started=0");
  return false;
}
bool identity() {
  cid_t cid; csd_t csd;
  if (!sd.card()->readCID(&cid) || !sd.card()->readCSD(&csd))
    return failure("read_card_identity");
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&cid);
  for (unsigned i = 0; i < 16; ++i) sprintf(cardId + 2 * i, "%02x", raw[i]);
  Serial.printf("SD_CARD cid=%s mid=%u product=%.5s serial=%lu sectors=%lu "
                "fat=%u spi_mhz=%u sdfat=%s spi_bus=%s\n", cardId, cid.mid, cid.pnm,
                static_cast<unsigned long>(cid.psn()),
                static_cast<unsigned long>(sd.card()->sectorCount()),
                sd.fatType(), currentMHz, SD_FAT_VERSION_STR, spiBusName());
  return true;
}
bool report(unsigned id, const char* mode, size_t block, const char* operation,
            const Stats& stats, uint64_t bytes, uint64_t elapsed,
            uint64_t lateCount, uint64_t lateMax, bool ok) {
  char name[80]; snprintf(name, sizeof(name), "%s/results.csv", runPath);
  FsFile f;
  if (!f.open(name, O_WRONLY | O_CREAT | O_APPEND)) return failure("results_open");
  f.printf("%u,%s,%u,%u,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u",
           id, mode, currentMHz, unsigned(block), operation, bytes, elapsed,
           stats.count, stats.count ? stats.minimum : 0,
           stats.count ? stats.total / stats.count : 0, stats.maximum,
           lateCount, lateMax, ok ? 1 : 0);
  for (auto bucket : stats.buckets) f.printf(",%llu", bucket);
  f.println();
  const bool good = !f.getWriteError() && f.close();
  Serial.printf("SD_RESULT case=%u mode=%s op=%s spi_mhz=%u block=%u "
                "bytes=%llu elapsed_us=%llu max_us=%llu late_slots=%llu "
                "max_lateness_us=%llu ok=%u\n", id, mode, operation,
                currentMHz, unsigned(block), bytes, elapsed, stats.maximum,
                lateCount, lateMax, ok ? 1 : 0);
  return good || failure("results_write");
}
bool appendCheckpoint(FsFile& f, uint32_t seed, uint64_t durable,
                      uint64_t planned, bool complete) {
  uint8_t data[fmt::kCheckpointBytes];
  fmt::checkpoint(data, seed, durable, planned, complete);
  return f.write(data, sizeof(data)) == sizeof(data) && f.sync();
}
bool prepare(unsigned id, uint64_t planned, uint64_t allocation, uint32_t seed,
             FsFile& data, FsFile& checkpoint) {
  char name[80];
  path(name, id, "bin");
  if (sd.exists(name)) return failure("case_already_exists");
  path(name, id, "chk");
  if (sd.exists(name)) return failure("case_already_exists");
  const int32_t clusters = sd.freeClusterCount();
  const uint64_t needed = (planned > allocation ? planned : allocation) +
                         pet::storage::kFreeReserveBytes + 64 * 1024;
  if (clusters < 0 || uint64_t(clusters) * sd.bytesPerCluster() < needed)
    return failure("insufficient_free_space");
  snprintf(name, sizeof(name), "%s/manifest.txt", runPath);
  FsFile manifest;
  if (!manifest.open(name, O_WRONLY | O_CREAT | O_APPEND))
    return failure("manifest_open");
  manifest.printf("%03u\n", id);
  if (manifest.getWriteError() || !manifest.close()) return failure("manifest_write");
  path(name, id, "bin");
  if (!data.open(name, O_RDWR | O_CREAT | O_EXCL)) return failure("data_open");
  path(name, id, "chk");
  if (!checkpoint.open(name, O_WRONLY | O_CREAT | O_EXCL))
    return failure("checkpoint_open");
  if (!appendCheckpoint(checkpoint, seed, 0, planned, false))
    return failure("initial_checkpoint");
  const uint64_t start = nowUs();
  const bool good = data.preAllocate(allocation) && data.seekSet(0) && data.sync();
  Stats stats; stats.add(nowUs() - start);
  if (!report(id, "allocation", 0, "preallocate", stats, allocation,
              stats.total, 0, 0, good)) return false;
  return good || failure("preallocate");
}
bool verify(unsigned id) {
  activeCase = id; activeBlock = sizeof(expected);
  char name[80]; path(name, id, "chk");
  FsFile checkpoint;
  if (!checkpoint.open(name, O_RDONLY)) return failure("verify_checkpoint_open");
  uint8_t item[32], last[32];
  bool found = false, tail = false;
  while (checkpoint.available()) {
    const int count = checkpoint.read(item, sizeof(item));
    if (count < 0) { checkpoint.close(); return failure("checkpoint_read"); }
    if (count != 32 || !fmt::validCheckpoint(item)) { tail = true; break; }
    if (found && (fmt::get32(item + 4) != fmt::get32(last + 4) ||
                  fmt::get64(item + 16) != fmt::get64(last + 16) ||
                  fmt::get64(item + 8) < fmt::get64(last + 8) ||
                  fmt::get32(last + 24) == 1)) {
      checkpoint.close(); return failure("checkpoint_order");
    }
    memcpy(last, item, sizeof(last)); found = true;
  }
  if (!checkpoint.close() || !found) return failure("no_valid_checkpoint");
  const uint64_t durable = fmt::get64(last + 8);
  const bool complete = fmt::get32(last + 24) == 1;
  if (fmt::get32(last + 4) != (0x50424943U ^ id)) return failure("checkpoint_case_identity");
  path(name, id, "bin");
  FsFile data;
  if (!data.open(name, O_RDONLY)) return failure("verify_data_open");
  const uint64_t actual = data.fileSize();
  bool good = actual >= durable && (!complete || (actual == durable && !tail));
  if ((id == 18 || id == 20) && durable < 64 * 1024) good = false;
  Stats reads; uint64_t offset = 0; const uint64_t start = nowUs();
  while (good && offset < durable) {
    size_t n = size_t(durable - offset < sizeof(expected) ? durable - offset : sizeof(expected));
    const uint64_t readStart = nowUs();
    const int got = data.read(buffer, n); reads.add(nowUs() - readStart);
    if (got != int(n)) { good = false; break; }
    fmt::fill(expected, n, offset, fmt::get32(last + 4));
    if (memcmp(buffer, expected, n) != 0) { good = false; break; }
    offset += n; yield();
  }
  const uint64_t elapsed = nowUs() - start;
  good = data.close() && good;
  Serial.printf("SD_VERIFY case=%u durable=%llu actual=%llu complete=%u "
                "checkpoint_tail=%u verified=%llu ok=%u\n", id, durable,
                actual, complete, tail, offset, good);
  if (!report(id, complete ? "verify_complete" : "verify_prefix", sizeof(expected),
              "read", reads, offset, elapsed, 0, 0, good)) return false;
  return good || failure("integrity_or_size");
}
bool verifyRun() {
  char name[80]; snprintf(name, sizeof(name), "%s/manifest.txt", runPath);
  FsFile manifest;
  if (!manifest.open(name, O_RDONLY)) return failure("manifest_missing");
  bool good = true; unsigned count = 0;
  while (manifest.available()) {
    char line[4];
    if (manifest.read(line, 4) != 4 || line[3] != '\n' ||
        line[0] < '0' || line[0] > '9' || line[1] < '0' || line[1] > '9' ||
        line[2] < '0' || line[2] > '9') {
      good = failure("manifest_corrupt"); break;
    }
    const unsigned id = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
    if (id != count++) { good = failure("manifest_order"); break; }
    if (!verify(id)) good = false;
  }
  good = manifest.close() && good && count > 0;
  Serial.printf("SD_RUN_VERIFY path=%s cases=%u ok=%u\n", runPath, count, good);
  return good;
}
bool runCase(unsigned id, uint8_t mhz, size_t block, bool paced) {
  activeCase = id; activeBlock = block;
  if (!mount(mhz)) return failure("mount_matrix");
  const uint64_t planned = paced ? kPacedBytes : kMaximumBytes;
  const uint32_t seed = 0x50424943U ^ id;
  FsFile data, checkpoint;
  if (!prepare(id, planned, paced ? 64 * 1024 : 2 * 1024 * 1024,
               seed, data, checkpoint)) return false;
  Stats writes, syncs, checkpointSyncs;
  uint64_t written = 0, generated = 0, durable = 0, lateCount = 0, lateMax = 0;
  size_t buffered = 0;
  const uint64_t start = nowUs();
  uint64_t nextSlot = start + 10000, lastSync = start;
  bool good = true;
  while (written < planned && good) {
    if (paced && generated < planned && nowUs() >= nextSlot) {
      const uint64_t lateness = nowUs() - nextSlot;
      if (lateness > lateMax) lateMax = lateness;
      const uint64_t skipped = lateness / 10000;
      lateCount += skipped; nextSlot += (skipped + 1) * 10000;
      const size_t n = size_t(planned - generated < 79 ? planned - generated : 79);
      if (buffered + n > sizeof(buffer)) { good = failure("paced_buffer_overflow"); break; }
      fmt::fill(buffer + buffered, n, generated, seed);
      buffered += n; generated += n;
    }
    if (!paced) {
      buffered = size_t(planned - written < block ? planned - written : block);
      fmt::fill(buffer, buffered, written, seed);
    }
    if (buffered >= block || (generated == planned && buffered)) {
      const size_t n = buffered < block ? buffered : block;
      const uint64_t t = nowUs();
      good = data.write(buffer, n) == n; writes.add(nowUs() - t);
      if (!good) { failure("data_write"); break; }
      written += n; buffered -= n;
      if (buffered) memmove(buffer, buffer + n, buffered);
    }
    if (written > durable && (written - durable >= kSyncBytes || nowUs() - lastSync >= 1000000)) {
      const uint64_t t = nowUs(); good = data.sync(); syncs.add(nowUs() - t);
      const uint64_t confirmed = written / 256 * 256;
      if (good) {
        const uint64_t c = nowUs();
        good = appendCheckpoint(checkpoint, seed, confirmed, planned, false);
        checkpointSyncs.add(nowUs() - c);
      }
      if (!good) { failure("data_or_checkpoint_sync"); break; }
      durable = confirmed; lastSync = nowUs();
    }
    if (paced && buffered < block && generated < planned) delay(1);
    else yield();
  }
  const uint64_t elapsed = nowUs() - start;
  if (good) {
    const uint64_t t = nowUs();
    good = data.truncate(written) && data.sync(); syncs.add(nowUs() - t);
    if (good) {
      const uint64_t c = nowUs();
      good = appendCheckpoint(checkpoint, seed, written, planned, true);
      checkpointSyncs.add(nowUs() - c);
    }
  }
  const bool dataClosed = data.close(), checkpointClosed = checkpoint.close();
  good = good && dataClosed && checkpointClosed;
  if (!good) failure("case_finalize");
  const char* mode = paced ? "7900Bps" : "maximum";
  if (!report(id, mode, block, "write", writes, written, elapsed, lateCount, lateMax, good)) good = false;
  if (!report(id, mode, block, "sync", syncs, 0, syncs.total, 0, 0, good)) good = false;
  if (!report(id, mode, block, "checkpoint_sync", checkpointSyncs, 0,
              checkpointSyncs.total, 0, 0, good)) good = false;
  const bool verified = verify(id);
  return good && verified;
}
bool newRun() {
  activeCase = 999; activeBlock = 0;
  if (!mount(pet::storage::kSpiClockMHz) || !identity()) return failure("new_run_mount");
  if (!sd.exists("/SDTEST") && !sd.mkdir("/SDTEST")) return failure("root_directory");
  bool found = false;
  for (unsigned i = 1; i <= 999999; ++i) {
    snprintf(runPath, sizeof(runPath), "/SDTEST/R%06u", i);
    if (!sd.exists(runPath)) { found = true; break; }
  }
  if (!found || !sd.mkdir(runPath)) return failure("run_directory");
  char name[80]; snprintf(name, sizeof(name), "%s/meta.txt", runPath);
  FsFile f;
  if (!f.open(name, O_WRONLY | O_CREAT | O_EXCL)) return failure("metadata_open");
  f.printf("format=SDT1\ntest_version=3\ncid=%s\nsdfat=%s\nsectors=%lu\nfat=%u\n"
           "cpu_mhz=%u\nflash_bytes=%lu\npsram_bytes=%lu\n"
           "sck=%u\nmiso=%u\nmosi=%u\ncs=%u\nvcc=3.3V\nspi_mode=SHARED_SPI\n"
           "spi_bus=%s\nspi_begin=USER_SPI_BEGIN\nboot_path=%s\nreference_spi_hz=4000000\n"
           "clocks_mhz=4,12,20\nblocks_bytes=512,1024,4096\n"
           "paced_target_Bps=7900\npaced_packet_bytes=79\npaced_period_us=10000\n"
           "maximum_bytes=%llu\npaced_bytes=%llu\n",
           cardId, SD_FAT_VERSION_STR, static_cast<unsigned long>(sd.card()->sectorCount()),
           sd.fatType(), ESP.getCpuFreqMHz(), static_cast<unsigned long>(ESP.getFlashChipSize()),
           static_cast<unsigned long>(ESP.getPsramSize()),
           unsigned(pet::pins::kSdSck), unsigned(pet::pins::kSdMiso),
           unsigned(pet::pins::kSdMosi), unsigned(pet::pins::kSdChipSelect),
           spiBusName(), bootPath, kMaximumBytes, kPacedBytes);
  if (f.getWriteError() || !f.close()) return failure("metadata_write");
  snprintf(name, sizeof(name), "%s/results.csv", runPath);
  if (!f.open(name, O_WRONLY | O_CREAT | O_EXCL)) return failure("results_create");
  f.println("case,mode,spi_mhz,block_bytes,operation,bytes,elapsed_us,calls,min_us,mean_us,max_us,late_slots,max_lateness_us,ok,lt1ms,1to2ms,2to5ms,5to10ms,10to20ms,20to50ms,50to100ms,ge100ms");
  if (f.getWriteError() || !f.close()) return failure("results_header");
  if (!prefs.putString("path", runPath) || !prefs.putString("cid", cardId) ||
      !prefs.putUChar("bus", selectedBus) || !prefs.putUChar("phase", 0)) return failure("nvs_new_run");
  return true;
}
bool setPhase(uint8_t phase) {
  const bool good = prefs.putUChar("phase", phase) == 1 && prefs.putBool("ok", suiteOk) == 1;
  return good || failure("nvs_phase_write");
}
bool prepareInterruption(unsigned id, uint8_t phase) {
  activeCase = id; activeBlock = 4096;
  // NVS intent is stored before creating evidence. Failure cannot silently skip recovery.
  if (!setPhase(phase)) return failure("nvs_interruption");
  if (!prepare(id, 256 * 1024, 512 * 1024, 0x50424943U ^ id,
               dirtyData, dirtyCheckpoint)) return false;
  const uint32_t seed = 0x50424943U ^ id;
  for (uint64_t offset = 0; offset < 64 * 1024; offset += 4096) {
    fmt::fill(buffer, 4096, offset, seed);
    if (dirtyData.write(buffer, 4096) != 4096) return failure("interruption_prefix_write");
  }
  if (!dirtyData.sync() || !appendCheckpoint(dirtyCheckpoint, seed, 64 * 1024, 256 * 1024, false))
    return failure("interruption_checkpoint");
  for (uint64_t offset = 64 * 1024; offset < 81 * 1024 + 17;) {
    const size_t n = size_t(81 * 1024 + 17 - offset < 4096 ? 81 * 1024 + 17 - offset : 4096);
    fmt::fill(buffer, n, offset, seed);
    if (dirtyData.write(buffer, n) != n) return failure("interruption_tail_write");
    offset += n;
  }
  armed = true;
  Serial.printf("%s path=%s case=%u durable_bytes=65536\n",
                phase == 3 ? "SD_POWER_CUT_READY" : "SD_SOFTWARE_RESET_READY", runPath, id);
  Serial.flush();
  if (phase != 3) { delay(100); ESP.restart(); }
  return true;
}
bool interruption(unsigned id, uint8_t phase) {
  if (prepareInterruption(id, phase)) return true;
  // Global files must never survive an aborted preparation and a later remount.
  if (dirtyData.isOpen() && !dirtyData.close()) failure("interruption_data_close");
  if (dirtyCheckpoint.isOpen() && !dirtyCheckpoint.close()) failure("interruption_checkpoint_close");
  if (!setPhase(0)) failure("interruption_clear_intent");
  return false;
}
bool canPowerCut() {
  char name[80]; path(name, 20, "bin");
  if (sd.exists(name)) return false;
  path(name, 20, "chk");
  if (sd.exists(name)) return false;
  snprintf(name, sizeof(name), "%s/manifest.txt", runPath);
  FsFile f;
  if (!f.open(name, O_RDONLY)) return false;
  const bool countOk = f.fileSize() == 20 * 4;
  if (!f.close() || !countOk || prefs.getUChar("phase", 0) != 0) return false;
  path(name, 19, "chk");
  uint8_t last[32];
  if (!f.open(name, O_RDONLY)) return false;
  const uint64_t length = f.fileSize();
  const bool good = length >= 32 && length % 32 == 0 && f.seekSet(length - 32) &&
                    f.read(last, 32) == 32 && fmt::validCheckpoint(last) &&
                    fmt::get32(last + 24) == 1;
  const bool closed = f.close();
  return good && closed;
}
void suite() {
  suiteOk = true;
  if (!newRun()) return;
  Serial.printf("SD_SUITE_START path=%s\n", runPath);
  unsigned id = 0;
  for (auto clock : kClocks) for (auto block : kBlocks) {
    if (!runCase(id++, clock, block, false) || !runCase(id++, clock, block, true)) {
      Serial.println("SD_SUITE_ABORTED: inspect errors.txt and serial output"); return;
    }
  }
  if (!mount(pet::storage::kSpiClockMHz)) { failure("reset_mount"); return; }
  interruption(18, 1);
}
void help() {
  Serial.println("SD_TEST v3: s=new suite; v=verify latest run; p=manual power-cut case (after suite)");
  Serial.println("No audio/sensors. New files only in /SDTEST. Software resets reconnect USB.");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && millis() - start < 3000) delay(10);
  Serial.printf("SD_BOOT reset_reason=%d reset=%s\n", int(esp_reset_reason()),
                pet::utils::resetReasonName(esp_reset_reason()));
  if (!prefs.begin("pbic-sdtest", false)) { Serial.println("SD_ERROR NVS unavailable"); return; }
  if (!mountAtBoot()) return;
  if (!identity()) return;
  available = true;
  const String previous = prefs.getString("path", "");
  if (previous.length()) {
    if (previous.length() >= sizeof(runPath) || !previous.startsWith("/SDTEST/R")) {
      failure("nvs_path_invalid"); available = false; return;
    }
    previous.toCharArray(runPath, sizeof(runPath));
    if (prefs.getString("cid", "") != cardId) {
      Serial.println("SD_CARD_CHANGED: previous evidence is on another card; use s for a new run");
      runPath[0] = 0; help(); return;
    }
    if (prefs.getUChar("bus", FSPI) != selectedBus) {
      Serial.printf("SD_RECOVERY_BUS_CHANGED spi_bus=%s saved_bus=%u "
                    "automatic_recovery=0; use s for a new run or verify old files in Python\n",
                    spiBusName(), prefs.getUChar("bus", FSPI));
      // Keep old reset intent/evidence, without mixing controllers in its CSV.
      runPath[0] = 0; help(); return;
    }
    suiteOk = prefs.getBool("ok", true);
    const uint8_t phase = prefs.getUChar("phase", 0);
    const bool verified = verifyRun();
    // Clear automatic reset intent before any next step: failures do not reboot-loop.
    if (!prefs.putUChar("phase", 0)) { failure("nvs_clear_phase"); available = false; return; }
    if (verified && phase == 1) {
      if (runCase(19, pet::storage::kSpiClockMHz, 512, false) && setPhase(2)) {
        Serial.println("SD_CLOSED_RESET_READY"); Serial.flush(); delay(100); ESP.restart();
      }
    } else if (phase) {
      Serial.printf("SD_RECOVERY phase=%u integrity_ok=%u suite_ok=%u\n",
                    phase, verified, suiteOk && verified);
    }
  }
  help();
}
void loop() {
  if (available && !armed && Serial.available()) {
    const char command = Serial.read();
    if (command == 's') suite();
    else if (command == 'v' && runPath[0]) verifyRun();
    else if (command == 'p' && runPath[0]) {
      if (canPowerCut()) interruption(20, 3);
      else Serial.println("SD_POWER_CUT_REQUIRES_COMPLETED_SUITE_AND_UNUSED_CASE20");
    } else if (command != '\r' && command != '\n') help();
  }
  delay(1);
}

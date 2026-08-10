#include <Arduino.h>
#include <Wire.h>

#include "config/constants.h"
#include "config/pins.h"
#include "data/imu_packet.h"
#include "drivers/bmp390.h"
#include "drivers/icm20948.h"
#include "drivers/pca9548a.h"
#include "services/imu_acquisition.h"
#include "services/sd_logger.h"

namespace {

pet::drivers::Pca9548a mux(Wire, pet::config::kPca9548aAddress,
                           pet::config::kPcaChannelSettleUs);
pet::drivers::Icm20948 icm0(mux, Wire, pet::config::kIcm0Channel, 100);
pet::drivers::Icm20948 icm1(mux, Wire, pet::config::kIcm1Channel, 110);
pet::drivers::Icm20948 icm2(mux, Wire, pet::config::kIcm2Channel, 120);
pet::drivers::Icm20948* const icms[] = {&icm0, &icm1, &icm2};
pet::drivers::Bmp390 bmp0(mux, Wire, pet::config::kBmp0Channel);
pet::drivers::Bmp390 bmp1(mux, Wire, pet::config::kBmp1Channel);
pet::drivers::Bmp390* const bmps[] = {&bmp0, &bmp1};
pet::services::ImuAcquisition acquisition(
    icm0, icm1, icm2, bmp0, bmp1, pet::config::kImuSamplePeriodUs,
    pet::config::kBmpSamplesPerImuSample,
    pet::config::kMagSamplesPerImuSample);
pet::services::SdLogger sdLogger;

bool acquisitionReady = false;

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

bool initializeSensor(pet::drivers::Icm20948& icm) {
  Serial.print("ICM channel ");
  Serial.print(icm.muxChannel());

  if (!icm.begin()) {
    Serial.println(" FAILED (tested 0x69 and 0x68; expected WHO_AM_I=0xEA)");
    return false;
  }

  Serial.print(" OK address=0x");
  printHexByte(icm.address());
  Serial.print(" WHO_AM_I=0x");
  printHexByte(icm.whoAmI());
  Serial.print(" AK09916_WIA2=0x");
  printHexByte(icm.magnetometerWhoAmI());
  Serial.print(" accel=+/-");
  Serial.print(pet::config::kIcmAccelRangeG);
  Serial.print("g gyro=+/-");
  Serial.print(pet::config::kIcmGyroRangeDps);
  Serial.println("dps mag=20Hz");
  return true;
}

void diagnoseMagnetometer(pet::drivers::Icm20948& icm) {
  pet::drivers::Ak09916RawSample sample{};
  Serial.print("AK09916 channel ");
  Serial.print(icm.muxChannel());
  Serial.print(" WIA2=0x");
  printHexByte(icm.magnetometerWhoAmI());

  if (!icm.readMagnetometerRaw(sample)) {
    Serial.println(" raw_read=FAILED");
    return;
  }

  Serial.print(" ST1=0x");
  printHexByte(sample.status1);
  Serial.print(" ST2=0x");
  printHexByte(sample.status2);
  Serial.print(" raw=");
  Serial.print(sample.magnetic.x);
  Serial.print(',');
  Serial.print(sample.magnetic.y);
  Serial.print(',');
  Serial.print(sample.magnetic.z);
  Serial.print(" data_ready=");
  Serial.print(sample.dataReady ? "yes" : "no");
  Serial.print(" overrun=");
  Serial.print(sample.dataOverrun ? "yes" : "no");
  Serial.print(" overflow=");
  Serial.println(sample.overflow ? "yes" : "no");
}

bool initializeBmp(pet::drivers::Bmp390& bmp) {
  Serial.print("BMP390 channel ");
  Serial.print(bmp.muxChannel());

  if (!bmp.begin()) {
    Serial.println(" FAILED (tested 0x77 and 0x76; expected CHIP_ID=0x60)");
    return false;
  }

  Serial.print(" OK address=0x");
  printHexByte(bmp.address());
  Serial.print(" CHIP_ID=0x");
  printHexByte(bmp.chipId());
  Serial.println();
  return true;
}

}  // namespace

void setup() {
  Serial.begin(pet::config::kSerialBaud);

  Serial.println();
  Serial.println("PBIC / Pet Analytics - synchronized ICM acquisition");
  Serial.print("SD pins CS=");
  Serial.print(pet::pins::kSdChipSelect);
  Serial.print(" MOSI=");
  Serial.print(pet::pins::kSdMosi);
  Serial.print(" MISO=");
  Serial.print(pet::pins::kSdMiso);
  Serial.print(" SCK=");
  Serial.println(pet::pins::kSdSck);
  const bool sdCardReady = sdLogger.beginCard();
  Serial.println(sdCardReady ? "SD read/write diagnostic OK"
                             : "SD unavailable; USB stream remains enabled");

  Serial.print("Wire pins SDA=");
  Serial.print(pet::pins::kI2cSda);
  Serial.print(" SCL=");
  Serial.print(pet::pins::kI2cScl);
  Serial.print(" clock_hz=");
  Serial.println(pet::config::kI2cClockHz);

  Wire.begin();
  Wire.setClock(pet::config::kI2cClockHz);

  if (!mux.begin()) {
    Serial.println("PCA9548A FAILED at address 0x70");
    return;
  }
  Serial.println("PCA9548A OK at address 0x70");

  pet::services::SdSessionMetadata sessionMetadata{};
  bool sensorsReady = true;
  for (size_t i = 0; i < pet::data::kIcmCount; ++i) {
    sessionMetadata.icmReady[i] = initializeSensor(*icms[i]);
    sensorsReady = sessionMetadata.icmReady[i] && sensorsReady;
  }
  if (sensorsReady) {
    delay(60);
    for (pet::drivers::Icm20948* icm : icms) {
      diagnoseMagnetometer(*icm);
    }
  }
  bool bmpsReady = true;
  for (size_t i = 0; i < pet::data::kBmpCount; ++i) {
    sessionMetadata.bmpReady[i] = initializeBmp(*bmps[i]);
    sessionMetadata.bmpAddress[i] = bmps[i]->address();
    bmpsReady = sessionMetadata.bmpReady[i] && bmpsReady;
  }
  if (bmpsReady) {
    bool rawSamplingReady = true;
    for (pet::drivers::Bmp390* bmp : bmps) {
      rawSamplingReady = bmp->startRawSampling25Hz() && rawSamplingReady;
    }
    if (!rawSamplingReady) {
      Serial.println("BMP raw sampling configuration FAILED");
      bmpsReady = false;
    } else {
      Serial.println("BMP raw sampling OK rate_hz=25 cached_in_100hz_packets");
    }
    for (size_t i = 0; i < pet::data::kBmpCount; ++i) {
      sessionMetadata.bmpNvmValid[i] =
          bmps[i]->readNvm(sessionMetadata.bmpNvm[i]);
      Serial.print("BMP390 channel ");
      Serial.print(bmps[i]->muxChannel());
      Serial.println(sessionMetadata.bmpNvmValid[i]
                         ? " NVM calibration read OK (21 bytes)"
                         : " NVM calibration read FAILED");
    }
  }
  mux.disableAllChannels();

  if (sdCardReady) {
    if (sdLogger.beginSession(sessionMetadata, acquisition.counters())) {
      Serial.print("SD_SESSION_START folder=");
      Serial.print(sdLogger.sessionFolder());
      Serial.print(" buffer_bytes=");
      Serial.print(pet::config::kSdRamBufferBytes);
      Serial.print(" block_bytes=");
      Serial.print(pet::config::kSdWriteBlockBytes);
      Serial.print(" flush_packets=");
      Serial.println(pet::config::kSdPacketsPerFlush);
    } else {
      Serial.println("SD session creation FAILED; USB stream remains enabled");
    }
  }

  if (!sensorsReady || !bmpsReady) {
    Serial.println("Acquisition disabled because at least one sensor failed.");
    return;
  }

  Serial.print(pet::config::kUsbBinaryStreamEnabled
                   ? "BINARY_STREAM_START packet_size="
                   : "USB_BINARY_STREAM_DISABLED packet_size=");
  Serial.print(sizeof(pet::data::ImuPacket));
  Serial.print(" packet_version=");
  Serial.print(pet::config::kImuPacketVersion);
  Serial.print(" sample_rate_hz=");
  Serial.print(pet::config::kImuSampleRateHz);
  Serial.print(" bmp_rate_hz=");
  Serial.print(pet::config::kBmpSampleRateHz);
  Serial.print(" mag_rate_hz=");
  Serial.print(pet::config::kMagSampleRateHz);
  Serial.print(" mag_poll_rate_hz=");
  Serial.println(pet::config::kMagPollRateHz);

  acquisition.start(micros());
  acquisitionReady = true;
}

void loop() {
  if (!acquisitionReady) {
    return;
  }

  pet::data::ImuPacket packet{};
  if (!acquisition.poll(packet)) {
    return;
  }

  sdLogger.enqueue(packet);

  if (pet::config::kUsbBinaryStreamEnabled) {
    if (Serial && Serial.availableForWrite() >=
                      static_cast<int>(sizeof(pet::data::ImuPacket))) {
      Serial.write(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    } else {
      acquisition.recordUsbDrop();
    }
  }
  sdLogger.service(acquisition.counters());
}

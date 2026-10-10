#include <cassert>
#include <fstream>
#include <string>
#include "services/imu_acquisition.h"

uint32_t hostNowUs = 0;

int main(int argc, char** argv) {
  assert(argc == 3);
  const std::string mode = argv[1];
  pet::drivers::Icm20948 icm0, icm1, icm2;
  icm1.base = 200; icm2.base = 300;
  pet::drivers::Bmp390 bmp0, bmp1;
  bmp1.pressure = 0x070809; bmp1.temperature = 0x0a0b0c;
  pet::services::ImuAcquisition acquisition(icm0, icm1, icm2, bmp0, bmp1,
                                           10000, 4, 4);
  pet::data::ImuPacket packet{};
  std::ofstream output(argv[2], std::ios::binary);
  const auto save = [&] {
    output.write(reinterpret_cast<const char*>(&packet), sizeof(packet));
    assert(output.good());
  };
  acquisition.start(0);
  if (mode == "raw") {
    hostNowUs = 10000;
    assert(acquisition.poll(packet));
    assert(acquisition.counters().maxAcquisitionDurationUs == 270);
    assert(acquisition.counters().maxScheduleLatenessUs == 0);
    save();
  } else if (mode == "late") {
    hostNowUs = 35000;
    assert(acquisition.poll(packet));
    assert(packet.sequence == 2);
    assert(acquisition.counters().missedScheduleReadings == 2);
    assert(acquisition.counters().maxScheduleLatenessUs == 25000);
    save();
    assert(!acquisition.poll(packet));  // No catch-up burst.
    hostNowUs = 40000;
    assert(acquisition.poll(packet));
    assert(packet.sequence == 3);
    save();
  } else if (mode == "failure") {
    icm1.readable = false;
    hostNowUs = 10000;
    assert(!acquisition.poll(packet));
    assert(acquisition.counters().failedAcquisitionRounds == 1);
    assert(acquisition.counters().i2cFailures[1] == 1);
    assert(acquisition.counters().packetsProduced == 0);
    assert(acquisition.counters().maxAcquisitionDurationUs == 150);
    icm1.readable = true;
    hostNowUs = 20000;
    assert(acquisition.poll(packet));
    assert(packet.sequence == 1);
    save();
  } else if (mode == "rollover") {
    const uint32_t start = 0xfffff000U;
    acquisition.start(start);
    hostNowUs = start + 9999U;
    assert(!acquisition.poll(packet));
    hostNowUs = start + 10000U;
    assert(acquisition.poll(packet));
    assert(packet.sequence == 0);
    assert(acquisition.counters().missedScheduleReadings == 0);
    save();
  } else {
    return 2;
  }
}

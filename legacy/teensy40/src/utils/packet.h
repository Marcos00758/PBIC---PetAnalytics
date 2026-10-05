#pragma once

#include <Arduino.h>

#include "data/imu_packet.h"

namespace pet::utils {

void buildImuPacket(data::ImuPacket& packet, uint32_t timestampUs,
                    uint16_t sequence,
                    const int16_t (&values)[data::kImuValueCount],
                    const data::BmpRawValues (&bmp)[data::kBmpCount]);

}  // namespace pet::utils

#include "utils/packet.h"

#include <string.h>

#include "config/constants.h"
#include "utils/crc8.h"

namespace pet::utils {

void buildImuPacket(data::ImuPacket& packet, uint32_t timestampUs,
                    uint16_t sequence,
                    const int16_t (&values)[data::kImuValueCount],
                    const data::BmpRawValues (&bmp)[data::kBmpCount]) {
  packet.magic = config::kImuPacketMagic;
  packet.timestampUs = timestampUs;
  packet.sequence = sequence;
  memcpy(packet.values, values, sizeof(packet.values));
  memcpy(packet.bmp, bmp, sizeof(packet.bmp));
  packet.crc8 = crc8(reinterpret_cast<const uint8_t*>(&packet),
                     offsetof(data::ImuPacket, crc8));
}

}  // namespace pet::utils

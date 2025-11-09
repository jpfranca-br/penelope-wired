#include "netmask_utils.h"

bool isValidSubnetMaskOctets(const std::array<uint8_t, 4> &maskOctets) {
  bool seenZeroBit = false;
  for (uint8_t octet : maskOctets) {
    for (int bit = 7; bit >= 0; --bit) {
      bool maskBitSet = (octet >> bit) & 0x1;
      if (!maskBitSet) {
        seenZeroBit = true;
      } else if (seenZeroBit) {
        return false;
      }
    }
  }
  return true;
}


#include <array>
#include <cassert>

#include "../netmask_utils.h"

int main() {
  // Valid masks
  assert(isValidSubnetMaskOctets({{255, 255, 255, 0}}));
  assert(isValidSubnetMaskOctets({{255, 255, 0, 0}}));
  assert(isValidSubnetMaskOctets({{255, 255, 255, 252}}));
  assert(isValidSubnetMaskOctets({{255, 255, 255, 248}}));

  // Invalid masks
  assert(!isValidSubnetMaskOctets({{255, 0, 255, 0}}));
  assert(!isValidSubnetMaskOctets({{255, 255, 1, 0}}));

  return 0;
}


#pragma once

#include <cstdint>

namespace ota_boot {

// Next otadata sequence number that selects OTA slot `destOtaIdx` ((seq - 1) % 2 picks the slot) and
// beats `activeSeq`. Near UINT32_MAX (0xFFFFFFFF marks erased flash) it wraps to a small value; then
// `wrapped` is set and the caller must invalidate the other otadata sector, or the bootloader, which
// picks the highest valid seq, would keep booting the old entry.
struct SeqPlan {
  uint32_t seq;
  bool wrapped;
};

inline SeqPlan planNextSeq(uint32_t activeSeq, uint32_t destOtaIdx) {
  const bool wrap = activeSeq >= 0xFFFFFFFCu;
  uint32_t seq = (wrap ? 0u : activeSeq) + 1u;
  while (((seq - 1u) % 2u) != (destOtaIdx % 2u)) ++seq;
  return SeqPlan{seq, wrap};
}

}  // namespace ota_boot

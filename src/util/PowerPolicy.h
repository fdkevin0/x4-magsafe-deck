#pragma once

#include <cstdint>

namespace power_policy {

// Decide whether the CPU may drop to the low-power frequency.
// - `lastInteractionMs`: last button press/release or UI update that needs fast rendering.
// - `bleHandshakeActive`: BLE pairing / ANCS discovery is running and should not be slowed down.
// Uses unsigned wrap-around arithmetic so it stays correct when millis() overflows.
inline bool shouldUseLowPower(uint32_t nowMs, uint32_t lastInteractionMs, uint32_t idleThresholdMs,
                              bool bleHandshakeActive) {
  if (bleHandshakeActive) return false;
  return static_cast<uint32_t>(nowMs - lastInteractionMs) >= idleThresholdMs;
}

}  // namespace power_policy

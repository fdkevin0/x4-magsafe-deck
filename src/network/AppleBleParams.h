#pragma once

#include <cstdint>

// Bluetooth LE advertising / connection parameters recommended by Apple for accessories
// (Technical Q&A QA1931 and the Accessory Design Guidelines). Units follow the BLE spec:
// advertising interval and connection interval in 0.625 ms / 1.25 ms units, supervision timeout in
// 10 ms units.
namespace apple_ble {

// Advertise at 20 ms for the first 30 s, then fall back to one of Apple's exact slow intervals
// (152.5, 211.25, 318.75, 417.5, 546.25, 760, 852.5, 1022.5 or 1285 ms) to save power.
inline constexpr uint16_t kFastAdvIntervalUnits = 32;  // 20 ms
inline constexpr uint32_t kFastAdvDurationMs = 30000;
inline constexpr uint16_t kSlowAdvIntervalUnits = 1636;  // 1022.5 ms

inline constexpr uint16_t kAppleSlowAdvIntervalsUnits[] = {244, 338, 510, 668, 874, 1216, 1364, 1636, 2056};

// Apple's connection-parameter rules (QA1931):
//   Interval Min >= 15 ms and a multiple of 15 ms; Interval Min + 15 ms <= Interval Max
//   (or Min == Max == 15 ms); Interval Max * (Latency + 1) <= 2 s;
//   Interval Max * (Latency + 1) * 3 < Supervision Timeout; Latency <= 30; 2 s <= Timeout <= 6 s.
inline constexpr bool connParamsValid(uint16_t minUnits, uint16_t maxUnits, uint16_t latency, uint16_t timeoutUnits) {
  // Work in 0.25 ms ticks so 1.25 ms units (x5) and 10 ms units (x40) stay integral.
  const uint32_t minQ = static_cast<uint32_t>(minUnits) * 5;
  const uint32_t maxQ = static_cast<uint32_t>(maxUnits) * 5;
  const uint32_t timeoutQ = static_cast<uint32_t>(timeoutUnits) * 40;
  constexpr uint32_t k15ms = 60;
  if (minQ < k15ms || (minQ % k15ms) != 0) return false;
  const bool bothAt15 = (minQ == k15ms && maxQ == k15ms);
  if (!bothAt15 && minQ + k15ms > maxQ) return false;
  if (latency > 30) return false;
  const uint32_t effectiveQ = maxQ * (static_cast<uint32_t>(latency) + 1);
  if (effectiveQ > 2000 * 4) return false;
  if (effectiveQ * 3 >= timeoutQ) return false;
  return timeoutQ >= 2000 * 4 && timeoutQ <= 6000 * 4;
}

// Connection parameters requested after ANCS subscription: 30–60 ms interval, latency 4, 4 s timeout.
// Latency is intentionally NOT raised to Apple's maximum of 30: every GetNotificationAttributes
// response is central-to-peripheral traffic, so with latency 30 each one could wait up to
// 60 ms * 31 = 1.86 s (close to the 2 s attribute timeout), and without ESP-IDF light sleep the CPU
// stays awake anyway, so the radio savings would be small.
inline constexpr uint16_t kConnIntervalMinUnits = 24;  // 30 ms
inline constexpr uint16_t kConnIntervalMaxUnits = 48;  // 60 ms
inline constexpr uint16_t kConnLatency = 4;
inline constexpr uint16_t kConnSupervisionTimeoutUnits = 400;  // 4 s
static_assert(connParamsValid(kConnIntervalMinUnits, kConnIntervalMaxUnits, kConnLatency, kConnSupervisionTimeoutUnits),
              "connection parameters violate Apple QA1931");

}  // namespace apple_ble

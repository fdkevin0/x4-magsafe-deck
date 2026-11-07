#pragma once

#include <algorithm>

namespace qr_layout {

// QR spec (ISO/IEC 18004) requires a light "quiet zone" at least 4 modules wide on every side.
inline constexpr int kQuietZoneModules = 4;

struct Layout {
  int modulePx = 0;  // Side of one module in pixels (0 = does not fit).
  int originX = 0;   // Top-left pixel of the first module.
  int originY = 0;
  int outerX = 0;  // Top-left of the symbol including its quiet zone (area to clear to white).
  int outerY = 0;
  int outerSide = 0;
};

// Fit a `modules` x `modules` symbol plus its quiet zone into the box, using an integer module size
// (crisp edges on e-ink) and centering the result.
inline Layout fit(int modules, int boxX, int boxY, int boxW, int boxH) {
  Layout l{};
  if (modules <= 0 || boxW <= 0 || boxH <= 0) return l;
  const int totalModules = modules + 2 * kQuietZoneModules;
  const int px = std::min(boxW, boxH) / totalModules;
  if (px < 1) return l;
  l.modulePx = px;
  l.outerSide = totalModules * px;
  l.outerX = boxX + (boxW - l.outerSide) / 2;
  l.outerY = boxY + (boxH - l.outerSide) / 2;
  l.originX = l.outerX + kQuietZoneModules * px;
  l.originY = l.outerY + kQuietZoneModules * px;
  return l;
}

}  // namespace qr_layout

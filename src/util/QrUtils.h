#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <string>

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

namespace QrUtils {

// Largest QR version drawn. Version 10 at ECC LOW holds 271 bytes in byte mode,
// which covers every badge payload (BadgeCard::kQrMax - 1 = 255 bytes) and keeps
// the two qrcodegen work buffers at ~400 B each instead of ~3.9 KB.
inline constexpr int kMaxVersion = 10;
inline constexpr size_t kMaxPayloadBytes = 271;

// Renders a QR code with the given text payload within the specified bounding box.
void drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload);

}  // namespace QrUtils

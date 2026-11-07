#include "QrUtils.h"

#include <Utf8.h>
#include <qrcode.h>

#include <algorithm>
#include <memory>

#include "Logging.h"
#include "Memory.h"
#include "QrLayout.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Dynamically calculate the QR code version based on text length
  // Version 4 holds ~114 bytes, Version 10 ~395, Version 20 ~1066, up to 40
  // qrcode.h max version is 40.
  // Formula: approx version = size / 26 + 1 (very rough estimate, better to find best fit)
  size_t len = textPayload.length();

  // Truncate to max QR capacity at a UTF-8 safe boundary to avoid splitting multi-byte sequences
  static constexpr size_t MAX_QR_CAPACITY = 2953;  // Version 40, ECC_LOW, byte mode
  std::string truncated;
  const char* payload = textPayload.c_str();
  if (len > MAX_QR_CAPACITY) {
    len = utf8SafeTruncateBuffer(textPayload.c_str(), static_cast<int>(MAX_QR_CAPACITY));
    truncated = textPayload.substr(0, len);
    payload = truncated.c_str();
  }

  // Select initial QR version using Byte-mode ECC_LOW capacities:
  // v4: 78 B, v6: 134 B, v10: 271 B, v20: 858 B, v30: 1732 B, v40: 2953 B.
  int version = 4;
  if (len > 78) version = 6;
  if (len > 134) version = 10;
  if (len > 271) version = 20;
  if (len > 858) version = 30;
  if (len > 1732) version = 40;

  QRCode qrcode;
  std::unique_ptr<uint8_t[]> qrcodeBytes;
  int8_t res = -1;
  while (version <= 40) {
    const uint32_t bufferSize = qrcode_getBufferSize(version);
    qrcodeBytes = makeUniqueNoThrow<uint8_t[]>(bufferSize);
    if (!qrcodeBytes) {
      LOG_ERR("QR", "Failed to allocate %u bytes for QR buffer", static_cast<unsigned>(bufferSize));
      return;
    }
    res = qrcode_initText(&qrcode, qrcodeBytes.get(), version, ECC_LOW, payload);
    if (res == 0) break;
    version += 2;
  }

  if (res == 0) {
    // Integer module size that leaves room for the 4-module quiet zone scanners need.
    const qr_layout::Layout layout = qr_layout::fit(qrcode.size, bounds.x, bounds.y, bounds.width, bounds.height);
    if (layout.modulePx < 1) {
      LOG_ERR("QR", "Bounds %dx%d too small for a %d-module QR code", bounds.width, bounds.height, qrcode.size);
      return;
    }
    renderer.fillRect(layout.outerX, layout.outerY, layout.outerSide, layout.outerSide, false);

    const int px = layout.modulePx;
    for (uint8_t cy = 0; cy < qrcode.size; cy++) {
      for (uint8_t cx = 0; cx < qrcode.size; cx++) {
        if (qrcode_getModule(&qrcode, cx, cy)) {
          renderer.fillRect(layout.originX + px * cx, layout.originY + px * cy, px, px, true);
        }
      }
    }
  } else {
    // If it fails (e.g. text too large), log an error
    LOG_ERR("QR", "Text too large for QR Code version %d", version);
  }
}

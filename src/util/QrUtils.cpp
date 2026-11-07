#include "QrUtils.h"

#include <Utf8.h>
#include <qrcodegen.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "Logging.h"
#include "Memory.h"
#include "QrLayout.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Payloads longer than the kMaxVersion byte-mode capacity are cut at a UTF-8
  // boundary so a multi-byte sequence is never split.
  std::string truncated;
  const char* payload = textPayload.c_str();
  if (textPayload.length() > kMaxPayloadBytes) {
    const size_t len = utf8SafeTruncateBuffer(textPayload.c_str(), static_cast<int>(kMaxPayloadBytes));
    truncated = textPayload.substr(0, len);
    payload = truncated.c_str();
  }

  // qrcodegen needs two work buffers sized for the largest allowed version; keep them off the stack.
  static constexpr size_t kBufLen = qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion);
  auto qrcode = makeUniqueNoThrow<uint8_t[]>(kBufLen);
  auto temp = makeUniqueNoThrow<uint8_t[]>(kBufLen);
  if (!qrcode || !temp) {
    LOG_ERR("QR", "Failed to allocate %u bytes for QR buffers", static_cast<unsigned>(2 * kBufLen));
    return;
  }

  // Picks the smallest version that fits, numeric/alphanumeric mode when the text
  // allows it, and raises ECC while the version stays the same.
  if (!qrcodegen_encodeText(payload, temp.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, kMaxVersion,
                            qrcodegen_Mask_AUTO, /*boostEcl=*/true)) {
    LOG_ERR("QR", "Text too large for a QR code (%u bytes)", static_cast<unsigned>(std::strlen(payload)));
    return;
  }

  const int size = qrcodegen_getSize(qrcode.get());

  // Integer module size that leaves room for the 4-module quiet zone scanners need.
  const qr_layout::Layout layout = qr_layout::fit(size, bounds.x, bounds.y, bounds.width, bounds.height);
  if (layout.modulePx < 1) {
    LOG_ERR("QR", "Bounds %dx%d too small for a %d-module QR code", bounds.width, bounds.height, size);
    return;
  }
  renderer.fillRect(layout.outerX, layout.outerY, layout.outerSide, layout.outerSide, false);

  const int px = layout.modulePx;
  for (int cy = 0; cy < size; cy++) {
    for (int cx = 0; cx < size; cx++) {
      if (qrcodegen_getModule(qrcode.get(), cx, cy)) {
        renderer.fillRect(layout.originX + px * cx, layout.originY + px * cy, px, px, true);
      }
    }
  }
}

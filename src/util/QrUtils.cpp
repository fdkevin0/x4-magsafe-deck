#include "QrUtils.h"

#include <Utf8.h>
#include <qrcodegen.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "Logging.h"
#include "Memory.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Byte-mode capacity of a version 40 symbol at ECC LOW. Longer payloads are cut
  // at a UTF-8 boundary so a multi-byte sequence is never split.
  static constexpr size_t kMaxQrCapacity = 2953;
  std::string truncated;
  const char* payload = textPayload.c_str();
  if (textPayload.length() > kMaxQrCapacity) {
    const size_t len = utf8SafeTruncateBuffer(textPayload.c_str(), static_cast<int>(kMaxQrCapacity));
    truncated = textPayload.substr(0, len);
    payload = truncated.c_str();
  }

  // qrcodegen needs two buffers of up to ~3.9 KB each; keep them off the stack.
  static constexpr size_t kBufLen = qrcodegen_BUFFER_LEN_MAX;
  auto qrcode = makeUniqueNoThrow<uint8_t[]>(kBufLen);
  auto temp = makeUniqueNoThrow<uint8_t[]>(kBufLen);
  if (!qrcode || !temp) {
    LOG_ERR("QR", "Failed to allocate %u bytes for QR buffers", static_cast<unsigned>(2 * kBufLen));
    return;
  }

  // Picks the smallest version that fits, numeric/alphanumeric mode when the text
  // allows it, and raises ECC while the version stays the same.
  if (!qrcodegen_encodeText(payload, temp.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                            qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, /*boostEcl=*/true)) {
    LOG_ERR("QR", "Text too large for a QR code (%u bytes)", static_cast<unsigned>(std::strlen(payload)));
    return;
  }

  const int size = qrcodegen_getSize(qrcode.get());
  const int maxDim = std::min(bounds.width, bounds.height);
  const int px = std::max(1, maxDim / size);

  const int qrDisplaySize = size * px;
  const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
  const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

  for (int cy = 0; cy < size; cy++) {
    for (int cx = 0; cx < size; cx++) {
      if (qrcodegen_getModule(qrcode.get(), cx, cy)) {
        renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
      }
    }
  }
}

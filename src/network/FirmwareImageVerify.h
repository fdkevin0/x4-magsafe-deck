#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "Memory.h"

namespace firmware_flash {

namespace detail {
constexpr uint8_t kEspImageMagic = 0xE9;
constexpr size_t kMinFirmwareSize = 64 * 1024;
constexpr size_t kMaxSegments = 16;
constexpr size_t kChunkSize = 4096;
constexpr size_t kShaTrailerSize = 32;
constexpr uint8_t kChecksumSeed = 0xEF;
constexpr size_t kHeaderSize = 24;
constexpr size_t kSegHeaderSize = 8;

using SequentialReadFn = bool (*)(void* ctx, uint8_t* buf, size_t len);

// Validate the 24-byte ESP32 image header (magic, segment count, entry point,
// target chip ID, and SHA-256 trailer flag).
inline Result validateImageHeader(const uint8_t (&header)[kHeaderSize], uint16_t expectedChipId,
                                  uint8_t* outSegCount = nullptr, bool* outHashAppended = nullptr) {
  if (header[0] != kEspImageMagic) {
    return Result::BAD_MAGIC;
  }

  const uint8_t segCount = header[1];
  if (segCount == 0 || segCount > kMaxSegments) {
    return Result::BAD_SEGMENTS;
  }

  uint32_t entryAddr = 0;
  std::memcpy(&entryAddr, header + 4, sizeof(entryAddr));
  if (entryAddr == 0 || entryAddr == 0xFFFFFFFFu) {
    return Result::BAD_MAGIC;
  }

  uint16_t imageChip = 0xFFFF;
  std::memcpy(&imageChip, header + 12, sizeof(imageChip));
  if (expectedChipId != 0xFFFF && imageChip != expectedChipId) {
    return Result::BAD_CHIP;
  }

  if (header[23] > 1) {
    return Result::BAD_MAGIC;
  }

  if (outSegCount) *outSegCount = segCount;
  if (outHashAppended) *outHashAppended = (header[23] != 0);
  return Result::OK;
}

// Stream `length` bytes from `readFn`, updating the XOR checksum, SHA-256 context,
// and board-tag scanner in 4 KiB chunks.
inline Result feedStreamHashAndChecksum(SequentialReadFn readFn, void* ctx, size_t length, uint8_t* xorAccum,
                                        mbedtls_sha256_context* sha, uint8_t* buf, board_tag::Scanner* tagScanner) {
  size_t remaining = length;
  size_t chunks = 0;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(kChunkSize, remaining);
    if (!readFn(ctx, buf, want)) {
      return Result::READ_FAIL;
    }
    if (sha) {
      mbedtls_sha256_update(sha, buf, want);
    }
    if (tagScanner) {
      tagScanner->feed(buf, want);
    }
    if (xorAccum) {
      uint8_t acc = *xorAccum;
      for (size_t i = 0; i < want; i++) {
        acc ^= buf[i];
      }
      *xorAccum = acc;
    }
    remaining -= want;
#ifdef ARDUINO
    if ((++chunks & 15u) == 0u) {
      delay(1);
    }
#else
    (void)chunks;
#endif
  }
  return Result::OK;
}

// Validate an ESP32 application image from a sequential byte stream.
// - When `exactSize == true`, `maxOrExactSize` is the exact file byte length and the
//   image's computed end (including optional 32-byte SHA-256 trailer) must equal it.
// - When `exactSize == false`, `maxOrExactSize` is the partition capacity and the
//   self-delimited image length must lie within `[kMinFirmwareSize, maxOrExactSize]`.
inline Result validateImageStream(SequentialReadFn readFn, void* ctx, size_t maxOrExactSize, bool exactSize,
                                  uint16_t expectedChipId, board_tag::Scanner* tagScanner) {
  if (maxOrExactSize < kMinFirmwareSize) {
    return Result::TOO_SMALL;
  }

  uint8_t header[kHeaderSize];
  if (!readFn(ctx, header, kHeaderSize)) {
    return Result::READ_FAIL;
  }
  uint8_t segCount = 0;
  bool hashAppended = false;
  const Result hdrRes = validateImageHeader(header, expectedChipId, &segCount, &hashAppended);
  if (hdrRes != Result::OK) {
    return hdrRes;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(kChunkSize);
  if (!buf) {
    return Result::OOM;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  mbedtls_sha256_update(&shaCtx, header, kHeaderSize);

  uint8_t xorAccum = kChecksumSeed;
  size_t pos = kHeaderSize;

  for (uint8_t i = 0; i < segCount; i++) {
    if (pos + kSegHeaderSize > maxOrExactSize) {
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SEGMENTS;
    }
    uint8_t segHdr[kSegHeaderSize];
    if (!readFn(ctx, segHdr, kSegHeaderSize)) {
      mbedtls_sha256_free(&shaCtx);
      return Result::READ_FAIL;
    }
    mbedtls_sha256_update(&shaCtx, segHdr, kSegHeaderSize);
    pos += kSegHeaderSize;

    uint32_t dataLen = 0;
    std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));
    if ((dataLen & 3u) != 0u || dataLen > maxOrExactSize || pos + dataLen > maxOrExactSize) {
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SEGMENTS;
    }

    const Result feedRes =
        feedStreamHashAndChecksum(readFn, ctx, dataLen, &xorAccum, &shaCtx, buf.get(), tagScanner);
    if (feedRes != Result::OK) {
      mbedtls_sha256_free(&shaCtx);
      return feedRes;
    }
    pos += dataLen;
  }

  if (tagScanner && tagScanner->mismatch()) {
    mbedtls_sha256_free(&shaCtx);
    return Result::WRONG_BOARD;
  }

  const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
  const size_t expectedTotal = padEnd + (hashAppended ? kShaTrailerSize : 0);
  if (exactSize) {
    if (expectedTotal != maxOrExactSize) {
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SIZE;
    }
  } else {
    if (expectedTotal < kMinFirmwareSize) {
      mbedtls_sha256_free(&shaCtx);
      return Result::TOO_SMALL;
    }
    if (expectedTotal > maxOrExactSize) {
      mbedtls_sha256_free(&shaCtx);
      return Result::TOO_LARGE;
    }
  }

  const size_t padLen = padEnd - pos;
  uint8_t padBuf[16];
  if (padLen == 0 || padLen > sizeof(padBuf)) {
    mbedtls_sha256_free(&shaCtx);
    return Result::BAD_SIZE;
  }
  if (!readFn(ctx, padBuf, padLen)) {
    mbedtls_sha256_free(&shaCtx);
    return Result::READ_FAIL;
  }
  mbedtls_sha256_update(&shaCtx, padBuf, padLen);

  const uint8_t storedChecksum = padBuf[padLen - 1];
  if ((xorAccum & 0xFF) != storedChecksum) {
    mbedtls_sha256_free(&shaCtx);
    return Result::BAD_CHECKSUM;
  }

  if (hashAppended) {
    uint8_t computed[kShaTrailerSize];
    mbedtls_sha256_finish(&shaCtx, computed);
    uint8_t stored[kShaTrailerSize];
    if (!readFn(ctx, stored, kShaTrailerSize)) {
      mbedtls_sha256_free(&shaCtx);
      return Result::READ_FAIL;
    }
    if (std::memcmp(computed, stored, kShaTrailerSize) != 0) {
      mbedtls_sha256_free(&shaCtx);
      return Result::BAD_SHA;
    }
  }

  mbedtls_sha256_free(&shaCtx);
  return Result::OK;
}

}  // namespace detail
}  // namespace firmware_flash

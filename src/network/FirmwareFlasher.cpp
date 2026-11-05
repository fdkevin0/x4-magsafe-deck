#include "FirmwareFlasher.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FirmwareBoardTag.h"
#include "FirmwareImageVerify.h"
#include "OtaBootSwitch.h"

namespace firmware_flash {

namespace {
constexpr size_t MIN_FIRMWARE_SIZE = detail::kMinFirmwareSize;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
constexpr size_t BLK = 64 * 1024;           // 64 KiB block-erase granularity
constexpr size_t CHUNK = detail::kChunkSize;
}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::OPEN_FAIL:
      return "OPEN_FAIL";
    case Result::TOO_SMALL:
      return "TOO_SMALL";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_MAGIC:
      return "BAD_MAGIC";
    case Result::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Result::BAD_SHA:
      return "BAD_SHA";
    case Result::BAD_CHIP:
      return "BAD_CHIP";
    case Result::WRONG_BOARD:
      return "WRONG_BOARD";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::NO_PARTITION:
      return "NO_PARTITION";
    case Result::OOM:
      return "OOM";
    case Result::READ_FAIL:
      return "READ_FAIL";
    case Result::ERASE_FAIL:
      return "ERASE_FAIL";
    case Result::WRITE_FAIL:
      return "WRITE_FAIL";
    case Result::OTADATA_FAIL:
      return "OTADATA_FAIL";
  }
  return "?";
}

uint16_t runningPartitionChipId() {
  // esp_partition_read hits SPI flash; cache the running slot's chip_id so we
  // only pay that cost once per boot. The running image is immutable at
  // runtime, so a function-local static is safe here.
  static uint16_t cached = [] {
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (!run) return static_cast<uint16_t>(0xFFFF);
    uint16_t id = 0xFFFF;
    // chip_id sits at offset 12 of esp_image_header_t. memcpy target is a
    // uint16_t local, so RISC-V alignment is guaranteed.
    if (esp_partition_read(run, 12, &id, sizeof(id)) != ESP_OK) return static_cast<uint16_t>(0xFFFF);
    return id;
  }();
  return cached;
}

namespace {
bool readFromHalFile(void* ctx, uint8_t* buf, size_t len) {
  auto* file = static_cast<HalFile*>(ctx);
  const int got = file->read(buf, len);
  return got > 0 && static_cast<size_t>(got) == len;
}

struct PartitionReadCtx {
  const esp_partition_t* part;
  size_t offset;
};

bool readFromPartition(void* ctx, uint8_t* buf, size_t len) {
  auto* pctx = static_cast<PartitionReadCtx*>(ctx);
  if (pctx->offset + len > pctx->part->size) return false;
  if (esp_partition_read(pctx->part, pctx->offset, buf, len) != ESP_OK) return false;
  pctx->offset += len;
  return true;
}
}  // namespace

Result validateImageFile(const char* sdPath, size_t partitionSize) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "validate: too small: %u", static_cast<unsigned>(fileSize));
    file.close();
    return Result::TOO_SMALL;
  }
  if (partitionSize > 0 && fileSize > partitionSize) {
    LOG_ERR("FLASH", "validate: too large: %u > %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(partitionSize));
    file.close();
    return Result::TOO_LARGE;
  }

  board_tag::Scanner tagScanner;
  const Result res = detail::validateImageStream(readFromHalFile, &file, fileSize, /*exactSize=*/true,
                                                 runningPartitionChipId(), &tagScanner);
  if (res != Result::OK) {
    LOG_ERR("FLASH", "validate file failed: %s", resultName(res));
  }
  file.close();
  return res;
}

Result validatePartition(const esp_partition_t* part) {
  if (!part) {
    return Result::NO_PARTITION;
  }
  PartitionReadCtx ctx{part, 0};
  board_tag::Scanner tagScanner;
  const Result res = detail::validateImageStream(readFromPartition, &ctx, part->size, /*exactSize=*/false,
                                                 runningPartitionChipId(), &tagScanner);
  if (res != Result::OK) {
    LOG_ERR("FLASH", "validate partition %s failed: %s", part->label, resultName(res));
  }
  return res;
}

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated) {
  // Resolve destination first so we can size-check during validation. The full image-integrity
  // pass below verifies header, segment table, XOR checksum and SHA256 trailer end-to-end before
  // we touch otadata, so a truncated/corrupted .bin can never become the next boot target.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }

  // When the caller already ran validateImageFile() against this same partition
  // size (e.g. SdFirmwareUpdateActivity validates before the confirmation
  // prompt), skip the redundant integrity scan. We still keep the partition
  // lookup so the rest of the flashing path stays unchanged.
  if (!alreadyValidated) {
    const Result validateRes = validateImageFile(sdPath, dest->size);
    if (validateRes != Result::OK) {
      LOG_ERR("FLASH", "image validation failed: %s", resultName(validateRes));
      return validateRes;
    }
  }

  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t firmwareSize = file.fileSize();
  if (firmwareSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "too small: %u", static_cast<unsigned>(firmwareSize));
    file.close();
    return Result::TOO_SMALL;
  }
  if (firmwareSize > dest->size) {
    LOG_ERR("FLASH", "too large: %u > %u", static_cast<unsigned>(firmwareSize), static_cast<unsigned>(dest->size));
    file.close();
    return Result::TOO_LARGE;
  }
  LOG_INF("FLASH", "src=%s size=%u dest=%s @0x%x partsize=%u", sdPath, static_cast<unsigned>(firmwareSize), dest->label,
          static_cast<unsigned>(dest->address), static_cast<unsigned>(dest->size));

  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[CHUNK]);
  auto sector0 = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[SEC]);
  if (!buffer || !sector0) {
    LOG_ERR("FLASH", "OOM");
    file.close();
    return Result::OOM;
  }

  // Hold back sector 0 (which contains the 0xE9 magic header) until all remaining
  // sectors are written so an interrupted flash never leaves a bootable header at offset 0.
  size_t streamPos = 0;
  size_t erasedUpto = 0;
  while (streamPos < firmwareSize) {
    if (streamPos >= erasedUpto) {
      size_t eraseLen = std::min<size_t>(BLK, dest->size - streamPos);
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      eraseLen = std::min<size_t>(eraseLen, dest->size - streamPos);
      if (esp_partition_erase_range(dest, streamPos, eraseLen) != ESP_OK) {
        LOG_ERR("FLASH", "erase @%u (len=%u) failed", static_cast<unsigned>(streamPos),
                static_cast<unsigned>(eraseLen));
        file.close();
        return Result::ERASE_FAIL;
      }
      erasedUpto = streamPos + eraseLen;
    }

    const size_t want = std::min<size_t>(CHUNK, firmwareSize - streamPos);
    uint8_t* dstBuf = (streamPos == 0) ? sector0.get() : buffer.get();
    const int read = file.read(dstBuf, want);
    if (read <= 0 || static_cast<size_t>(read) != want) {
      LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(streamPos), read, static_cast<unsigned>(want));
      file.close();
      return Result::READ_FAIL;
    }
    if (streamPos > 0) {
      if (esp_partition_write(dest, streamPos, dstBuf, want) != ESP_OK) {
        LOG_ERR("FLASH", "write @%u failed", static_cast<unsigned>(streamPos));
        file.close();
        return Result::WRITE_FAIL;
      }
    }
    streamPos += want;
    if (onProgress) onProgress(streamPos, firmwareSize, ctx);
    delay(1);
  }
  file.close();

  // Commit sector 0 last now that the body + trailer are on flash.
  const size_t sec0Len = std::min<size_t>(SEC, firmwareSize);
  if (esp_partition_write(dest, 0, sector0.get(), sec0Len) != ESP_OK) {
    LOG_ERR("FLASH", "write @0 failed");
    return Result::WRITE_FAIL;
  }

  // Verify the written partition in SPI flash before flipping otadata.
  const Result postCheck = validatePartition(dest);
  if (postCheck != Result::OK) {
    LOG_ERR("FLASH", "post-write verification failed: %s", resultName(postCheck));
    return postCheck;
  }

  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("FLASH", "otadata switch failed");
    return Result::OTADATA_FAIL;
  }
  return Result::OK;
}

}  // namespace firmware_flash

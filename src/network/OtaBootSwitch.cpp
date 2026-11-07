#include "OtaBootSwitch.h"

#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <spi_flash_mmap.h>
#include <string.h>

#include "FirmwareFlasher.h"
#include "FirmwareImageVerify.h"

namespace ota_boot {

uint32_t computeSeqCrc(uint32_t seq) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), kOtaSeqCrcLen);
}

bool switchTo(const esp_partition_t* dest) {
  if (!dest) return false;

  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) {
    LOG_ERR("BOOT", "otadata partition not found");
    return false;
  }
  if (otadata->size < 2 * SPI_FLASH_SEC_SIZE) {
    LOG_ERR("BOOT", "otadata too small: %u", static_cast<unsigned>(otadata->size));
    return false;
  }

  SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(SelectEntry)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata read failed");
    return false;
  }

  // Pick the slot with valid CRC and highest seq, ignoring INVALID/ABORTED.
  int activeIdx = -1;
  uint32_t activeSeq = 0;
  bool slotValid[2] = {false, false};
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu || slots[i].ota_seq == 0u) continue;
    if (slots[i].crc != computeSeqCrc(slots[i].ota_seq)) continue;
    if (slots[i].ota_state == kOtaImgInvalid || slots[i].ota_state == kOtaImgAborted) continue;
    slotValid[i] = true;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }
  LOG_INF("BOOT", "otadata: active slot=%d seq=%u", activeIdx, static_cast<unsigned>(activeSeq));

  // ota_seq encoding: (seq - 1) % NUM_OTA_PARTITIONS picks the partition.
  const uint32_t destOtaIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destOtaIdx > 15) {
    LOG_ERR("BOOT", "dest is not an OTA app partition (subtype=0x%02X)", dest->subtype);
    return false;
  }

  const SeqPlan plan = planNextSeq(activeSeq, destOtaIdx);
  const uint32_t newSeq = plan.seq;

  SelectEntry next = {};
  next.ota_seq = newSeq;
  memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  next.ota_state = kOtaImgValid;
  next.crc = computeSeqCrc(next.ota_seq);

  // Anchor targetSlot selection so we never overwrite the only valid sector mapping
  // to the currently running partition (e.g. if the bootloader fell back from a broken slot).
  int targetSlot = (activeIdx == 0) ? 1 : 0;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0) {
    const uint32_t runOtaIdx =
        static_cast<uint32_t>(running->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    const bool s0MapsRun = slotValid[0] && (((slots[0].ota_seq - 1u) % 2u) == (runOtaIdx % 2u));
    const bool s1MapsRun = slotValid[1] && (((slots[1].ota_seq - 1u) % 2u) == (runOtaIdx % 2u));
    if (s0MapsRun && !s1MapsRun) {
      targetSlot = 1;
    } else if (s1MapsRun && !s0MapsRun) {
      targetSlot = 0;
    }
  }
  const size_t targetOff = static_cast<size_t>(targetSlot) * SPI_FLASH_SEC_SIZE;

  if (esp_partition_erase_range(otadata, targetOff, SPI_FLASH_SEC_SIZE) != ESP_OK) {
    LOG_ERR("BOOT", "otadata erase failed (slot=%d)", targetSlot);
    return false;
  }
  if (esp_partition_write(otadata, targetOff, &next, sizeof(next)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata write failed (slot=%d)", targetSlot);
    return false;
  }

  LOG_INF("BOOT", "otadata: wrote slot=%d seq=%u crc=0x%08x -> %s", targetSlot, static_cast<unsigned>(newSeq),
          static_cast<unsigned>(next.crc), dest->label);

  if (plan.wrapped) {
    // The new entry restarted from a small seq, but the bootloader picks the highest valid seq, so the
    // other sector (holding a seq near UINT32_MAX) would still win. Invalidate it now that the new
    // entry is on flash; if power fails in between, the old mapping simply stays in effect.
    const size_t otherOff = static_cast<size_t>(1 - targetSlot) * SPI_FLASH_SEC_SIZE;
    if (esp_partition_erase_range(otadata, otherOff, SPI_FLASH_SEC_SIZE) != ESP_OK) {
      LOG_ERR("BOOT", "otadata: seq wrapped but erasing slot=%d failed", 1 - targetSlot);
      return false;
    }
    LOG_INF("BOOT", "otadata: seq wrapped; erased slot=%d", 1 - targetSlot);
  }
  return true;
}

bool isPartitionBootable(const esp_partition_t* part) {
  if (!part) return false;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && part->address == running->address) return false;
  return firmware_flash::validatePartition(part) == firmware_flash::Result::OK;
}

bool hasBootableNextPartition() {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) return false;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && next->address == running->address) return false;

  uint8_t header[firmware_flash::detail::kHeaderSize] = {};
  if (esp_partition_read(next, 0, header, sizeof(header)) != ESP_OK) return false;
  return firmware_flash::detail::validateImageHeader(header, firmware_flash::runningPartitionChipId()) ==
         firmware_flash::Result::OK;
}

bool switchToNextPartition() {
  HalPowerManager::Lock powerLock;
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) {
    LOG_ERR("BOOT", "no alternate OTA partition found");
    return false;
  }
  if (!isPartitionBootable(next)) {
    LOG_ERR("BOOT", "alternate partition %s is empty or invalid", next->label);
    return false;
  }
  return switchTo(next);
}

}  // namespace ota_boot

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ancs {

// Apple Notification Center Service (ANCS) UUIDs (little-endian wire order for BLE).
// Service:             7905F431-B5CE-4E99-A40F-4B1E122D00D0
// Notification Source: 9FBF120D-6301-42D9-8C58-25E699A21DBD
// Control Point:       69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
// Data Source:         22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
inline constexpr const char* kAncsServiceUuid = "7905F431-B5CE-4E99-A40F-4B1E122D00D0";
inline constexpr const char* kAncsNotificationSourceUuid = "9FBF120D-6301-42D9-8C58-25E699A21DBD";
inline constexpr const char* kAncsControlPointUuid = "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9";
inline constexpr const char* kAncsDataSourceUuid = "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB";

// 18-byte AD structure: length=17 (0x11), AD type=0x15 (128-bit Service Solicitation),
// followed by 7905F431-B5CE-4E99-A40F-4B1E122D00D0 in little-endian byte order.
inline constexpr uint8_t kAncsSolicitationAd[18] = {
    0x11, 0x15, 0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
};

enum class EventId : uint8_t {
  Added = 0,
  Modified = 1,
  Removed = 2,
};

enum EventFlags : uint8_t {
  kEventFlagSilent = 1U << 0,
  kEventFlagImportant = 1U << 1,
  kEventFlagPreExisting = 1U << 2,
  kEventFlagPositiveAction = 1U << 3,
  kEventFlagNegativeAction = 1U << 4,
};

enum class CommandId : uint8_t {
  GetNotificationAttributes = 0,
  GetAppAttributes = 1,
  PerformNotificationAction = 2,
};

enum class AttributeId : uint8_t {
  AppIdentifier = 0,
  Title = 1,
  Subtitle = 2,
  Message = 3,
  MessageSize = 4,
  Date = 5,
  PositiveActionLabel = 6,
  NegativeActionLabel = 7,
};

enum class AppAttributeId : uint8_t {
  DisplayName = 0,
};

enum class ActionId : uint8_t {
  Positive = 0,
  Negative = 1,
};

struct NotificationSourceEvent {
  EventId eventId;
  uint8_t eventFlags;
  uint8_t categoryId;
  uint8_t categoryCount;
  uint32_t uid;
};

struct AncsNotification {
  static constexpr size_t kAppIdMax = 40;
  static constexpr size_t kTitleMax = 64;
  static constexpr size_t kMessageMax = 192;
  static constexpr size_t kDateMax = 20;
  static constexpr size_t kAppNameMax = 32;

  uint32_t uid = 0;
  uint8_t categoryId = 0;
  uint8_t eventFlags = 0;
  bool valid = false;
  char appId[kAppIdMax] = {0};
  char title[kTitleMax] = {0};
  char message[kMessageMax] = {0};
  char date[kDateMax] = {0};
  // App display name from GetAppAttributes; empty until iOS has answered for this appId.
  char appName[kAppNameMax] = {0};

  void clear() {
    uid = 0;
    categoryId = 0;
    eventFlags = 0;
    valid = false;
    appId[0] = '\0';
    title[0] = '\0';
    message[0] = '\0';
    date[0] = '\0';
    appName[0] = '\0';
  }
};

// Truncate `buf` in-place so a multi-byte UTF-8 sequence at the tail is never
// split across the buffer boundary. `len` is the current number of bytes before '\0'.
inline size_t utf8SafeTruncate(char* buf, size_t len) {
  if (!buf || len == 0) {
    if (buf) buf[0] = '\0';
    return 0;
  }
  size_t i = len;
  while (i > 0 && (static_cast<uint8_t>(buf[i - 1]) & 0xC0U) == 0x80U) {
    --i;
  }
  if (i == 0) {
    buf[0] = '\0';
    return 0;
  }
  const size_t tailBytes = len - (i - 1);
  const uint8_t lead = static_cast<uint8_t>(buf[i - 1]);
  size_t seqLen = 0;
  if ((lead & 0x80U) == 0x00U) {
    seqLen = 1;
  } else if ((lead & 0xE0U) == 0xC0U && lead >= 0xC2U) {
    seqLen = 2;
  } else if ((lead & 0xF0U) == 0xE0U) {
    seqLen = 3;
  } else if ((lead & 0xF8U) == 0xF0U && lead <= 0xF4U) {
    seqLen = 4;
  } else {
    buf[i - 1] = '\0';
    return i - 1;
  }
  if (tailBytes < seqLen) {
    buf[i - 1] = '\0';
    return i - 1;
  }
  if (tailBytes > seqLen) {
    const size_t validEnd = (i - 1) + seqLen;
    buf[validEnd] = '\0';
    return validEnd;
  }
  buf[len] = '\0';
  return len;
}

// Parse an 8-byte ANCS Notification Source GATT notification.
inline bool parseNotificationSourcePacket(const uint8_t* data, size_t len, NotificationSourceEvent& out) {
  if (!data || len < 8 || data[0] > static_cast<uint8_t>(EventId::Removed)) {
    return false;
  }
  out.eventId = static_cast<EventId>(data[0]);
  out.eventFlags = data[1];
  out.categoryId = data[2];
  out.categoryCount = data[3];
  std::memcpy(&out.uid, data + 4, sizeof(uint32_t));
  return true;
}

// Whether a Notification Source event should trigger a GetNotificationAttributes request.
// Silent notifications (Added or Modified) are never shown, and Removed events carry no attributes.
inline bool shouldFetchAttributes(const NotificationSourceEvent& ev) {
  if (ev.eventId == EventId::Removed) return false;
  return (ev.eventFlags & kEventFlagSilent) == 0;
}

// Whether a Data Source packet can be the first packet of the GetNotificationAttributes response for
// `expectedUid`: CommandID 0 followed by the little-endian UID. A packet shorter than the 5-byte header
// is accepted if its bytes match the header prefix. Used to drop late fragments of a timed-out
// response instead of parsing them as the start of the next one.
inline bool isAttributeResponseStart(const uint8_t* data, size_t len, uint32_t expectedUid) {
  if (!data || len == 0) return false;
  uint8_t header[5];
  header[0] = static_cast<uint8_t>(CommandId::GetNotificationAttributes);
  std::memcpy(header + 1, &expectedUid, sizeof(uint32_t));
  return std::memcmp(data, header, std::min<size_t>(len, sizeof(header))) == 0;
}

// Build the 13-byte GetNotificationAttributes Control Point request for:
// AppIdentifier (0x00), Title (0x01, maxLen), Message (0x03, maxLen), Date (0x05).
inline constexpr size_t kGetAttributesRequestSize = 13;

inline size_t buildGetNotificationAttributesPacket(uint32_t uid, uint8_t out[kGetAttributesRequestSize]) {
  out[0] = static_cast<uint8_t>(CommandId::GetNotificationAttributes);
  std::memcpy(out + 1, &uid, sizeof(uint32_t));
  out[5] = static_cast<uint8_t>(AttributeId::AppIdentifier);
  out[6] = static_cast<uint8_t>(AttributeId::Title);
  const uint16_t titleMax = static_cast<uint16_t>(AncsNotification::kTitleMax - 1);
  std::memcpy(out + 7, &titleMax, sizeof(uint16_t));
  out[9] = static_cast<uint8_t>(AttributeId::Message);
  const uint16_t msgMax = static_cast<uint16_t>(AncsNotification::kMessageMax - 1);
  std::memcpy(out + 10, &msgMax, sizeof(uint16_t));
  out[12] = static_cast<uint8_t>(AttributeId::Date);
  return kGetAttributesRequestSize;
}

inline constexpr size_t kPerformActionRequestSize = 6;

inline size_t buildPerformActionPacket(uint32_t uid, ActionId action, uint8_t out[kPerformActionRequestSize]) {
  out[0] = static_cast<uint8_t>(CommandId::PerformNotificationAction);
  std::memcpy(out + 1, &uid, sizeof(uint32_t));
  out[5] = static_cast<uint8_t>(action);
  return kPerformActionRequestSize;
}

// GetAppAttributes Control Point request for the app's DisplayName:
// CommandID (1) + NUL-terminated AppIdentifier + AppAttributeID DisplayName (0).
inline constexpr size_t kGetAppAttributesRequestMax = 1 + AncsNotification::kAppIdMax + 1;

// Returns the packet length, or 0 when `appId` is empty or does not fit.
inline size_t buildGetAppAttributesPacket(const char* appId, uint8_t out[kGetAppAttributesRequestMax]) {
  if (!appId || appId[0] == '\0') return 0;
  const size_t idLen = std::strlen(appId);
  if (idLen >= AncsNotification::kAppIdMax) return 0;
  out[0] = static_cast<uint8_t>(CommandId::GetAppAttributes);
  std::memcpy(out + 1, appId, idLen + 1);  // includes the NUL terminator
  out[2 + idLen] = static_cast<uint8_t>(AppAttributeId::DisplayName);
  return 3 + idLen;
}

// Whether a Data Source packet can be the first packet of the GetAppAttributes response for
// `expectedAppId`: CommandID 1 followed by the NUL-terminated app identifier. A packet that ends
// inside that header is accepted if its bytes match the header prefix.
inline bool isAppAttributeResponseStart(const uint8_t* data, size_t len, const char* expectedAppId) {
  if (!data || len == 0 || !expectedAppId) return false;
  if (data[0] != static_cast<uint8_t>(CommandId::GetAppAttributes)) return false;
  const size_t headerLen = std::strlen(expectedAppId) + 1;  // app id + NUL
  const size_t cmpLen = std::min(len - 1, headerLen);
  return std::memcmp(data + 1, expectedAppId, cmpLen) == 0;
}

// Short names for common iOS bundle identifiers. Used only while iOS has not reported the app's
// DisplayName (request pending, timed out, or rejected).
inline const char* friendlyAppName(const char* appId) {
  if (!appId || appId[0] == '\0') return "iOS";
  if (std::strstr(appId, "tencent.xin")) return "WeChat";
  if (std::strstr(appId, "MobileSMS")) return "Messages";
  if (std::strstr(appId, "mobilephone")) return "Phone";
  if (std::strstr(appId, "mobilemail")) return "Mail";
  if (std::strstr(appId, "mobilecal")) return "Calendar";
  if (std::strstr(appId, "reminders")) return "Reminders";
  if (std::strstr(appId, "telegra")) return "Telegram";
  if (std::strstr(appId, "whatsapp")) return "WhatsApp";
  if (std::strstr(appId, "slack")) return "Slack";
  if (std::strstr(appId, "dingtalk")) return "DingTalk";
  if (std::strstr(appId, "lark") || std::strstr(appId, "feishu")) return "Feishu";
  const char* lastDot = std::strrchr(appId, '.');
  return (lastDot && lastDot[1] != '\0') ? (lastDot + 1) : appId;
}

// Name to show for a notification: the DisplayName iOS reported for the app, falling back to
// friendlyAppName() until (or unless) that answer arrives.
inline const char* displayAppName(const AncsNotification& item) {
  if (item.appName[0] != '\0') return item.appName;
  return friendlyAppName(item.appId);
}

// Fixed-capacity appId -> DisplayName cache, replacing the oldest entry when full. An empty
// name is cached too, so an app iOS reports no name for is not asked about again. Entries put
// with `provisional = true` (e.g. a timed-out request) are dropped by clearProvisional(), so
// the app is asked again on the next connection.
// Not thread-safe: callers serialize access.
template <size_t N>
class AppNameCache {
 public:
  static_assert(N > 0, "AppNameCache needs capacity");

  // Returns the cached name (possibly empty), or nullptr when `appId` is unknown.
  const char* find(const char* appId) const {
    if (!appId || appId[0] == '\0') return nullptr;
    for (size_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].appId, appId) == 0) return entries_[i].name;
    }
    return nullptr;
  }

  void put(const char* appId, const char* name, bool provisional = false) {
    if (!appId || appId[0] == '\0' || std::strlen(appId) >= AncsNotification::kAppIdMax) return;
    Entry* slot = nullptr;
    for (size_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].appId, appId) == 0) slot = &entries_[i];
    }
    if (!slot) {
      if (count_ < N) {
        slot = &entries_[count_++];
      } else {
        slot = &entries_[next_];
        next_ = (next_ + 1) % N;
      }
      std::strcpy(slot->appId, appId);
    }
    const size_t n = name ? std::min(std::strlen(name), AncsNotification::kAppNameMax - 1) : 0;
    if (n > 0) std::memcpy(slot->name, name, n);
    slot->name[n] = '\0';
    utf8SafeTruncate(slot->name, n);
    slot->provisional = provisional;
  }

  // Drop every provisional entry, keeping the others in insertion order.
  void clearProvisional() {
    size_t w = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (!entries_[i].provisional) {
        if (w != i) entries_[w] = entries_[i];
        ++w;
      }
    }
    for (size_t i = w; i < count_; ++i) entries_[i] = Entry{};
    count_ = w;
    next_ = (count_ == N) ? next_ : 0;
  }

  size_t size() const { return count_; }

 private:
  struct Entry {
    char appId[AncsNotification::kAppIdMax] = {0};
    char name[AncsNotification::kAppNameMax] = {0};
    bool provisional = false;
  };
  std::array<Entry, N> entries_{};
  size_t count_ = 0;
  size_t next_ = 0;
};

// Byte-stream parser for the Data Source response to GetAppAttributes(DisplayName):
// CommandID (1) + NUL-terminated AppIdentifier + {AttributeID, u16 length, value}.
// Reassembles fragments across MTU boundaries; the value is truncated to kAppNameMax - 1 bytes
// at a UTF-8 boundary.
class AncsAppAttributeParser {
 public:
  using CompleteCallback = void (*)(const char* appId, const char* displayName, void* ctx);

  void reset() {
    phase_ = Phase::CommandId;
    idLen_ = 0;
    hdrPos_ = 0;
    remaining_ = 0;
    nameLen_ = 0;
    appId_[0] = '\0';
    name_[0] = '\0';
  }

  bool atStart() const { return phase_ == Phase::CommandId; }

  void feed(const uint8_t* data, size_t len, CompleteCallback onComplete, void* ctx) {
    size_t idx = 0;
    while (idx < len) {
      switch (phase_) {
        case Phase::CommandId:
          if (data[idx++] != static_cast<uint8_t>(CommandId::GetAppAttributes)) {
            reset();
            return;
          }
          phase_ = Phase::AppId;
          break;
        case Phase::AppId: {
          const uint8_t b = data[idx++];
          if (b == 0) {
            appId_[idLen_] = '\0';
            phase_ = Phase::AttrHeader;
          } else if (idLen_ + 1 >= sizeof(appId_)) {
            reset();
            return;
          } else {
            appId_[idLen_++] = static_cast<char>(b);
          }
          break;
        }
        case Phase::AttrHeader:
          hdr_[hdrPos_++] = data[idx++];
          if (hdrPos_ == 3) {
            hdrPos_ = 0;
            if (hdr_[0] != static_cast<uint8_t>(AppAttributeId::DisplayName)) {
              reset();
              return;
            }
            std::memcpy(&remaining_, hdr_ + 1, sizeof(uint16_t));
            nameLen_ = 0;
            name_[0] = '\0';
            if (remaining_ == 0) {
              finish(onComplete, ctx);
            } else {
              phase_ = Phase::AttrPayload;
            }
          }
          break;
        case Phase::AttrPayload: {
          const size_t consume = std::min<size_t>(len - idx, remaining_);
          const size_t room = (sizeof(name_) - 1) - nameLen_;
          const size_t copyLen = std::min(consume, room);
          std::memcpy(name_ + nameLen_, data + idx, copyLen);
          nameLen_ += copyLen;
          name_[nameLen_] = '\0';
          idx += consume;
          remaining_ = static_cast<uint16_t>(remaining_ - consume);
          if (remaining_ == 0) {
            utf8SafeTruncate(name_, nameLen_);
            finish(onComplete, ctx);
          }
          break;
        }
      }
    }
  }

 private:
  enum class Phase : uint8_t { CommandId, AppId, AttrHeader, AttrPayload };

  void finish(CompleteCallback onComplete, void* ctx) {
    if (onComplete) onComplete(appId_, name_, ctx);
    reset();
  }

  Phase phase_ = Phase::CommandId;
  char appId_[AncsNotification::kAppIdMax] = {0};
  size_t idLen_ = 0;
  uint8_t hdr_[3] = {0};
  uint8_t hdrPos_ = 0;
  uint16_t remaining_ = 0;
  char name_[AncsNotification::kAppNameMax] = {0};
  size_t nameLen_ = 0;
};

// Extract "HH:MM" from an ANCS ISO-8601 date string "yyyyMMdd'T'HHmmss".
inline void formatAncsTime(const char* rawDate, char out[8]) {
  if (!rawDate) {
    out[0] = '\0';
    return;
  }
  const char* tPos = std::strchr(rawDate, 'T');
  if (tPos && std::strlen(tPos) >= 5) {
    out[0] = tPos[1];
    out[1] = tPos[2];
    out[2] = ':';
    out[3] = tPos[3];
    out[4] = tPos[4];
    out[5] = '\0';
    return;
  }
  out[0] = '\0';
}

// Zero-allocation stateful byte-stream parser for ANCS Data Source notifications.
// Reassembles fragmented GATT notifications across arbitrary MTU boundaries.
class AncsDataSourceParser {
 public:
  static constexpr uint8_t kExpectedAttributeCount = 4;
  static constexpr uint16_t kMaxWireAttributeLen = 1024;

  enum class ParsePhase : uint8_t {
    CommandId,
    NotificationUid,
    AttrHeader,
    AttrPayload,
  };

  using CompleteCallback = void (*)(const AncsNotification& item, void* ctx);

  void reset() {
    phase_ = ParsePhase::CommandId;
    hdrPos_ = 0;
    completedAttrs_ = 0;
    currentAttrId_ = 0;
    attrBytesRemaining_ = 0;
    dstOffset_ = 0;
    staging_.clear();
  }

  ParsePhase phase() const { return phase_; }

  void feed(const uint8_t* data, size_t len, CompleteCallback onComplete, void* ctx) {
    size_t idx = 0;
    while (idx < len) {
      switch (phase_) {
        case ParsePhase::CommandId: {
          const uint8_t cmd = data[idx++];
          if (cmd != static_cast<uint8_t>(CommandId::GetNotificationAttributes)) {
            reset();
            return;
          }
          staging_.clear();
          hdrPos_ = 0;
          completedAttrs_ = 0;
          phase_ = ParsePhase::NotificationUid;
          break;
        }
        case ParsePhase::NotificationUid: {
          while (idx < len && hdrPos_ < sizeof(uint32_t)) {
            hdrBuf_[hdrPos_++] = data[idx++];
          }
          if (hdrPos_ == sizeof(uint32_t)) {
            std::memcpy(&staging_.uid, hdrBuf_, sizeof(uint32_t));
            hdrPos_ = 0;
            phase_ = ParsePhase::AttrHeader;
          }
          break;
        }
        case ParsePhase::AttrHeader: {
          while (idx < len && hdrPos_ < 3) {
            hdrBuf_[hdrPos_++] = data[idx++];
          }
          if (hdrPos_ == 3) {
            currentAttrId_ = hdrBuf_[0];
            uint16_t attrLen = 0;
            std::memcpy(&attrLen, hdrBuf_ + 1, sizeof(uint16_t));
            if (currentAttrId_ > static_cast<uint8_t>(AttributeId::NegativeActionLabel) ||
                attrLen > kMaxWireAttributeLen) {
              reset();
              return;
            }
            attrBytesRemaining_ = attrLen;
            dstOffset_ = 0;
            hdrPos_ = 0;
            char* dst = nullptr;
            size_t cap = 0;
            resolveTargetBuffer(currentAttrId_, &dst, &cap);
            if (dst && cap > 0) {
              dst[0] = '\0';
            }
            if (attrBytesRemaining_ == 0) {
              finishCurrentAttribute(onComplete, ctx);
            } else {
              phase_ = ParsePhase::AttrPayload;
            }
          }
          break;
        }
        case ParsePhase::AttrPayload: {
          const size_t avail = len - idx;
          const size_t consume = std::min<size_t>(avail, attrBytesRemaining_);
          char* dst = nullptr;
          size_t cap = 0;
          resolveTargetBuffer(currentAttrId_, &dst, &cap);
          if (dst && cap > 1 && dstOffset_ < cap - 1) {
            const size_t copyLen = std::min<size_t>(consume, (cap - 1) - dstOffset_);
            std::memcpy(dst + dstOffset_, data + idx, copyLen);
            dstOffset_ += copyLen;
            dst[dstOffset_] = '\0';
          }
          idx += consume;
          attrBytesRemaining_ = static_cast<uint16_t>(attrBytesRemaining_ - consume);
          if (attrBytesRemaining_ == 0) {
            if (dst && cap > 1) {
              utf8SafeTruncate(dst, dstOffset_);
            }
            finishCurrentAttribute(onComplete, ctx);
          }
          break;
        }
      }
    }
  }

 private:
  void resolveTargetBuffer(uint8_t attrId, char** outBuf, size_t* outCap) {
    if (attrId == static_cast<uint8_t>(AttributeId::AppIdentifier)) {
      *outBuf = staging_.appId;
      *outCap = sizeof(staging_.appId);
    } else if (attrId == static_cast<uint8_t>(AttributeId::Title)) {
      *outBuf = staging_.title;
      *outCap = sizeof(staging_.title);
    } else if (attrId == static_cast<uint8_t>(AttributeId::Message)) {
      *outBuf = staging_.message;
      *outCap = sizeof(staging_.message);
    } else if (attrId == static_cast<uint8_t>(AttributeId::Date)) {
      *outBuf = staging_.date;
      *outCap = sizeof(staging_.date);
    } else {
      *outBuf = nullptr;
      *outCap = 0;
    }
  }

  void finishCurrentAttribute(CompleteCallback onComplete, void* ctx) {
    ++completedAttrs_;
    if (completedAttrs_ >= kExpectedAttributeCount) {
      staging_.valid = true;
      if (onComplete) {
        onComplete(staging_, ctx);
      }
      reset();
    } else {
      phase_ = ParsePhase::AttrHeader;
    }
  }

  ParsePhase phase_ = ParsePhase::CommandId;
  uint8_t hdrBuf_[4] = {0};
  uint8_t hdrPos_ = 0;
  uint8_t completedAttrs_ = 0;
  uint8_t currentAttrId_ = 0;
  uint16_t attrBytesRemaining_ = 0;
  size_t dstOffset_ = 0;
  AncsNotification staging_{};
};

}  // namespace ancs

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Utf8.h"
#include "network/AncsNotificationList.h"
#include "network/AncsProtocol.h"
#include "network/AppleBleParams.h"
#include "network/FirmwareBoardTag.h"
#include "network/FirmwareImageVerify.h"
#include "network/MagSafeBadgeStore.h"
#include "network/OtaSeq.h"
#include "util/PowerPolicy.h"
#include "util/QrLayout.h"
#include "qrcodegen.h"

namespace {

void appendAttr(std::vector<uint8_t>& pkt, ancs::AttributeId id, const std::string& val) {
  pkt.push_back(static_cast<uint8_t>(id));
  const uint16_t len = static_cast<uint16_t>(val.size());
  pkt.push_back(static_cast<uint8_t>(len & 0xFF));
  pkt.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
  pkt.insert(pkt.end(), val.begin(), val.end());
}

struct VectorReadCtx {
  const std::vector<uint8_t>* data;
  size_t pos = 0;
};

bool readFromVector(void* ctx, uint8_t* buf, size_t len) {
  auto* vctx = static_cast<VectorReadCtx*>(ctx);
  if (vctx->pos + len > vctx->data->size()) return false;
  std::memcpy(buf, vctx->data->data() + vctx->pos, len);
  vctx->pos += len;
  return true;
}

std::vector<uint8_t> buildSyntheticEspImage(uint16_t chipId, const std::string& embeddedTag, bool appendSha = true,
                                            size_t segPayloadLen = 65536) {
  std::vector<uint8_t> img(24, 0);
  img[0] = 0xE9;  // magic
  img[1] = 1;     // 1 segment
  const uint32_t entry = 0x40380000u;
  std::memcpy(&img[4], &entry, sizeof(entry));
  std::memcpy(&img[12], &chipId, sizeof(chipId));
  img[23] = appendSha ? 1 : 0;

  uint8_t segHdr[8] = {};
  const uint32_t loadAddr = 0x3C000020u;
  const uint32_t dataLen = static_cast<uint32_t>(segPayloadLen);
  std::memcpy(segHdr, &loadAddr, 4);
  std::memcpy(segHdr + 4, &dataLen, 4);
  img.insert(img.end(), segHdr, segHdr + 8);

  std::vector<uint8_t> payload(segPayloadLen, 0x5Au);
  if (!embeddedTag.empty() && embeddedTag.size() < payload.size()) {
    std::memcpy(payload.data() + 128, embeddedTag.data(), embeddedTag.size());
  }

  uint8_t xorAcc = 0xEFu;
  for (uint8_t b : payload) {
    xorAcc ^= b;
  }
  img.insert(img.end(), payload.begin(), payload.end());

  const size_t padEnd = (img.size() + 16) & ~static_cast<size_t>(15);
  const size_t padLen = padEnd - img.size();
  img.insert(img.end(), padLen, 0x00u);
  img.back() = xorAcc;

  if (appendSha) {
    uint8_t digest[32] = {};
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, img.data(), img.size());
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    img.insert(img.end(), digest, digest + 32);
  }
  return img;
}

}  // namespace

TEST(AncsProtocolTest, Parses8ByteNotificationSourcePacket) {
  const uint8_t raw[8] = {
      0x00,                    // EventId::Added
      0x18,                    // Positive + Negative Action flags
      0x04,                    // Category Social
      0x02,                    // CategoryCount
      0x78, 0x56, 0x34, 0x12,  // UID = 0x12345678
  };
  ancs::NotificationSourceEvent ev{};
  ASSERT_TRUE(ancs::parseNotificationSourcePacket(raw, sizeof(raw), ev));
  EXPECT_EQ(ev.eventId, ancs::EventId::Added);
  EXPECT_EQ(ev.eventFlags, 0x18);
  EXPECT_EQ(ev.categoryId, 4);
  EXPECT_EQ(ev.categoryCount, 2);
  EXPECT_EQ(ev.uid, 0x12345678U);

  const uint8_t bad[8] = {0x05, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(ancs::parseNotificationSourcePacket(bad, sizeof(bad), ev));
  EXPECT_FALSE(ancs::parseNotificationSourcePacket(raw, 7, ev));
}

TEST(AncsProtocolTest, BuildsGetNotificationAttributesAndPerformActionRequests) {
  uint8_t pkt[ancs::kGetAttributesRequestSize] = {};
  const size_t len = ancs::buildGetNotificationAttributesPacket(0xAABBCCDDU, pkt);
  ASSERT_EQ(len, 13U);
  EXPECT_EQ(pkt[0], static_cast<uint8_t>(ancs::CommandId::GetNotificationAttributes));
  uint32_t uid = 0;
  std::memcpy(&uid, pkt + 1, sizeof(uint32_t));
  EXPECT_EQ(uid, 0xAABBCCDDU);
  EXPECT_EQ(pkt[5], static_cast<uint8_t>(ancs::AttributeId::AppIdentifier));
  EXPECT_EQ(pkt[6], static_cast<uint8_t>(ancs::AttributeId::Title));
  EXPECT_EQ(pkt[9], static_cast<uint8_t>(ancs::AttributeId::Message));
  EXPECT_EQ(pkt[12], static_cast<uint8_t>(ancs::AttributeId::Date));

  uint8_t actPkt[ancs::kPerformActionRequestSize] = {};
  ASSERT_EQ(ancs::buildPerformActionPacket(0x11223344U, ancs::ActionId::Positive, actPkt), 6U);
  EXPECT_EQ(actPkt[0], static_cast<uint8_t>(ancs::CommandId::PerformNotificationAction));
  std::memcpy(&uid, actPkt + 1, sizeof(uint32_t));
  EXPECT_EQ(uid, 0x11223344U);
  EXPECT_EQ(actPkt[5], static_cast<uint8_t>(ancs::ActionId::Positive));
}

TEST(AncsProtocolTest, Utf8SafeTruncateHandlesBoundaryAndStrayContinuationBytes) {
  // 1. Buffer containing only stray continuation bytes (i == 0).
  char strayOnly[8] = {'\x80', '\xBF', '\x80', '\0'};
  EXPECT_EQ(ancs::utf8SafeTruncate(strayOnly, 3), 0U);
  EXPECT_STREQ(strayOnly, "");
  EXPECT_EQ(utf8SafeTruncateBuffer("\x80\xBF", 2), 0);

  // 2. Buffer starting with an incomplete 3-byte sequence at index 0.
  char incompleteAtZero[4] = {'\xE4', '\xB8', '\0'};
  EXPECT_EQ(ancs::utf8SafeTruncate(incompleteAtZero, 2), 0U);
  EXPECT_STREQ(incompleteAtZero, "");
  EXPECT_EQ(utf8SafeTruncateBuffer("\xE4\xB8", 2), 0);

  // 3. Valid 3-byte CJK character followed by 4 stray continuation bytes (len - i == 4).
  char fourStrayTail[16] = {'a', '\xE4', '\xB8', '\xAD', '\x80', '\x80', '\x80', '\x80', '\0'};
  EXPECT_EQ(ancs::utf8SafeTruncate(fourStrayTail, 8), 4U);
  EXPECT_STREQ(fourStrayTail, "a\xE4\xB8\xAD");

  // 4. Incomplete 4-byte emoji split at tail.
  char splitEmoji[8] = {'O', 'K', '\xF0', '\x9F', '\x98', '\0'};
  EXPECT_EQ(ancs::utf8SafeTruncate(splitEmoji, 5), 2U);
  EXPECT_STREQ(splitEmoji, "OK");
}

TEST(AncsProtocolTest, ReassemblesFragmentedDataSourceStreamAndTruncatesUtf8Safely) {
  std::vector<uint8_t> stream;
  stream.push_back(0x00);
  const uint32_t uid = 42;
  stream.push_back(static_cast<uint8_t>(uid & 0xFF));
  stream.push_back(0);
  stream.push_back(0);
  stream.push_back(0);

  const std::string longAppId = "com.example.enterprise.super.long.bundle.identifier.tencent.xin";
  appendAttr(stream, ancs::AttributeId::AppIdentifier, longAppId);
  appendAttr(stream, ancs::AttributeId::Title, "微信消息");

  std::string longMsg;
  for (int i = 0; i < 80; ++i) {
    longMsg += "墨水屏";
  }
  appendAttr(stream, ancs::AttributeId::Message, longMsg);
  appendAttr(stream, ancs::AttributeId::Date, "20261105T233512");

  ancs::AncsDataSourceParser parser;
  ancs::AncsNotification completed{};
  bool done = false;

  auto cb = +[](const ancs::AncsNotification& item, void* ctx) {
    auto* out = static_cast<ancs::AncsNotification*>(ctx);
    *out = item;
  };

  for (size_t i = 0; i < stream.size(); ++i) {
    parser.feed(&stream[i], 1, cb, &completed);
    if (completed.valid) {
      done = true;
    }
  }

  ASSERT_TRUE(done);
  EXPECT_EQ(completed.uid, 42U);
  EXPECT_STREQ(completed.title, "微信消息");
  EXPECT_STREQ(completed.date, "20261105T233512");

  char formattedTime[8];
  ancs::formatAncsTime(completed.date, formattedTime);
  EXPECT_STREQ(formattedTime, "23:35");

  const size_t msgLen = std::strlen(completed.message);
  EXPECT_GT(msgLen, 0U);
  EXPECT_LT(msgLen, ancs::AncsNotification::kMessageMax);
  EXPECT_EQ(msgLen % 3U, 0U);
}

TEST(AncsProtocolTest, RejectsMalformedCommandAndOversizedAttributeHeaders) {
  ancs::AncsDataSourceParser parser;
  ancs::AncsNotification completed{};
  auto cb = +[](const ancs::AncsNotification& item, void* ctx) {
    *static_cast<ancs::AncsNotification*>(ctx) = item;
  };

  // Packet starting with non-zero command byte must be discarded without hunting for an embedded 0x00.
  const uint8_t bogusPacket[] = {0x99, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 'H', 'i'};
  parser.feed(bogusPacket, sizeof(bogusPacket), cb, &completed);
  EXPECT_EQ(parser.phase(), ancs::AncsDataSourceParser::ParsePhase::CommandId);
  EXPECT_FALSE(completed.valid);

  // Packet with invalid AttributeId (0xFE) or attrLen > kMaxWireAttributeLen resets cleanly.
  const uint8_t badAttrPacket[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0xFE, 0x10, 0x00};
  parser.feed(badAttrPacket, sizeof(badAttrPacket), cb, &completed);
  EXPECT_EQ(parser.phase(), ancs::AncsDataSourceParser::ParsePhase::CommandId);
}

TEST(AncsProtocolTest, HandlesZeroLengthAttributesCleanly) {
  std::vector<uint8_t> stream = {0x00, 0x07, 0x00, 0x00, 0x00};
  appendAttr(stream, ancs::AttributeId::AppIdentifier, "com.apple.MobileSMS");
  appendAttr(stream, ancs::AttributeId::Title, "");
  appendAttr(stream, ancs::AttributeId::Message, "Verification code: 849201");
  appendAttr(stream, ancs::AttributeId::Date, "");

  ancs::AncsDataSourceParser parser;
  ancs::AncsNotification completed{};
  parser.feed(
      stream.data(), stream.size(),
      +[](const ancs::AncsNotification& item, void* ctx) { *static_cast<ancs::AncsNotification*>(ctx) = item; },
      &completed);

  ASSERT_TRUE(completed.valid);
  EXPECT_EQ(completed.uid, 7U);
  EXPECT_STREQ(ancs::friendlyAppName(completed.appId), "Messages");
  EXPECT_STREQ(completed.title, "");
  EXPECT_STREQ(completed.message, "Verification code: 849201");
}

TEST(MagSafeBadgeStoreTest, Parses2To5FieldPipeDelimitedCardsAndDefaults) {
  const char* sample =
      "# Comment line\n"
      "BOARDING PASS | MU5101 PEK->SHA | GATE A08 SEAT 16K | BOARD 08:30 | M1TEST/PASSENGER\n"
      "DIGITAL CARD | XTEINK X4 | Zero-Power Companion | https://crosspointreader.com\n"
      "EMERGENCY ICE | IF FOUND CALL | tel:+8613800000000\n"
      "Quick WiFi | WIFI:S:Guest;T:WPA;P:secret;;\n";

  std::array<magsafe_badge::BadgeCard, 6> cards{};
  const size_t n = magsafe_badge::parseCardsText(sample, cards);
  ASSERT_EQ(n, 4U);

  EXPECT_STREQ(cards[0].tag, "BOARDING PASS");
  EXPECT_STREQ(cards[0].title, "MU5101 PEK->SHA");
  EXPECT_STREQ(cards[0].subtitle, "GATE A08 SEAT 16K");
  EXPECT_STREQ(cards[0].meta, "BOARD 08:30");
  EXPECT_STREQ(cards[0].qrPayload, "M1TEST/PASSENGER");

  EXPECT_STREQ(cards[1].tag, "DIGITAL CARD");
  EXPECT_STREQ(cards[1].title, "XTEINK X4");
  EXPECT_STREQ(cards[1].subtitle, "Zero-Power Companion");
  EXPECT_STREQ(cards[1].meta, "");
  EXPECT_STREQ(cards[1].qrPayload, "https://crosspointreader.com");

  EXPECT_STREQ(cards[2].tag, "EMERGENCY ICE");
  EXPECT_STREQ(cards[2].title, "IF FOUND CALL");
  EXPECT_STREQ(cards[2].qrPayload, "tel:+8613800000000");

  EXPECT_STREQ(cards[3].tag, "MAGSAFE VAULT");
  EXPECT_STREQ(cards[3].title, "Quick WiFi");
  EXPECT_STREQ(cards[3].qrPayload, "WIFI:S:Guest;T:WPA;P:secret;;");

  magsafe_badge::BadgeCard def0{}, def1{};
  magsafe_badge::populateDefaultCards(def0, def1);
  EXPECT_STREQ(def0.tag, "DIGITAL BADGE");
  EXPECT_STREQ(def1.tag, "BOARDING PASS");
}

TEST(FirmwareBoardTagTest, ScannerDetectsMatchingMismatchedAndFalsePrefixTags) {
  EXPECT_EQ(std::string(board_tag::boardName(), board_tag::boardNameLen()), "x4");

  // 1. Matching tag split byte-by-byte across chunks.
  board_tag::Scanner okScanner;
  const std::string validStream = "padding...CROSSPOINT-BOARD-V1:x4;...tail";
  for (char c : validStream) {
    const uint8_t b = static_cast<uint8_t>(c);
    okScanner.feed(&b, 1);
  }
  EXPECT_FALSE(okScanner.mismatch());

  // 2. False prefix collision aborted by non-printable byte '\x01' followed immediately by real mismatch tag.
  board_tag::Scanner collisionScanner;
  const std::string tricky = "CROSSPOINT-BOARD-V1:bad\x01" "CROSSPOINT-BOARD-V1:x4pro;";
  collisionScanner.feed(reinterpret_cast<const uint8_t*>(tricky.data()), tricky.size());
  EXPECT_TRUE(collisionScanner.mismatch());
  EXPECT_STREQ(collisionScanner.foundName(), "x4pro");
}

TEST(FirmwareImageVerifyTest, ValidatesHeaderChecksumSha256AndBoardTag) {
  constexpr uint16_t kEsp32C3ChipId = 0x0005;

  // 1. Valid image with matching board tag and SHA-256 trailer.
  const auto validImg = buildSyntheticEspImage(kEsp32C3ChipId, "CROSSPOINT-BOARD-V1:x4;", /*appendSha=*/true);
  VectorReadCtx ctx{&validImg, 0};
  board_tag::Scanner scanner;
  EXPECT_EQ(firmware_flash::detail::validateImageStream(readFromVector, &ctx, validImg.size(), /*exactSize=*/true,
                                                        kEsp32C3ChipId, &scanner),
            firmware_flash::Result::OK);

  // 2. Wrong chip ID rejected.
  ctx.pos = 0;
  EXPECT_EQ(firmware_flash::detail::validateImageStream(readFromVector, &ctx, validImg.size(), /*exactSize=*/true,
                                                        /*expectedChipId=*/0x0009, nullptr),
            firmware_flash::Result::BAD_CHIP);

  // 3. Mismatched board tag rejected.
  const auto wrongBoardImg = buildSyntheticEspImage(kEsp32C3ChipId, "CROSSPOINT-BOARD-V1:x4pro;", /*appendSha=*/true);
  VectorReadCtx wbCtx{&wrongBoardImg, 0};
  board_tag::Scanner wbScanner;
  EXPECT_EQ(firmware_flash::detail::validateImageStream(readFromVector, &wbCtx, wrongBoardImg.size(),
                                                        /*exactSize=*/true, kEsp32C3ChipId, &wbScanner),
            firmware_flash::Result::WRONG_BOARD);

  // 4. Corrupted SHA-256 trailer rejected.
  auto badShaImg = validImg;
  badShaImg.back() ^= 0xFFu;
  VectorReadCtx shaCtx{&badShaImg, 0};
  EXPECT_EQ(firmware_flash::detail::validateImageStream(readFromVector, &shaCtx, badShaImg.size(), /*exactSize=*/true,
                                                        kEsp32C3ChipId, nullptr),
            firmware_flash::Result::BAD_SHA);

  // 5. Corrupted XOR checksum byte rejected.
  auto badXorImg = buildSyntheticEspImage(kEsp32C3ChipId, "CROSSPOINT-BOARD-V1:x4;", /*appendSha=*/false);
  badXorImg.back() ^= 0x01u;
  VectorReadCtx xorCtx{&badXorImg, 0};
  EXPECT_EQ(firmware_flash::detail::validateImageStream(readFromVector, &xorCtx, badXorImg.size(), /*exactSize=*/true,
                                                        kEsp32C3ChipId, nullptr),
            firmware_flash::Result::BAD_CHECKSUM);

  // 6. Unaligned segment length (not a multiple of 4) rejected.
  auto unalignedImg = buildSyntheticEspImage(kEsp32C3ChipId, "", /*appendSha=*/false, /*segPayloadLen=*/65538);
  VectorReadCtx unalignedCtx{&unalignedImg, 0};
  EXPECT_EQ(firmware_flash::detail::validateImageStream(readFromVector, &unalignedCtx, unalignedImg.size(),
                                                        /*exactSize=*/true, kEsp32C3ChipId, nullptr),
            firmware_flash::Result::BAD_SEGMENTS);
}

TEST(QrCodegenTest, PicksSmallestVersionAndCompactMode) {
  std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX);
  std::vector<uint8_t> tmp(qrcodegen_BUFFER_LEN_MAX);

  // A 60-character uppercase IATA BCBP boarding pass string fits alphanumeric mode.
  // Byte mode at ECC LOW needs version 4 (78 B); alphanumeric fits in version 3.
  const char* bcbp = "M1DESMARAIS/LUC       EABC123 YULFRAAC 0834 326J001A0025 100";
  ASSERT_TRUE(qrcodegen_encodeText(bcbp, tmp.data(), qr.data(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true));
  EXPECT_LE(qrcodegen_getSize(qr.data()), 21 + 4 * 2);  // version <= 3

  // Mixed-case UTF-8 text falls back to byte mode and still encodes.
  const char* cjk = "WIFI:S:\xE5\x92\x96\xE5\x95\xA1;T:WPA;P:secret;;";
  ASSERT_TRUE(qrcodegen_encodeText(cjk, tmp.data(), qr.data(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true));
  EXPECT_GE(qrcodegen_getSize(qr.data()), 21);

  // QrUtils caps the version at 10: 271 bytes of byte-mode text fit, 272 do not,
  // and buffers sized for version 10 are enough.
  std::vector<uint8_t> qr10(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
  std::vector<uint8_t> tmp10(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
  const std::string fits(271, 'x');
  ASSERT_TRUE(qrcodegen_encodeText(fits.c_str(), tmp10.data(), qr10.data(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                   10, qrcodegen_Mask_AUTO, true));
  EXPECT_EQ(qrcodegen_getSize(qr10.data()), 21 + 4 * 9);
  const std::string tooLong(272, 'x');
  EXPECT_FALSE(qrcodegen_encodeText(tooLong.c_str(), tmp10.data(), qr10.data(), qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true));
}

namespace {
ancs::AncsNotification makeNotif(uint32_t uid, const char* title) {
  ancs::AncsNotification n{};
  n.uid = uid;
  n.valid = true;
  std::snprintf(n.title, sizeof(n.title), "%s", title);
  return n;
}
}  // namespace

TEST(AncsNotificationListTest, UpsertsNewestFirstDedupesAndEvictsOldest) {
  ancs::NotificationList<3> list;
  list.upsertFront(makeNotif(1, "a"));
  list.upsertFront(makeNotif(2, "b"));
  list.upsertFront(makeNotif(3, "c"));
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(0).uid, 3u);
  EXPECT_EQ(list.at(2).uid, 1u);

  // Re-announcing an existing UID moves it to the front without duplicating it.
  list.upsertFront(makeNotif(1, "a2"));
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(0).uid, 1u);
  EXPECT_STREQ(list.at(0).title, "a2");
  EXPECT_EQ(list.at(1).uid, 3u);
  EXPECT_EQ(list.at(2).uid, 2u);

  // A fourth UID evicts the oldest entry (uid 2).
  list.upsertFront(makeNotif(4, "d"));
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(0).uid, 4u);
  EXPECT_EQ(list.at(1).uid, 1u);
  EXPECT_EQ(list.at(2).uid, 3u);
}

TEST(AncsNotificationListTest, RemoveAndClearDropStaleEntries) {
  ancs::NotificationList<4> list;
  list.upsertFront(makeNotif(10, "x"));
  list.upsertFront(makeNotif(11, "y"));
  list.upsertFront(makeNotif(12, "z"));

  EXPECT_TRUE(list.remove(11));
  EXPECT_FALSE(list.remove(11));
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list.at(0).uid, 12u);
  EXPECT_EQ(list.at(1).uid, 10u);
  EXPECT_FALSE(list.at(2).valid);

  // Reconnect path: stale UIDs from the previous session must all disappear.
  list.clear();
  EXPECT_EQ(list.size(), 0u);
  EXPECT_FALSE(list.remove(10));
  for (size_t i = 0; i < list.capacity(); ++i) {
    EXPECT_FALSE(list.at(i).valid);
  }
}

TEST(PowerPolicyTest, LowPowerOnlyWhenIdleAndNoBleHandshake) {
  constexpr uint32_t kIdle = 3000;
  EXPECT_FALSE(power_policy::shouldUseLowPower(1000, 0, kIdle, false));
  EXPECT_TRUE(power_policy::shouldUseLowPower(3000, 0, kIdle, false));
  EXPECT_FALSE(power_policy::shouldUseLowPower(10000, 0, kIdle, true));
  // millis() wrap-around: 200 ms after an interaction just before overflow is not idle.
  EXPECT_FALSE(power_policy::shouldUseLowPower(100, 0xFFFFFF9Cu, kIdle, false));
  EXPECT_TRUE(power_policy::shouldUseLowPower(5000, 0xFFFFFF9Cu, kIdle, false));
}

TEST(AncsProtocolTest, SkipsSilentAddedAndModifiedEvents) {
  ancs::NotificationSourceEvent ev{};
  ev.eventId = ancs::EventId::Added;
  ev.eventFlags = 0;
  EXPECT_TRUE(ancs::shouldFetchAttributes(ev));
  ev.eventFlags = ancs::kEventFlagSilent;
  EXPECT_FALSE(ancs::shouldFetchAttributes(ev));
  ev.eventId = ancs::EventId::Modified;
  EXPECT_FALSE(ancs::shouldFetchAttributes(ev));
  ev.eventFlags = ancs::kEventFlagImportant;
  EXPECT_TRUE(ancs::shouldFetchAttributes(ev));
  ev.eventId = ancs::EventId::Removed;
  EXPECT_FALSE(ancs::shouldFetchAttributes(ev));
}

TEST(AncsProtocolTest, DropsLateFragmentOfTimedOutResponse) {
  const uint32_t oldUid = 0x11223344u;
  const uint32_t newUid = 0x55667788u;

  // Full response for the new request.
  std::vector<uint8_t> resp = {0x00};
  for (int i = 0; i < 4; ++i) resp.push_back(static_cast<uint8_t>((newUid >> (8 * i)) & 0xFF));
  appendAttr(resp, ancs::AttributeId::AppIdentifier, "com.apple.MobileSMS");
  appendAttr(resp, ancs::AttributeId::Title, "Alice");
  appendAttr(resp, ancs::AttributeId::Message, "hello");
  appendAttr(resp, ancs::AttributeId::Date, "20261107T101500");

  // A late continuation of the old response whose first byte happens to be 0x00.
  const std::vector<uint8_t> lateFragment = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 'x', 'y'};
  EXPECT_FALSE(ancs::isAttributeResponseStart(lateFragment.data(), lateFragment.size(), newUid));
  EXPECT_TRUE(ancs::isAttributeResponseStart(resp.data(), resp.size(), newUid));
  EXPECT_FALSE(ancs::isAttributeResponseStart(resp.data(), resp.size(), oldUid));
  // A header split across packets is accepted by prefix.
  EXPECT_TRUE(ancs::isAttributeResponseStart(resp.data(), 3, newUid));
  EXPECT_FALSE(ancs::isAttributeResponseStart(nullptr, 0, newUid));

  // Gate exactly like IosAncsService::onDataSourceBytes and check the real response still parses.
  ancs::AncsDataSourceParser parser;
  int completed = 0;
  ancs::AncsNotification got{};
  std::pair<int*, ancs::AncsNotification*> ctxPair{&completed, &got};
  auto feedGated = [&](const std::vector<uint8_t>& pkt) {
    if (parser.phase() == ancs::AncsDataSourceParser::ParsePhase::CommandId &&
        !ancs::isAttributeResponseStart(pkt.data(), pkt.size(), newUid)) {
      return;
    }
    parser.feed(
        pkt.data(), pkt.size(),
        [](const ancs::AncsNotification& item, void* ctx) {
          auto* pair = static_cast<std::pair<int*, ancs::AncsNotification*>*>(ctx);
          ++*pair->first;
          *pair->second = item;
        },
        &ctxPair);
  };
  feedGated(lateFragment);
  feedGated(resp);
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(got.uid, newUid);
  EXPECT_STREQ(got.title, "Alice");
  EXPECT_STREQ(got.message, "hello");
}

TEST(MagSafeBadgeStoreTest, StreamParserHandlesChunksLargeFilesAndOverlongLines) {
  std::string text = "# comment line\r\n";
  text += std::string(3000, '#') + "\n";                     // >2 KB of comments must not reject the file
  text += "A|" + std::string(600, 'x') + "\n";               // overlong line is skipped, not truncated
  for (int i = 0; i < 10; ++i) {
    text += "TAG|Title " + std::to_string(i) + "|payload-" + std::to_string(i) + "\r\n";
  }

  std::array<magsafe_badge::BadgeCard, 8> cards{};
  magsafe_badge::CardsTextStreamParser<8> parser(cards, 0);
  // Feed in awkward 7-byte chunks so lines straddle chunk boundaries.
  size_t fed = 0;
  for (size_t pos = 0; pos < text.size() && !parser.full(); pos += 7) {
    const size_t n = std::min<size_t>(7, text.size() - pos);
    parser.feed(text.data() + pos, n);
    fed = pos + n;
  }
  parser.finish();

  EXPECT_TRUE(parser.full());
  EXPECT_LT(fed, text.size());  // stopped reading once 8 cards were parsed
  ASSERT_EQ(parser.count(), 8u);
  EXPECT_EQ(parser.skippedOverlongLines(), 2u);  // the 3000-byte comment and the 600-byte card
  EXPECT_STREQ(cards[0].title, "Title 0");
  EXPECT_STREQ(cards[0].qrPayload, "payload-0");
  EXPECT_STREQ(cards[7].title, "Title 7");
}

TEST(MagSafeBadgeStoreTest, StreamParserParsesTrailingLineWithoutNewlineAndKeepsExisting) {
  std::array<magsafe_badge::BadgeCard, 4> cards{};
  magsafe_badge::copyTrimmedField(cards[0].title, sizeof(cards[0].title), "existing");
  magsafe_badge::CardsTextStreamParser<4> parser(cards, 1);
  const std::string text = "Only|tel:+100";
  parser.feed(text.data(), text.size());
  EXPECT_EQ(parser.count(), 1u);
  parser.finish();
  ASSERT_EQ(parser.count(), 2u);
  EXPECT_STREQ(cards[0].title, "existing");
  EXPECT_STREQ(cards[1].title, "Only");
  EXPECT_STREQ(cards[1].qrPayload, "tel:+100");
}

TEST(QrLayoutTest, ReservesFourModuleQuietZone) {
  // 57-module (version 10) symbol in a 400x400 box: 65 modules incl. quiet zone -> 6 px modules.
  const auto l = qr_layout::fit(57, 40, 100, 400, 400);
  EXPECT_EQ(l.modulePx, 6);
  EXPECT_EQ(l.outerSide, 65 * 6);
  EXPECT_EQ(l.originX - l.outerX, 4 * 6);
  EXPECT_EQ(l.originY - l.outerY, 4 * 6);
  // Symbol + quiet zone stays inside the box and is centered.
  EXPECT_GE(l.outerX, 40);
  EXPECT_LE(l.outerX + l.outerSide, 440);
  EXPECT_EQ(l.outerX - 40, 440 - (l.outerX + l.outerSide));

  // Too small to fit even 1 px modules plus quiet zone.
  EXPECT_EQ(qr_layout::fit(57, 0, 0, 60, 60).modulePx, 0);
  EXPECT_EQ(qr_layout::fit(0, 0, 0, 100, 100).modulePx, 0);
}

TEST(AppleBleParamsTest, ValidatesQa1931ConnectionRules) {
  // Shipped parameters: 30-60 ms, latency 4, 4 s.
  EXPECT_TRUE(apple_ble::connParamsValid(24, 48, 4, 400));
  // Apple's max-latency option is also legal: 60 ms * 31 = 1.86 s <= 2 s, x3 = 5.58 s < 6 s.
  EXPECT_TRUE(apple_ble::connParamsValid(24, 48, 30, 600));
  EXPECT_TRUE(apple_ble::connParamsValid(12, 12, 0, 200));    // Min == Max == 15 ms is allowed
  EXPECT_FALSE(apple_ble::connParamsValid(16, 48, 4, 400));   // 20 ms min is not a multiple of 15 ms
  EXPECT_FALSE(apple_ble::connParamsValid(24, 30, 4, 400));   // Min + 15 ms > Max
  EXPECT_FALSE(apple_ble::connParamsValid(24, 48, 31, 600));  // latency > 30
  EXPECT_FALSE(apple_ble::connParamsValid(24, 48, 30, 400));  // 1.86 s * 3 >= 4 s timeout
  EXPECT_FALSE(apple_ble::connParamsValid(24, 48, 4, 700));   // timeout > 6 s
  EXPECT_FALSE(apple_ble::connParamsValid(24, 48, 0, 150));   // timeout < 2 s
}

TEST(AppleBleParamsTest, AdvertisingIntervalsMatchAppleList) {
  EXPECT_EQ(apple_ble::kFastAdvIntervalUnits * 625, 20000);  // 20 ms in microseconds
  bool found = false;
  for (const auto units : apple_ble::kAppleSlowAdvIntervalsUnits) {
    found |= (units == apple_ble::kSlowAdvIntervalUnits);
  }
  EXPECT_TRUE(found);
  EXPECT_EQ(apple_ble::kSlowAdvIntervalUnits * 625, 1022500);  // 1022.5 ms exactly
  // Every listed Apple interval converts exactly to 0.625 ms units.
  const uint32_t appleUs[] = {152500, 211250, 318750, 417500, 546250, 760000, 852500, 1022500, 1285000};
  for (size_t i = 0; i < 9; ++i) {
    EXPECT_EQ(apple_ble::kAppleSlowAdvIntervalsUnits[i] * 625u, appleUs[i]);
  }
}

TEST(OtaSeqTest, PicksSlotAndWrapsAboveOtherSlot) {
  // (seq - 1) % 2 selects the OTA slot, and the new seq must beat the active one.
  auto p = ota_boot::planNextSeq(5, 1);  // active seq 5 -> ota_0; switch to ota_1
  EXPECT_EQ(p.seq, 6u);
  EXPECT_FALSE(p.wrapped);
  p = ota_boot::planNextSeq(5, 0);
  EXPECT_EQ(p.seq, 7u);
  EXPECT_EQ((p.seq - 1) % 2, 0u);
  p = ota_boot::planNextSeq(0, 0);  // fresh otadata
  EXPECT_EQ(p.seq, 1u);

  // Near UINT32_MAX the seq wraps; the caller must then erase the other sector (wrapped == true),
  // because a small seq alone would lose to the old entry.
  p = ota_boot::planNextSeq(0xFFFFFFFCu, 1);
  EXPECT_TRUE(p.wrapped);
  EXPECT_EQ(p.seq, 2u);
  EXPECT_LT(p.seq, 0xFFFFFFFCu);
  p = ota_boot::planNextSeq(0xFFFFFFFBu, 0);
  EXPECT_FALSE(p.wrapped);
  EXPECT_EQ(p.seq, 0xFFFFFFFDu);  // never 0xFFFFFFFF (erased flash)
  EXPECT_EQ((p.seq - 1) % 2, 0u);
}

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Bitmap.h>
#include <BoardConfig.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <builtinFonts/all.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

#include "fontIds.h"
#include "network/AncsProtocol.h"
#include "network/IosAncsService.h"
#include "network/MagSafeBadgeStore.h"
#include "network/OtaBootSwitch.h"
#include "network/SharedFontLoader.h"
#include "util/QrUtils.h"

GfxRenderer renderer(display);

static EpdFont ui10RegularFont(&ubuntu_10_regular);
static EpdFont ui10BoldFont(&ubuntu_10_bold);
static EpdFontFamily ui10FontFamily(10, &ui10RegularFont, &ui10BoldFont);

static EpdFont ui12RegularFont(&ubuntu_12_regular);
static EpdFont ui12BoldFont(&ubuntu_12_bold);
static EpdFontFamily ui12FontFamily(12, &ui12RegularFont, &ui12BoldFont);

namespace {

constexpr const char* kBadgesDir = "/badges";
constexpr const char* kCardsTxtPath = "/badges/cards.txt";
constexpr size_t kMaxBadgeFileBytes = 2048;
constexpr size_t kMaxBadges = 8;
constexpr uint8_t kHalfRefreshCadence = 15;
constexpr uint32_t kIdleAutoSleepMs = 20UL * 60UL * 1000UL;  // 20 minutes

enum class DeckTab : uint8_t {
  Notifications,
  BadgeVault,
};

DeckTab g_activeTab = DeckTab::Notifications;
size_t g_selectedNotifIdx = 0;
size_t g_selectedBadgeIdx = 0;
size_t g_badgeCount = 0;
bool g_forceFullRefresh = true;
uint8_t g_partialRefreshCount = 0;
uint32_t g_lastActivityMs = 0;

ancs::AncsNotification g_currentNotif{};
size_t g_notifCount = 0;
IosAncsService::State g_ancsState = IosAncsService::State::Stopped;
std::array<magsafe_badge::BadgeCard, kMaxBadges> g_badges{};
SharedFontLoader g_sharedFonts{};

static_assert(magsafe_badge::BadgeCard::kQrMax - 1 <= QrUtils::kMaxPayloadBytes,
              "Badge QR payloads must fit the largest QR version QrUtils draws");

bool hasExtension(const char* name, const char* ext) {
  if (!name || !ext) return false;
  const size_t nameLen = std::strlen(name);
  const size_t extLen = std::strlen(ext);
  if (nameLen <= extLen) return false;
  return strcasecmp(name + (nameLen - extLen), ext) == 0;
}

void drawPopupMessage(const char* msg) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int boxW = w - 60;
  const int boxH = 76;
  const int boxX = 30;
  const int boxY = (h - boxH) / 2;
  renderer.fillRect(boxX, boxY, boxW, boxH, false);
  renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 2, 8, true);
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 26, msg, true, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawHeaderBar(const int pageWidth, const char* title, const char* subtitle) {
  renderer.drawText(UI_12_FONT_ID, 20, 14, title, true, EpdFontFamily::BOLD);
  if (subtitle && subtitle[0] != '\0') {
    const auto subTrunc = renderer.truncatedText(UI_10_FONT_ID, subtitle, pageWidth - 40, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, 20, 40, subTrunc.c_str(), true, EpdFontFamily::REGULAR);
  }
  renderer.drawLine(16, 64, pageWidth - 16, 64, true);
}

void drawBottomButtonHints(const int pageWidth, const int pageHeight, const char* b1, const char* b2, const char* b3,
                           const char* b4) {
  const int topY = pageHeight - 38;
  renderer.drawLine(16, topY, pageWidth - 16, topY, true);
  const char* labels[4] = {b1, b2, b3, b4};
  const int slotW = pageWidth / 4;
  for (int i = 0; i < 4; ++i) {
    const int cx = slotW * i + (slotW - renderer.getTextWidth(UI_10_FONT_ID, labels[i], EpdFontFamily::BOLD)) / 2;
    renderer.drawText(UI_10_FONT_ID, std::max(4, cx), topY + 10, labels[i], true, EpdFontFamily::BOLD);
  }
}

void loadBadgesFromSd() {
  g_badgeCount = 0;
  for (auto& b : g_badges) {
    b.clear();
  }

  if (Storage.ready() && Storage.exists(kBadgesDir)) {
    if (Storage.exists(kCardsTxtPath)) {
      std::string raw;
      if (Storage.readFileToString("DECK", kCardsTxtPath, kMaxBadgeFileBytes, raw)) {
        g_badgeCount = magsafe_badge::parseCardsText(raw, g_badges, g_badgeCount);
      }
    }

    const auto files = Storage.listFiles(kBadgesDir, 16);
    for (const auto& name : files) {
      if (g_badgeCount >= kMaxBadges) break;
      const char* cname = name.c_str();
      if (!cname || cname[0] == '.') continue;  // Skip hidden / macOS AppleDouble (._*) files

      if (hasExtension(cname, ".json")) {
        std::string fullPath = std::string(kBadgesDir) + "/" + cname;
        std::string jsonStr;
        if (!Storage.readFileToString("DECK", fullPath, kMaxBadgeFileBytes, jsonStr)) continue;
        JsonDocument doc;
        if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) continue;

        auto& card = g_badges[g_badgeCount];
        card.clear();
        card.kind = magsafe_badge::BadgeKind::PassOrCard;
        const char* metaVal = doc["meta"] | doc["detail"] | "";
        magsafe_badge::copyTrimmedField(card.tag, sizeof(card.tag), doc["tag"] | "MAGSAFE PASS");
        magsafe_badge::copyTrimmedField(card.title, sizeof(card.title), doc["title"] | cname);
        magsafe_badge::copyTrimmedField(card.subtitle, sizeof(card.subtitle), doc["subtitle"] | "");
        magsafe_badge::copyTrimmedField(card.meta, sizeof(card.meta), metaVal);
        magsafe_badge::copyTrimmedField(card.qrPayload, sizeof(card.qrPayload), doc["qr"] | "");
        if (card.title[0] != '\0' || card.qrPayload[0] != '\0') {
          ++g_badgeCount;
        } else {
          card.clear();
        }
      } else if (hasExtension(cname, ".bmp")) {
        auto& card = g_badges[g_badgeCount];
        card.clear();
        const int n = std::snprintf(card.bmpPath, sizeof(card.bmpPath), "%s/%s", kBadgesDir, cname);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(card.bmpPath)) {
          card.clear();
          continue;
        }
        HalFile bmpFile;
        if (!Storage.openFileForRead("DECK", card.bmpPath, bmpFile) || !bmpFile) {
          card.clear();
          continue;
        }
        Bitmap probe(bmpFile, /*dithering=*/false);
        const bool validBmp = (probe.parseHeaders() == BmpReaderError::Ok);
        bmpFile.close();
        if (!validBmp) {
          card.clear();
          continue;
        }
        card.kind = magsafe_badge::BadgeKind::BmpImage;
        magsafe_badge::copyTrimmedField(card.tag, sizeof(card.tag), "BMP BADGE");
        magsafe_badge::copyTrimmedField(card.title, sizeof(card.title), cname);
        ++g_badgeCount;
      }
    }
  }

  if (g_badgeCount == 0 && kMaxBadges >= 2) {
    magsafe_badge::populateDefaultCards(g_badges[0], g_badges[1]);
    g_badgeCount = 2;
  }
}

void renderNotificationsTab(const int pageWidth, const int pageHeight) {
  const char* statusSub = "Pair 'X4-MagSafe' in iOS Bluetooth Settings";
  switch (g_ancsState) {
    case IosAncsService::State::Stopped:
    case IosAncsService::State::Advertising:
      statusSub = "Pair 'X4-MagSafe' in iOS Bluetooth Settings";
      break;
    case IosAncsService::State::ConnectedAuthenticating:
      statusSub = "Authenticating with iPhone...";
      break;
    case IosAncsService::State::DiscoveringAncs:
      statusSub = "Subscribing to iOS ANCS...";
      break;
    case IosAncsService::State::SubscribedReady:
      statusSub = "iPhone Connected — No New Notifications";
      break;
  }

  drawHeaderBar(pageWidth, "iOS Notifications", statusSub);

  const int contentTop = 76;
  const int contentBottom = pageHeight - 50;
  const int margin = 20;
  const int cardWidth = pageWidth - margin * 2;

  if (g_notifCount == 0 || !g_currentNotif.valid) {
    renderer.drawRect(margin, contentTop, cardWidth, contentBottom - contentTop, 2, true);
    const int centerY = (contentTop + contentBottom) / 2 - 28;
    const char* emptyTitle =
        (g_ancsState == IosAncsService::State::SubscribedReady) ? "All Caught Up" : "Waiting for iPhone ANCS";
    renderer.drawCenteredText(UI_12_FONT_ID, centerY, emptyTitle, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 34, statusSub, true, EpdFontFamily::REGULAR);
    return;
  }

  const size_t idx = std::min(g_selectedNotifIdx, g_notifCount - 1);
  const ancs::AncsNotification& item = g_currentNotif;

  char metaLine[96];
  char timeBuf[8];
  ancs::formatAncsTime(item.date, timeBuf);
  if (timeBuf[0] != '\0') {
    std::snprintf(metaLine, sizeof(metaLine), "[%u/%u]  %s   %s", static_cast<unsigned>(idx + 1),
                  static_cast<unsigned>(g_notifCount), ancs::friendlyAppName(item.appId), timeBuf);
  } else {
    std::snprintf(metaLine, sizeof(metaLine), "[%u/%u]  %s", static_cast<unsigned>(idx + 1),
                  static_cast<unsigned>(g_notifCount), ancs::friendlyAppName(item.appId));
  }

  int y = contentTop;
  renderer.drawRect(margin, y, cardWidth, contentBottom - y, 2, true);
  y += 12;
  renderer.drawText(UI_10_FONT_ID, margin + 14, y, metaLine, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  renderer.drawLine(margin + 10, y, margin + cardWidth - 10, y, true);
  y += 10;

  if (item.title[0] != '\0') {
    const auto titleTrunc = renderer.truncatedText(UI_12_FONT_ID, item.title, cardWidth - 28, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, margin + 14, y, titleTrunc.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 8;
  }

  if (item.message[0] != '\0') {
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID) + 4;
    const int availHeight = std::max(lineHeight, (contentBottom - 14) - y);
    const int maxLines = std::max(1, availHeight / lineHeight);
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, item.message, cardWidth - 28, maxLines);
    for (const auto& ln : lines) {
      renderer.drawText(UI_12_FONT_ID, margin + 14, y, ln.c_str(), true, EpdFontFamily::REGULAR);
      y += lineHeight;
    }
  }
}

void renderBadgeVaultTab(const int pageWidth, const int pageHeight, const bool cleanLockFrame) {
  if (g_badgeCount == 0) return;
  const size_t idx = std::min(g_selectedBadgeIdx, g_badgeCount - 1);
  const magsafe_badge::BadgeCard& card = g_badges[idx];

  int topY = 16;
  int bottomY = pageHeight - 16;

  if (!cleanLockFrame) {
    char sub[48];
    std::snprintf(sub, sizeof(sub), "%s (%u/%u)", card.tag[0] ? card.tag : "MAGSAFE PASS",
                  static_cast<unsigned>(idx + 1), static_cast<unsigned>(g_badgeCount));
    drawHeaderBar(pageWidth, "MagSafe Pass Vault", sub);
    topY = 74;
    bottomY = pageHeight - 48;
  }

  const int availH = std::max(60, bottomY - topY);

  if (card.kind == magsafe_badge::BadgeKind::BmpImage && card.bmpPath[0] != '\0') {
    HalFile file;
    if (Storage.openFileForRead("DECK", card.bmpPath, file) && file) {
      auto bmp = makeUniqueNoThrow<Bitmap>(file, /*dithering=*/true);
      if (bmp && bmp->parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(*bmp, 0, topY, pageWidth, availH);
        return;
      }
    }
    // Fall through to render a visible fallback card if the SD card or BMP read failed.
  }

  const int margin = 20;
  const int boxW = pageWidth - margin * 2;
  renderer.drawRoundedRect(margin, topY, boxW, availH, 2, 10, true);

  int y = topY + 14;
  if (cleanLockFrame && card.tag[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, y, card.tag, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  }

  if (card.title[0] != '\0') {
    const auto trunc = renderer.truncatedText(UI_12_FONT_ID, card.title, boxW - 24, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, y, trunc.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 6;
  }

  if (card.subtitle[0] != '\0') {
    const auto trunc = renderer.truncatedText(UI_12_FONT_ID, card.subtitle, boxW - 24, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, y, trunc.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 6;
  }

  if (card.meta[0] != '\0') {
    const auto trunc = renderer.truncatedText(UI_10_FONT_ID, card.meta, boxW - 24, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, y, trunc.c_str(), true, EpdFontFamily::REGULAR);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;
  }

  if (card.qrPayload[0] != '\0') {
    const int qrTop = y + 4;
    const int qrAvailH = std::max(80, (topY + availH - 16) - qrTop);
    const int qrSide = std::min(boxW - 40, qrAvailH);
    const int qrX = (pageWidth - qrSide) / 2;
    renderer.fillRect(qrX - 8, qrTop - 4, qrSide + 16, qrSide + 8, false);
    QrUtils::drawQrCode(renderer, Rect{qrX, qrTop, qrSide, qrSide}, card.qrPayload);
  }
}

void commitDisplayRefresh(const bool fullRefresh) {
  if (fullRefresh) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    g_forceFullRefresh = false;
    g_partialRefreshCount = 0;
  } else if (++g_partialRefreshCount >= kHalfRefreshCadence) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    g_partialRefreshCount = 0;
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void renderDeckFrame(const bool cleanLockFrame = false) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (cleanLockFrame) {
    display.setInverted(false);
    renderBadgeVaultTab(pageWidth, pageHeight, /*cleanLockFrame=*/true);
    commitDisplayRefresh(/*fullRefresh=*/true);
    return;
  }

  if (g_activeTab == DeckTab::Notifications) {
    renderNotificationsTab(pageWidth, pageHeight);
    const bool hasPos = g_notifCount > 0 && g_currentNotif.valid &&
                        (g_currentNotif.eventFlags & ancs::kEventFlagPositiveAction) != 0;
    drawBottomButtonHints(pageWidth, pageHeight, "Reader", hasPos ? "Dismiss / OK" : "Dismiss", "Tab", "Next");
  } else {
    renderBadgeVaultTab(pageWidth, pageHeight, /*cleanLockFrame=*/false);
    drawBottomButtonHints(pageWidth, pageHeight, "Reader", "Lock Pass", "Tab", "Next");
  }

  commitDisplayRefresh(g_forceFullRefresh);
}

void lockPassAndDeepSleep() {
  IosAncsService::getInstance().end();
  renderDeckFrame(/*cleanLockFrame=*/true);

  g_sharedFonts.unload(renderer);
  display.deepSleep();
  Storage.prepareForDeepSleep();
  gpio.prepareForDeepSleep();
  powerManager.startDeepSleep(gpio, false);
}

void switchBackToReaderPartition() {
  IosAncsService::getInstance().end();
  if (ota_boot::switchToNextPartition()) {
    drawPopupMessage("Switching boot partition...");
    g_sharedFonts.unload(renderer);
    delay(250);
    ESP.restart();
  } else {
    drawPopupMessage("No valid firmware in alternate slot");
    delay(800);
    g_forceFullRefresh = true;
    renderDeckFrame();
  }
}

void syncNotificationSelection() {
  g_notifCount = IosAncsService::getInstance().notificationCount();
  if (g_selectedNotifIdx >= g_notifCount && g_notifCount > 0) {
    g_selectedNotifIdx = g_notifCount - 1;
  }
  IosAncsService::getInstance().getNotification(g_selectedNotifIdx, g_currentNotif, g_notifCount);
}

}  // namespace

void setup() {
  BoardConfig::holdPowerRails();
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.begin(115200);

  HalSystem::begin();
  gpio.begin();
  powerManager.begin();

  // Sample and debounce the resistor-ladder ADC before inspecting cold-boot button combos.
  for (int i = 0; i < 4; ++i) {
    gpio.update();
    delay(6);
  }
  const auto wakeupReason = gpio.getWakeupReason();
  const bool powerBoot = (wakeupReason == HalGPIO::WakeupReason::PowerButton) ||
                         (wakeupReason == HalGPIO::WakeupReason::AfterUSBPower &&
                          gpio.isPressed(HalGPIO::BTN_POWER));
  const bool switchBackShortcut =
      powerBoot && (esp_reset_reason() != ESP_RST_SW) && gpio.isPressed(HalGPIO::BTN_LEFT);

  Storage.begin();
  HalSystem::checkPanic();

  display.begin(/*seamless=*/false);
  renderer.begin();
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);

  if (switchBackShortcut) {
    switchBackToReaderPartition();
  }

  g_sharedFonts.begin(renderer);
  loadBadgesFromSd();

  if (IosAncsService::getInstance().begin("X4-MagSafe")) {
    g_activeTab = DeckTab::Notifications;
  } else {
    g_activeTab = DeckTab::BadgeVault;
  }

  g_ancsState = IosAncsService::getInstance().state();
  syncNotificationSelection();
  g_forceFullRefresh = true;
  renderDeckFrame();

  powerManager.setPowerSaving(true);
  g_lastActivityMs = millis();
}

void loop() {
  gpio.update();
  const uint32_t now = millis();
  bool dirty = false;

  if (IosAncsService::getInstance().consumeUiDirty()) {
    g_ancsState = IosAncsService::getInstance().state();
    syncNotificationSelection();
    g_lastActivityMs = now;
    dirty = true;
  }

  if (gpio.wasAnyPressed() || gpio.wasAnyReleased()) {
    g_lastActivityMs = now;
  }

  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    g_activeTab = (g_activeTab == DeckTab::Notifications) ? DeckTab::BadgeVault : DeckTab::Notifications;
    dirty = true;
  }

  const bool prevPressed = gpio.wasPressed(HalGPIO::BTN_UP);
  const bool nextPressed = gpio.wasPressed(HalGPIO::BTN_RIGHT) || gpio.wasPressed(HalGPIO::BTN_DOWN);
  if (prevPressed || nextPressed) {
    if (g_activeTab == DeckTab::Notifications && g_notifCount > 0) {
      g_selectedNotifIdx =
          nextPressed ? (g_selectedNotifIdx + 1) % g_notifCount : (g_selectedNotifIdx + g_notifCount - 1) % g_notifCount;
      IosAncsService::getInstance().getNotification(g_selectedNotifIdx, g_currentNotif, g_notifCount);
      dirty = true;
    } else if (g_activeTab == DeckTab::BadgeVault && g_badgeCount > 0) {
      g_selectedBadgeIdx =
          nextPressed ? (g_selectedBadgeIdx + 1) % g_badgeCount : (g_selectedBadgeIdx + g_badgeCount - 1) % g_badgeCount;
      dirty = true;
    }
  }

  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    if (g_activeTab == DeckTab::Notifications) {
      if (g_selectedNotifIdx < g_notifCount && g_currentNotif.valid) {
        IosAncsService::getInstance().dismissNotification(g_currentNotif.uid);
        syncNotificationSelection();
        dirty = true;
      }
    } else {
      lockPassAndDeepSleep();
      return;
    }
  }

  if (gpio.wasReleased(HalGPIO::BTN_POWER) && g_activeTab == DeckTab::Notifications) {
    if (g_selectedNotifIdx < g_notifCount && g_currentNotif.valid &&
        (g_currentNotif.eventFlags & ancs::kEventFlagPositiveAction) != 0) {
      IosAncsService::getInstance().performAction(g_currentNotif.uid, ancs::ActionId::Positive);
      syncNotificationSelection();
      dirty = true;
    }
  }

  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    switchBackToReaderPartition();
    return;
  }

  if ((now - g_lastActivityMs) >= kIdleAutoSleepMs) {
    lockPassAndDeepSleep();
    return;
  }

  if (dirty) {
    renderDeckFrame();
  }
  delay(15);
}

#include "IosAncsService.h"

#include <Arduino.h>
#include <Logging.h>

#if ENABLE_X4_DECK

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>

#include "AncsNotificationList.h"
#include "AppleBleParams.h"

namespace {

constexpr uint32_t kBurstCoalesceMs = 250;
constexpr uint32_t kMaxBurstDelayMs = 1200;
constexpr uint32_t kAttrRequestTimeoutMs = 2000;
constexpr uint32_t kDiscoveryRetryIntervalMs = 400;
constexpr uint8_t kMaxDiscoveryRetries = 3;
constexpr size_t kPendingQueueCap = 8;
constexpr size_t kActionQueueCap = 4;
constexpr size_t kAppQueueCap = 4;
constexpr size_t kAppNameCacheCap = 12;

struct PendingUidEntry {
  uint32_t uid = 0;
  uint8_t categoryId = 0;
  uint8_t eventFlags = 0;
};

struct PendingActionEntry {
  uint32_t uid = 0;
  ancs::ActionId action = ancs::ActionId::Negative;
};

StaticSemaphore_t g_ancsMutexStorage;
SemaphoreHandle_t g_ancsMutex = nullptr;

class ScopedLock {
 public:
  ScopedLock() {
    if (!g_ancsMutex) {
      g_ancsMutex = xSemaphoreCreateMutexStatic(&g_ancsMutexStorage);
    }
    xSemaphoreTake(g_ancsMutex, portMAX_DELAY);
  }
  ~ScopedLock() { xSemaphoreGive(g_ancsMutex); }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
};

std::atomic<bool> g_running{false};
std::atomic<IosAncsService::State> g_state{IosAncsService::State::Stopped};

// Owned exclusively by the foreground thread (begin / poll / end):
NimBLEServer* g_server = nullptr;
NimBLEClient* g_client = nullptr;
NimBLERemoteCharacteristic* g_ctrlPointChr = nullptr;

// Guarded by g_ancsMutex:
uint16_t g_connHandle = BLE_HS_CONN_HANDLE_NONE;
// Incremented on every connect / disconnect. NimBLE reuses conn handles (single-connection builds
// almost always get the same value), so the handle alone cannot tell a reconnect from the same link.
uint32_t g_connGeneration = 0;
bool g_peerCleanupNeeded = false;
bool g_needDiscovery = false;
uint8_t g_discoveryRetries = 0;
uint32_t g_nextDiscoveryAttemptMs = 0;

ancs::NotificationList<IosAncsService::kMaxNotifications> g_items{};

std::array<PendingUidEntry, kPendingQueueCap> g_pendingUids{};
size_t g_pendingHead = 0;
size_t g_pendingTail = 0;
size_t g_pendingCount = 0;

std::array<PendingActionEntry, kActionQueueCap> g_pendingActions{};
size_t g_actionHead = 0;
size_t g_actionTail = 0;
size_t g_actionCount = 0;

// App identifiers waiting for a GetAppAttributes(DisplayName) request (FIFO, deduplicated).
std::array<std::array<char, ancs::AncsNotification::kAppIdMax>, kAppQueueCap> g_pendingApps{};
size_t g_pendingAppCount = 0;

// At most one Control Point attribute request is in flight; its response arrives on Data Source.
enum class RequestKind : uint8_t { Notification, App };
bool g_requestInFlight = false;
RequestKind g_inFlightKind = RequestKind::Notification;
PendingUidEntry g_inFlightMeta{};
std::array<char, ancs::AncsNotification::kAppIdMax> g_inFlightAppId{};
uint32_t g_requestSentAtMs = 0;

ancs::AncsDataSourceParser g_parser{};
ancs::AncsAppAttributeParser g_appParser{};
// Kept across reconnects: an app's display name does not change between sessions.
ancs::AppNameCache<kAppNameCacheCap> g_appNames{};

bool g_uiDirty = false;
uint32_t g_firstDirtyMs = 0;
uint32_t g_lastChangeMs = 0;

void markDirtyLocked(uint32_t nowMs) {
  if (!g_uiDirty) {
    g_firstDirtyMs = nowMs;
  }
  g_uiDirty = true;
  g_lastChangeMs = nowMs;
}

void removeUidLocked(uint32_t uid) { g_items.remove(uid); }

void removePendingUidLocked(uint32_t uid) {
  size_t writeIdx = 0;
  for (size_t i = 0; i < g_pendingCount; ++i) {
    const PendingUidEntry entry = g_pendingUids[(g_pendingTail + i) % kPendingQueueCap];
    if (entry.uid != uid) {
      g_pendingUids[(g_pendingTail + writeIdx) % kPendingQueueCap] = entry;
      ++writeIdx;
    }
  }
  g_pendingCount = writeIdx;
  g_pendingHead = (g_pendingTail + g_pendingCount) % kPendingQueueCap;

  if (g_requestInFlight && g_inFlightKind == RequestKind::Notification && g_inFlightMeta.uid == uid) {
    g_requestInFlight = false;
    g_parser.reset();
  }
}

void enqueuePendingUidLocked(uint32_t uid, uint8_t categoryId, uint8_t eventFlags) {
  for (size_t i = 0; i < g_pendingCount; ++i) {
    const size_t idx = (g_pendingTail + i) % kPendingQueueCap;
    if (g_pendingUids[idx].uid == uid) {
      g_pendingUids[idx].categoryId = categoryId;
      g_pendingUids[idx].eventFlags = eventFlags;
      return;
    }
  }
  if (g_pendingCount == kPendingQueueCap) {
    g_pendingTail = (g_pendingTail + 1) % kPendingQueueCap;
    --g_pendingCount;
  }
  g_pendingUids[g_pendingHead] = PendingUidEntry{uid, categoryId, eventFlags};
  g_pendingHead = (g_pendingHead + 1) % kPendingQueueCap;
  ++g_pendingCount;
}

bool popPendingUidLocked(PendingUidEntry& out) {
  if (g_pendingCount == 0) return false;
  out = g_pendingUids[g_pendingTail];
  g_pendingTail = (g_pendingTail + 1) % kPendingQueueCap;
  --g_pendingCount;
  return true;
}

void enqueueAppLocked(const char* appId) {
  if (!appId || appId[0] == '\0' || g_appNames.find(appId)) return;
  if (g_requestInFlight && g_inFlightKind == RequestKind::App && std::strcmp(g_inFlightAppId.data(), appId) == 0) {
    return;
  }
  for (size_t i = 0; i < g_pendingAppCount; ++i) {
    if (std::strcmp(g_pendingApps[i].data(), appId) == 0) return;
  }
  if (g_pendingAppCount == kAppQueueCap) return;  // retried when the app's next notification arrives
  std::strncpy(g_pendingApps[g_pendingAppCount].data(), appId, ancs::AncsNotification::kAppIdMax - 1);
  g_pendingApps[g_pendingAppCount][ancs::AncsNotification::kAppIdMax - 1] = '\0';
  ++g_pendingAppCount;
}

bool popAppLocked(std::array<char, ancs::AncsNotification::kAppIdMax>& out) {
  if (g_pendingAppCount == 0) return false;
  out = g_pendingApps[0];
  for (size_t i = 1; i < g_pendingAppCount; ++i) {
    g_pendingApps[i - 1] = g_pendingApps[i];
  }
  --g_pendingAppCount;
  return true;
}

void clearActionsLocked() {
  g_actionCount = 0;
  g_actionHead = 0;
  g_actionTail = 0;
}

void enqueueActionLocked(uint32_t uid, ancs::ActionId action) {
  if (g_actionCount == kActionQueueCap) {
    g_actionTail = (g_actionTail + 1) % kActionQueueCap;
    --g_actionCount;
  }
  g_pendingActions[g_actionHead] = PendingActionEntry{uid, action};
  g_actionHead = (g_actionHead + 1) % kActionQueueCap;
  ++g_actionCount;
}

bool popActionLocked(PendingActionEntry& out) {
  if (g_actionCount == 0) return false;
  out = g_pendingActions[g_actionTail];
  g_actionTail = (g_actionTail + 1) % kActionQueueCap;
  --g_actionCount;
  return true;
}

void onDataSourceNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  IosAncsService::getInstance().onDataSourceBytes(data, len);
}

void onNotifSourceNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  IosAncsService::getInstance().onNotificationSourceBytes(data, len);
}

void onParserComplete(const ancs::AncsNotification& item, void*) {
  IosAncsService::getInstance().onAttributeComplete(item);
}

// Called with g_ancsMutex held inside onDataSourceBytes on the ble_hs task.
void onAppParserComplete(const char* appId, const char* displayName, void*) {
  if (!g_requestInFlight || g_inFlightKind != RequestKind::App || std::strcmp(g_inFlightAppId.data(), appId) != 0) {
    return;
  }
  g_requestInFlight = false;
  g_appNames.put(appId, displayName);
  if (displayName[0] != '\0' && g_items.setAppName(appId, displayName)) {
    markDirtyLocked(millis());
  }
}

class AncsServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    IosAncsService::getInstance().onPeerConnect(connInfo.getConnHandle());
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    IosAncsService::getInstance().onPeerDisconnect();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    IosAncsService::getInstance().onPeerAuthComplete(connInfo.getConnHandle(), connInfo.isEncrypted());
  }
};

AncsServerCallbacks g_serverCallbacks;

char g_lastError[64] = {0};

void setLastError(const char* what) {
  // Free internal heap is the usual culprit on the no-PSRAM ESP32-C3, so record it with the reason.
  const unsigned freeKb = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  std::snprintf(g_lastError, sizeof(g_lastError), "%s (free heap %u KB)", what, freeKb);
  LOG_ERR("ANCS", "%s", g_lastError);
}

char g_deviceName[32] = "X4-MagSafe";
// Set by onPeerDisconnect() (NimBLE host task) and consumed by poll() on the foreground thread.
std::atomic<bool> g_restartFastAdvertising{false};

void startAdvertisingAncs(const char* deviceName, bool fast) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  adv->stop();

  // Apple QA1931: 20 ms for the first 30 s so iOS discovers / reconnects quickly, then an exact slow
  // interval. The fast burst is time-limited by NimBLE; poll() switches to the slow interval when it ends.
  const uint16_t interval = fast ? apple_ble::kFastAdvIntervalUnits : apple_ble::kSlowAdvIntervalUnits;
  adv->setMinInterval(interval);
  adv->setMaxInterval(interval);

  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addData(ancs::kAncsSolicitationAd, sizeof(ancs::kAncsSolicitationAd));
  adv->setAdvertisementData(advData);

  // Place Complete Local Name in Scan Response so the primary PDU stays at 21 bytes (<= 31B limit).
  NimBLEAdvertisementData scanResp;
  scanResp.setName(deviceName ? deviceName : "X4-MagSafe");
  adv->setScanResponseData(scanResp);

  adv->start(fast ? apple_ble::kFastAdvDurationMs : 0);
}

// Foreground-thread advertising management: restart the fast burst after a disconnect, and drop to
// the slow interval once the 30 s burst has expired without a connection.
void manageAdvertisingOnOwnerThread() {
  if (!g_server || g_state.load() != IosAncsService::State::Advertising) return;
  if (g_server->getConnectedCount() > 0) return;  // a connection is being set up; leave it alone
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  if (g_restartFastAdvertising.exchange(false)) {
    startAdvertisingAncs(g_deviceName, /*fast=*/true);
  } else if (!adv->isAdvertising()) {
    startAdvertisingAncs(g_deviceName, /*fast=*/false);
  }
}

void cleanupClientOnOwnerThread() {
  g_ctrlPointChr = nullptr;
  if (g_client) {
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
  }
}

bool discoverAndSubscribeAncs(uint16_t connHandle) {
  if (!g_server || connHandle == BLE_HS_CONN_HANDLE_NONE) return false;

  cleanupClientOnOwnerThread();

  g_client = g_server->getClient(connHandle);
  if (!g_client) {
    LOG_ERR("ANCS", "getClient(%u) failed", static_cast<unsigned>(connHandle));
    return false;
  }

  NimBLERemoteService* svc = g_client->getService(NimBLEUUID(ancs::kAncsServiceUuid));
  if (!svc) {
    LOG_ERR("ANCS", "ANCS service not found on peer");
    cleanupClientOnOwnerThread();
    return false;
  }

  NimBLERemoteCharacteristic* dataSrc = svc->getCharacteristic(NimBLEUUID(ancs::kAncsDataSourceUuid));
  NimBLERemoteCharacteristic* notifSrc = svc->getCharacteristic(NimBLEUUID(ancs::kAncsNotificationSourceUuid));
  NimBLERemoteCharacteristic* ctrlPt = svc->getCharacteristic(NimBLEUUID(ancs::kAncsControlPointUuid));
  if (!dataSrc || !notifSrc || !ctrlPt) {
    LOG_ERR("ANCS", "ANCS characteristics missing");
    cleanupClientOnOwnerThread();
    return false;
  }

  // Apple ANCS Specification requires subscribing to Data Source BEFORE Notification Source.
  if (!dataSrc->subscribe(true, onDataSourceNotify)) {
    LOG_ERR("ANCS", "Data Source subscribe failed");
    cleanupClientOnOwnerThread();
    return false;
  }
  if (!notifSrc->subscribe(true, onNotifSourceNotify)) {
    LOG_ERR("ANCS", "Notification Source subscribe failed");
    cleanupClientOnOwnerThread();
    return false;
  }

  g_ctrlPointChr = ctrlPt;
  // Request power-friendly peripheral connection parameters (see AppleBleParams.h for the rationale).
  g_server->updateConnParams(connHandle, apple_ble::kConnIntervalMinUnits, apple_ble::kConnIntervalMaxUnits,
                             apple_ble::kConnLatency, apple_ble::kConnSupervisionTimeoutUnits);
  return true;
}

}  // namespace

IosAncsService& IosAncsService::getInstance() {
  static IosAncsService instance;
  return instance;
}

bool IosAncsService::begin(const char* deviceName) {
  if (g_running.load()) return true;
  g_lastError[0] = '\0';

  if (!g_ancsMutex) {
    g_ancsMutex = xSemaphoreCreateMutexStatic(&g_ancsMutexStorage);
  }

  if (NimBLEDevice::isInitialized()) {
    cleanupClientOnOwnerThread();
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!NimBLEDevice::init(deviceName ? deviceName : "X4-MagSafe")) {
    setLastError("Bluetooth init failed");
    return false;
  }

  NimBLEDevice::setMTU(185);
  // iOS requires Bonding + LE Secure Connections + Identity Key (IRK) distribution
  // for Resolvable Private Addresses and ANCS access.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  g_server = NimBLEDevice::createServer();
  if (!g_server) {
    setLastError("BLE server create failed");
    NimBLEDevice::deinit(true);
    return false;
  }
  g_server->setCallbacks(&g_serverCallbacks);
  // Advertising is restarted by poll() (fast burst first) instead of NimBLE reusing the last interval.
  g_server->advertiseOnDisconnect(false);

  {
    ScopedLock lock;
    g_connHandle = BLE_HS_CONN_HANDLE_NONE;
    g_peerCleanupNeeded = false;
    g_needDiscovery = false;
    g_discoveryRetries = 0;
    g_nextDiscoveryAttemptMs = 0;
    g_pendingHead = 0;
    g_pendingTail = 0;
    g_pendingCount = 0;
    g_actionHead = 0;
    g_actionTail = 0;
    g_actionCount = 0;
    g_requestInFlight = false;
    g_parser.reset();
    markDirtyLocked(millis());
  }

  g_ctrlPointChr = nullptr;
  g_client = nullptr;
  g_state.store(State::Advertising);
  std::snprintf(g_deviceName, sizeof(g_deviceName), "%s", deviceName ? deviceName : "X4-MagSafe");
  g_restartFastAdvertising.store(false);
  startAdvertisingAncs(g_deviceName, /*fast=*/true);

  g_running.store(true);
  LOG_INF("ANCS", "Started iOS ANCS advertising as '%s'", deviceName ? deviceName : "X4-MagSafe");
  return true;
}

bool IosAncsService::end(uint32_t timeoutMs) {
  if (!g_running.load()) return true;

  if (NimBLEDevice::isInitialized()) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv && adv->isAdvertising()) {
      adv->stop();
    }
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    {
      ScopedLock lock;
      handle = g_connHandle;
    }
    if (g_server && handle != BLE_HS_CONN_HANDLE_NONE) {
      g_server->disconnect(handle);
      const uint32_t startMs = millis();
      while ((millis() - startMs) < timeoutMs) {
        {
          ScopedLock lock;
          if (g_connHandle == BLE_HS_CONN_HANDLE_NONE) break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    cleanupClientOnOwnerThread();
    NimBLEDevice::deinit(true);
  }

  {
    ScopedLock lock;
    g_connHandle = BLE_HS_CONN_HANDLE_NONE;
    g_peerCleanupNeeded = false;
    g_needDiscovery = false;
    g_discoveryRetries = 0;
    g_requestInFlight = false;
    g_pendingAppCount = 0;
    g_pendingCount = 0;
    g_pendingHead = 0;
    g_pendingTail = 0;
    g_actionCount = 0;
    g_actionHead = 0;
    g_actionTail = 0;
    g_parser.reset();
  }

  g_server = nullptr;
  g_state.store(State::Stopped);
  g_running.store(false);
  LOG_INF("ANCS", "Stopped iOS ANCS service");
  return true;
}

void IosAncsService::poll() {
  if (!g_running.load()) return;

  const uint32_t now = millis();
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  uint32_t generation = 0;
  bool doCleanup = false;
  bool doDiscovery = false;

  {
    ScopedLock lock;
    if (g_peerCleanupNeeded) {
      g_peerCleanupNeeded = false;
      doCleanup = true;
    }
    handle = g_connHandle;
    generation = g_connGeneration;
    if (g_needDiscovery && handle != BLE_HS_CONN_HANDLE_NONE &&
        static_cast<int32_t>(now - g_nextDiscoveryAttemptMs) >= 0) {
      g_needDiscovery = false;
      doDiscovery = true;
    }
  }

  if (doCleanup) {
    cleanupClientOnOwnerThread();
  }

  manageAdvertisingOnOwnerThread();

  if (doDiscovery && handle != BLE_HS_CONN_HANDLE_NONE) {
    if (discoverAndSubscribeAncs(handle)) {
      bool stillConnected = false;
      {
        ScopedLock lock;
        // The peer may have disconnected while the blocking GATT discovery ran on this thread; in that
        // case onPeerDisconnect() already moved the state to Advertising and must not be overwritten.
        stillConnected = (g_connGeneration == generation && g_connHandle == handle);
        if (stillConnected) {
          g_discoveryRetries = 0;
          // Actions the user queued against the old list while disconnected carry stale UIDs.
          clearActionsLocked();
          // ANCS UIDs from a previous connection are stale: notifications removed on the phone while we
          // were disconnected never produce a Removed event. iOS re-announces every live notification
          // with the PreExisting flag right after subscription, so rebuild the list from scratch.
          g_items.clear();
          // Publish the new state while still holding the lock: every g_state transition driven by the
          // connection lifecycle is stored under g_ancsMutex, so they are totally ordered.
          g_state.store(State::SubscribedReady);
          markDirtyLocked(millis());
        }
      }
      if (stillConnected) {
        LOG_INF("ANCS", "Subscribed to iOS ANCS");
      } else {
        LOG_INF("ANCS", "Peer disconnected during ANCS discovery; discarding subscription");
        cleanupClientOnOwnerThread();
      }
    } else {
      bool shouldDisconnect = false;
      {
        ScopedLock lock;
        if (g_connGeneration == generation && g_connHandle == handle) {
          ++g_discoveryRetries;
          if (g_discoveryRetries < kMaxDiscoveryRetries) {
            g_needDiscovery = true;
            g_nextDiscoveryAttemptMs = millis() + kDiscoveryRetryIntervalMs;
          } else {
            g_discoveryRetries = 0;
            shouldDisconnect = true;
          }
        }
      }
      if (shouldDisconnect && g_server) {
        LOG_ERR("ANCS", "ANCS discovery failed after %u retries; disconnecting peer",
                static_cast<unsigned>(kMaxDiscoveryRetries));
        g_server->disconnect(handle);
      }
    }
  }

  if (g_state.load() != State::SubscribedReady || !g_ctrlPointChr) {
    return;
  }

  // 1. Dispatch any queued Positive / Negative action requests.
  PendingActionEntry actionReq{};
  bool hasAction = false;
  {
    ScopedLock lock;
    hasAction = popActionLocked(actionReq);
  }
  if (hasAction && g_ctrlPointChr) {
    uint8_t pkt[ancs::kPerformActionRequestSize];
    const size_t len = ancs::buildPerformActionPacket(actionReq.uid, actionReq.action, pkt);
    g_ctrlPointChr->writeValue(pkt, len, true);
  }

  // 2. Check timeout on the in-flight attribute request, then send the next queued request:
  //    notification attributes first (so new text appears quickly), then app display names.
  bool needSend = false;
  RequestKind sendKind = RequestKind::Notification;
  PendingUidEntry nextReq{};
  std::array<char, ancs::AncsNotification::kAppIdMax> nextApp{};
  {
    ScopedLock lock;
    if (g_connHandle == BLE_HS_CONN_HANDLE_NONE) return;
    if (g_requestInFlight && (now - g_requestSentAtMs) > kAttrRequestTimeoutMs) {
      if (g_inFlightKind == RequestKind::App) {
        // Remember the miss so this app is not asked about on every notification.
        g_appNames.put(g_inFlightAppId.data(), "");
      }
      g_requestInFlight = false;
      g_parser.reset();
      g_appParser.reset();
    }
    if (!g_requestInFlight) {
      if (popPendingUidLocked(nextReq)) {
        sendKind = RequestKind::Notification;
        g_inFlightMeta = nextReq;
        needSend = true;
      } else if (popAppLocked(nextApp)) {
        sendKind = RequestKind::App;
        g_inFlightAppId = nextApp;
        needSend = true;
      }
      if (needSend) {
        g_requestInFlight = true;
        g_inFlightKind = sendKind;
        g_requestSentAtMs = now;
        g_parser.reset();
        g_appParser.reset();
      }
    }
  }

  if (needSend && g_ctrlPointChr) {
    uint8_t pkt[std::max(ancs::kGetAppAttributesRequestMax, ancs::kGetAttributesRequestSize)];
    const size_t len = sendKind == RequestKind::Notification
                           ? ancs::buildGetNotificationAttributesPacket(nextReq.uid, pkt)
                           : ancs::buildGetAppAttributesPacket(nextApp.data(), pkt);
    if (len == 0 || !g_ctrlPointChr->writeValue(pkt, len, true)) {
      ScopedLock lock;
      g_requestInFlight = false;
      // An identifier that cannot be encoded never will be; a failed write is retried on the
      // app's next notification.
      if (sendKind == RequestKind::App && len == 0) {
        g_appNames.put(nextApp.data(), "");
      }
    }
  }
}

IosAncsService::State IosAncsService::state() const { return g_state.load(); }

const char* IosAncsService::lastError() const { return g_lastError; }

bool IosAncsService::isConnected() const {
  const State s = g_state.load();
  return s == State::ConnectedAuthenticating || s == State::DiscoveringAncs || s == State::SubscribedReady;
}

bool IosAncsService::isSubscribed() const { return g_state.load() == State::SubscribedReady; }

bool IosAncsService::consumeUiDirty() {
  poll();
  bool ready = false;
  const uint32_t now = millis();
  ScopedLock lock;
  if (g_uiDirty &&
      ((now - g_lastChangeMs) >= kBurstCoalesceMs || (now - g_firstDirtyMs) >= kMaxBurstDelayMs)) {
    g_uiDirty = false;
    ready = true;
  }
  return ready;
}

bool IosAncsService::getNotification(size_t index, ancs::AncsNotification& out, size_t& outTotal) const {
  bool ok = false;
  ScopedLock lock;
  outTotal = g_items.size();
  if (g_items.size() > 0) {
    const size_t clamped = std::min(index, g_items.size() - 1);
    out = g_items.at(clamped);
    ok = true;
  } else {
    out.clear();
  }
  return ok;
}

size_t IosAncsService::notificationCount() const {
  ScopedLock lock;
  return g_items.size();
}

bool IosAncsService::performAction(uint32_t uid, ancs::ActionId action) {
  ScopedLock lock;
  // Only act on notifications from the live ANCS session: UIDs shown while disconnected are stale, and
  // a queued action would otherwise be sent to iOS after the next subscription.
  if (g_state.load() != State::SubscribedReady) {
    return false;
  }
  removeUidLocked(uid);
  removePendingUidLocked(uid);
  enqueueActionLocked(uid, action);
  markDirtyLocked(millis());
  return true;
}

bool IosAncsService::dismissNotification(uint32_t uid) {
  return performAction(uid, ancs::ActionId::Negative);
}

void IosAncsService::onPeerConnect(uint16_t connHandle) {
  {
    ScopedLock lock;
    g_connHandle = connHandle;
    ++g_connGeneration;
    g_peerCleanupNeeded = true;
    g_needDiscovery = false;
    g_discoveryRetries = 0;
    g_state.store(State::ConnectedAuthenticating);
    markDirtyLocked(millis());
  }

  // Initiate SMP encryption/bonding immediately so iOS exposes ANCS.
  NimBLEDevice::startSecurity(connHandle);
}

void IosAncsService::onPeerAuthComplete(uint16_t connHandle, bool encrypted) {
  if (!encrypted) {
    LOG_ERR("ANCS", "Link authentication completed without encryption");
    return;
  }
  {
    ScopedLock lock;
    g_connHandle = connHandle;
    g_discoveryRetries = 0;
    g_nextDiscoveryAttemptMs = millis();
    g_needDiscovery = true;
    g_state.store(State::DiscoveringAncs);
    markDirtyLocked(millis());
  }
}

void IosAncsService::onPeerDisconnect() {
  {
    ScopedLock lock;
    if (g_running.load()) {
      g_state.store(State::Advertising);
    }
    g_connHandle = BLE_HS_CONN_HANDLE_NONE;
    ++g_connGeneration;
    g_peerCleanupNeeded = true;
    g_restartFastAdvertising.store(true);
    g_needDiscovery = false;
    g_discoveryRetries = 0;
    g_requestInFlight = false;
    g_pendingAppCount = 0;
    g_pendingCount = 0;
    g_pendingHead = 0;
    g_pendingTail = 0;
    g_actionCount = 0;
    g_actionHead = 0;
    g_actionTail = 0;
    g_parser.reset();
    g_appParser.reset();
    markDirtyLocked(millis());
  }
}

void IosAncsService::onNotificationSourceBytes(const uint8_t* data, size_t len) {
  ancs::NotificationSourceEvent ev{};
  if (!ancs::parseNotificationSourcePacket(data, len, ev)) return;

  ScopedLock lock;
  switch (ev.eventId) {
    case ancs::EventId::Added:
    case ancs::EventId::Modified:
      if (ancs::shouldFetchAttributes(ev)) {
        enqueuePendingUidLocked(ev.uid, ev.categoryId, ev.eventFlags);
      }
      break;
    case ancs::EventId::Removed:
      removeUidLocked(ev.uid);
      removePendingUidLocked(ev.uid);
      markDirtyLocked(millis());
      break;
  }
}

void IosAncsService::onDataSourceBytes(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  ScopedLock lock;
  if (!g_requestInFlight) {
    return;
  }
  if (g_inFlightKind == RequestKind::App) {
    if (g_appParser.atStart() && !ancs::isAppAttributeResponseStart(data, len, g_inFlightAppId.data())) {
      return;
    }
    g_appParser.feed(data, len, onAppParserComplete, nullptr);
    return;
  }
  // At a response boundary, only accept the header of the response we are waiting for. A late
  // fragment of a previously timed-out response would otherwise be parsed as a new header and could
  // desynchronize the parser for the real response.
  if (g_parser.phase() == ancs::AncsDataSourceParser::ParsePhase::CommandId &&
      !ancs::isAttributeResponseStart(data, len, g_inFlightMeta.uid)) {
    return;
  }
  g_parser.feed(data, len, onParserComplete, nullptr);
}

void IosAncsService::onAttributeComplete(const ancs::AncsNotification& parsed) {
  // Called with g_ancsMutex already held inside onDataSourceBytes on the ble_hs task.
  if (!g_requestInFlight || g_inFlightKind != RequestKind::Notification || g_inFlightMeta.uid != parsed.uid) {
    return;
  }
  const uint8_t metaCategory = g_inFlightMeta.categoryId;
  const uint8_t metaFlags = g_inFlightMeta.eventFlags;
  g_requestInFlight = false;

  if (parsed.title[0] == '\0' && parsed.message[0] == '\0') {
    return;
  }

  ancs::AncsNotification item = parsed;
  item.categoryId = metaCategory;
  item.eventFlags = metaFlags;
  if (const char* cached = g_appNames.find(item.appId)) {
    std::strncpy(item.appName, cached, sizeof(item.appName) - 1);
    item.appName[sizeof(item.appName) - 1] = '\0';
  } else {
    enqueueAppLocked(item.appId);
  }
  g_items.upsertFront(item);
  markDirtyLocked(millis());
}

#else  // !ENABLE_X4_DECK

IosAncsService& IosAncsService::getInstance() {
  static IosAncsService instance;
  return instance;
}

bool IosAncsService::begin(const char*) { return false; }
bool IosAncsService::end(uint32_t) { return true; }
void IosAncsService::poll() {}
IosAncsService::State IosAncsService::state() const { return State::Stopped; }
const char* IosAncsService::lastError() const { return "Bluetooth disabled in this build"; }
bool IosAncsService::isConnected() const { return false; }
bool IosAncsService::isSubscribed() const { return false; }
bool IosAncsService::consumeUiDirty() { return false; }
bool IosAncsService::getNotification(size_t, ancs::AncsNotification& out, size_t& outTotal) const {
  out.clear();
  outTotal = 0;
  return false;
}
size_t IosAncsService::notificationCount() const { return 0; }
bool IosAncsService::performAction(uint32_t, ancs::ActionId) { return false; }
bool IosAncsService::dismissNotification(uint32_t) { return false; }
void IosAncsService::onPeerConnect(uint16_t) {}
void IosAncsService::onPeerAuthComplete(uint16_t, bool) {}
void IosAncsService::onPeerDisconnect() {}
void IosAncsService::onNotificationSourceBytes(const uint8_t*, size_t) {}
void IosAncsService::onDataSourceBytes(const uint8_t*, size_t) {}
void IosAncsService::onAttributeComplete(const ancs::AncsNotification&) {}

#endif  // ENABLE_X4_DECK

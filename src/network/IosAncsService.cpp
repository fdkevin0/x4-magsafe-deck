#include "IosAncsService.h"

#include <Arduino.h>
#include <Logging.h>

#if ENABLE_X4_DECK

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <atomic>
#include <cstring>

namespace {

constexpr uint32_t kBurstCoalesceMs = 250;
constexpr uint32_t kMaxBurstDelayMs = 1200;
constexpr uint32_t kAttrRequestTimeoutMs = 2000;
constexpr uint32_t kDiscoveryRetryIntervalMs = 400;
constexpr uint8_t kMaxDiscoveryRetries = 3;
constexpr size_t kPendingQueueCap = 8;
constexpr size_t kActionQueueCap = 4;

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
bool g_peerCleanupNeeded = false;
bool g_needDiscovery = false;
uint8_t g_discoveryRetries = 0;
uint32_t g_nextDiscoveryAttemptMs = 0;

std::array<ancs::AncsNotification, IosAncsService::kMaxNotifications> g_items{};
size_t g_itemCount = 0;

std::array<PendingUidEntry, kPendingQueueCap> g_pendingUids{};
size_t g_pendingHead = 0;
size_t g_pendingTail = 0;
size_t g_pendingCount = 0;

std::array<PendingActionEntry, kActionQueueCap> g_pendingActions{};
size_t g_actionHead = 0;
size_t g_actionTail = 0;
size_t g_actionCount = 0;

bool g_requestInFlight = false;
PendingUidEntry g_inFlightMeta{};
uint32_t g_requestSentAtMs = 0;

ancs::AncsDataSourceParser g_parser{};

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

void removeUidLocked(uint32_t uid) {
  for (size_t i = 0; i < g_itemCount; ++i) {
    if (g_items[i].valid && g_items[i].uid == uid) {
      for (size_t j = i + 1; j < g_itemCount; ++j) {
        g_items[j - 1] = g_items[j];
      }
      g_items[g_itemCount - 1].clear();
      --g_itemCount;
      break;
    }
  }
}

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

  if (g_requestInFlight && g_inFlightMeta.uid == uid) {
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

void startAdvertisingAncs(const char* deviceName) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  adv->stop();

  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addData(ancs::kAncsSolicitationAd, sizeof(ancs::kAncsSolicitationAd));
  adv->setAdvertisementData(advData);

  // Place Complete Local Name in Scan Response so the primary PDU stays at 21 bytes (<= 31B limit).
  NimBLEAdvertisementData scanResp;
  scanResp.setName(deviceName ? deviceName : "X4-MagSafe");
  adv->setScanResponseData(scanResp);

  adv->start();
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
  // Request power-friendly peripheral connection parameters (30..60 ms interval, latency 4, timeout 4 s).
  g_server->updateConnParams(connHandle, 24, 48, 4, 400);
  return true;
}

}  // namespace

IosAncsService& IosAncsService::getInstance() {
  static IosAncsService instance;
  return instance;
}

bool IosAncsService::begin(const char* deviceName) {
  if (g_running.load()) return true;

  if (!g_ancsMutex) {
    g_ancsMutex = xSemaphoreCreateMutexStatic(&g_ancsMutexStorage);
  }

  if (NimBLEDevice::isInitialized()) {
    cleanupClientOnOwnerThread();
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!NimBLEDevice::init(deviceName ? deviceName : "X4-MagSafe")) {
    LOG_ERR("ANCS", "NimBLEDevice::init failed");
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
    LOG_ERR("ANCS", "createServer failed");
    NimBLEDevice::deinit(true);
    return false;
  }
  g_server->setCallbacks(&g_serverCallbacks);
  g_server->advertiseOnDisconnect(true);

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
  startAdvertisingAncs(deviceName);

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
  bool doCleanup = false;
  bool doDiscovery = false;

  {
    ScopedLock lock;
    if (g_peerCleanupNeeded) {
      g_peerCleanupNeeded = false;
      doCleanup = true;
    }
    handle = g_connHandle;
    if (g_needDiscovery && handle != BLE_HS_CONN_HANDLE_NONE &&
        static_cast<int32_t>(now - g_nextDiscoveryAttemptMs) >= 0) {
      g_needDiscovery = false;
      doDiscovery = true;
    }
  }

  if (doCleanup) {
    cleanupClientOnOwnerThread();
  }

  if (doDiscovery && handle != BLE_HS_CONN_HANDLE_NONE) {
    if (discoverAndSubscribeAncs(handle)) {
      g_state.store(State::SubscribedReady);
      ScopedLock lock;
      g_discoveryRetries = 0;
      markDirtyLocked(millis());
      LOG_INF("ANCS", "Subscribed to iOS ANCS");
    } else {
      bool shouldDisconnect = false;
      {
        ScopedLock lock;
        if (g_connHandle == handle) {
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

  // 2. Check timeout on in-flight GetNotificationAttributes request and send next queued UID.
  bool needSend = false;
  PendingUidEntry nextReq{};
  {
    ScopedLock lock;
    if (g_connHandle == BLE_HS_CONN_HANDLE_NONE) return;
    if (g_requestInFlight && (now - g_requestSentAtMs) > kAttrRequestTimeoutMs) {
      g_requestInFlight = false;
      g_parser.reset();
    }
    if (!g_requestInFlight && popPendingUidLocked(nextReq)) {
      g_requestInFlight = true;
      g_inFlightMeta = nextReq;
      g_requestSentAtMs = now;
      g_parser.reset();
      needSend = true;
    }
  }

  if (needSend && g_ctrlPointChr) {
    uint8_t pkt[ancs::kGetAttributesRequestSize];
    const size_t len = ancs::buildGetNotificationAttributesPacket(nextReq.uid, pkt);
    if (!g_ctrlPointChr->writeValue(pkt, len, true)) {
      ScopedLock lock;
      g_requestInFlight = false;
    }
  }
}

IosAncsService::State IosAncsService::state() const { return g_state.load(); }

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
  outTotal = g_itemCount;
  if (g_itemCount > 0) {
    const size_t clamped = std::min(index, g_itemCount - 1);
    out = g_items[clamped];
    ok = true;
  } else {
    out.clear();
  }
  return ok;
}

size_t IosAncsService::notificationCount() const {
  ScopedLock lock;
  return g_itemCount;
}

bool IosAncsService::performAction(uint32_t uid, ancs::ActionId action) {
  ScopedLock lock;
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
    g_peerCleanupNeeded = true;
    g_needDiscovery = false;
    g_discoveryRetries = 0;
    markDirtyLocked(millis());
  }

  g_state.store(State::ConnectedAuthenticating);
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
    markDirtyLocked(millis());
  }

  g_state.store(State::DiscoveringAncs);
}

void IosAncsService::onPeerDisconnect() {
  {
    ScopedLock lock;
    g_connHandle = BLE_HS_CONN_HANDLE_NONE;
    g_peerCleanupNeeded = true;
    g_needDiscovery = false;
    g_discoveryRetries = 0;
    g_requestInFlight = false;
    g_pendingCount = 0;
    g_pendingHead = 0;
    g_pendingTail = 0;
    g_actionCount = 0;
    g_actionHead = 0;
    g_actionTail = 0;
    g_parser.reset();
    markDirtyLocked(millis());
  }

  if (g_running.load()) {
    g_state.store(State::Advertising);
  }
}

void IosAncsService::onNotificationSourceBytes(const uint8_t* data, size_t len) {
  ancs::NotificationSourceEvent ev{};
  if (!ancs::parseNotificationSourcePacket(data, len, ev)) return;

  if ((ev.eventFlags & ancs::kEventFlagSilent) != 0 && ev.eventId == ancs::EventId::Added) {
    return;
  }

  ScopedLock lock;
  switch (ev.eventId) {
    case ancs::EventId::Added:
    case ancs::EventId::Modified:
      enqueuePendingUidLocked(ev.uid, ev.categoryId, ev.eventFlags);
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
  g_parser.feed(data, len, onParserComplete, nullptr);
}

void IosAncsService::onAttributeComplete(const ancs::AncsNotification& parsed) {
  // Called with g_ancsMutex already held inside onDataSourceBytes on the ble_hs task.
  if (!g_requestInFlight || g_inFlightMeta.uid != parsed.uid) {
    return;
  }
  const uint8_t metaCategory = g_inFlightMeta.categoryId;
  const uint8_t metaFlags = g_inFlightMeta.eventFlags;
  g_requestInFlight = false;

  if (parsed.title[0] == '\0' && parsed.message[0] == '\0') {
    return;
  }

  removeUidLocked(parsed.uid);
  const size_t shiftCount = std::min<size_t>(g_itemCount, kMaxNotifications - 1);
  for (size_t i = shiftCount; i > 0; --i) {
    g_items[i] = g_items[i - 1];
  }
  g_items[0] = parsed;
  g_items[0].categoryId = metaCategory;
  g_items[0].eventFlags = metaFlags;
  g_itemCount = shiftCount + 1;
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

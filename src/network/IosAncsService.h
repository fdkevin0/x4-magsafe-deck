#pragma once

#include <cstddef>
#include <cstdint>

#include "AncsProtocol.h"

class IosAncsService {
 public:
  static constexpr size_t kMaxNotifications = 8;

  enum class State : uint8_t {
    Stopped,
    Advertising,
    ConnectedAuthenticating,
    DiscoveringAncs,
    SubscribedReady,
  };

  static IosAncsService& getInstance();

  bool begin(const char* deviceName = "X4-MagSafe");
  bool end(uint32_t timeoutMs = 2000);

  // Advances ANCS GATT discovery, attribute requests, and action dispatch on the caller's thread.
  void poll();

  State state() const;
  bool isConnected() const;
  bool isSubscribed() const;

  // Calls poll() and returns true once when the notification list or connection state has
  // changed and either 250 ms have elapsed since the last event or 1200 ms since the burst began.
  bool consumeUiDirty();

  // Fetch a single notification at `index` (clamped if needed) and total active count.
  bool getNotification(size_t index, ancs::AncsNotification& out, size_t& outTotal) const;
  size_t notificationCount() const;

  // Queue an ANCS PerformNotificationAction (Positive or Negative) for `uid` and remove it locally.
  bool performAction(uint32_t uid, ancs::ActionId action);
  bool dismissNotification(uint32_t uid);

  // Internal callbacks invoked by NimBLE host task.
  void onPeerConnect(uint16_t connHandle);
  void onPeerAuthComplete(uint16_t connHandle, bool encrypted);
  void onPeerDisconnect();
  void onNotificationSourceBytes(const uint8_t* data, size_t len);
  void onDataSourceBytes(const uint8_t* data, size_t len);
  void onAttributeComplete(const ancs::AncsNotification& item);

 private:
  IosAncsService() = default;
};

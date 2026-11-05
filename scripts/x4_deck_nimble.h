#pragma once

#include <sdkconfig.h>

// Force-included (-include) in [env:x4_deck] so NimBLE-Arduino (which is compiled
// from source as a PlatformIO library while CONFIG_ARDUINO_SELECTIVE_BLE=n disables
// the Arduino core's BLE wrapper) enables Peripheral + Broadcaster + GATT Server,
// a 185-byte ATT MTU, and expanded MSYS/ACL buffers for iOS ANCS notifications,
// while keeping custom_sdkconfig identical to crosspoint-reader so pioarduino's
// shared esp32c3 framework cache is never invalidated between app0 and app1 builds.

#undef CONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED
#undef CONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED

#undef CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL 1

#undef CONFIG_BT_NIMBLE_ROLE_BROADCASTER
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER 1

#undef CONFIG_BT_NIMBLE_GATT_SERVER
#define CONFIG_BT_NIMBLE_GATT_SERVER 1

#undef CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU
#define CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 185

#undef CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT
#define CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT 24

#undef CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT
#define CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT 24

#undef CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT
#define CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT 16

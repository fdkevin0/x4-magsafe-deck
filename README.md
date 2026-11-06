# X4 MagSafe Deck (`x4-magsafe-deck`)

Standalone companion firmware for the **XTEINK X4** (ESP32-C3, 380 KB SRAM, 16 MB Dual-OTA Flash, 800×480 E-Ink, 7 tactile buttons, MagSafe magnetic body).

By leveraging the X4's 16 MB dual-OTA partition table (`app0` @ `0x010000` + `app1` @ `0x650000`), your device keeps **CrossPoint Reader** in `app0` for distraction-free EPUB reading, while booting **X4 MagSafe Deck** from `app1` when holding **`Left + Power`** at power-on (or via **Settings → General → Switch Boot Partition**).

---

## Core Capabilities

1. **Dual-Partition Boot Switcher (`otadata` Flip + Inactive Slot Verification)**
   - Holding **`Left + Power`** during cold boot (or pressing **`Back`** inside MagSafe Deck) verifies the inactive OTA partition (`esp_image_header_t` magic `0xE9`, 4-byte aligned segments, `CROSSPOINT-BOARD-V1:<board>;` board-identity tag, and full-image XOR checksum + optional SHA-256 tail) before writing a validated `ESP_OTA_IMG_VALID` entry to the alternate `otadata` sector (`ota_boot::switchTo()`).
   - Preserves the known-good `otadata` sector mapping to the currently running partition and prevents bricking on an erased, half-written, or wrong-board `app1` / `app0` partition.

2. **iOS BLE ANCS Notification Board (No Companion iOS App Required)**
   - Runs `NimBLE-Arduino@2.5.1` in simultaneous **BLE Peripheral + GATT Client** mode.
   - Advertises 128-bit **ANCS Service Solicitation** (`7905F431-B5CE-4E99-A40F-4B1E122D00D0`) as **`X4-MagSafe`** with iOS LE Secure Connections + IRK bonding so iOS pairs natively from **Settings → Bluetooth → X4-MagSafe**.
   - Streams live notifications into a fixed-capacity ring buffer with burst coalescing (`250 ms` quiet window, `1200 ms` max burst delay) and anti-ghosting E-Ink refresh (`FAST_REFRESH` for normal updates, periodic `HALF_REFRESH` every 15 partial refreshes).
   - **Positive / Negative Action Dispatch**: Press **`Confirm`** to dismiss a notification (`ActionId::Negative`), or press **`Power`** when `kEventFlagPositiveAction` is set to trigger the notification's positive action (`ActionId::Positive`).

3. **MagSafe Zero-Power Badge / Boarding Pass / Card Vault**
   - Loads `/badges/cards.txt`, `/badges/*.json`, or `/badges/*.bmp` from the MicroSD card (skipping hidden `.*` / macOS `._*` files and falling back to built-in default passes when no SD card is inserted).
   - Renders high-contrast QR codes (`ricmoo/QRCode`, auto-selecting Byte-mode QR versions 4–40) + pass metadata (`Tag`, `Title`, `Subtitle`, `Meta`).
   - **Zero-Power Lock (`Lock Pass & Sleep`)**: Press **`Confirm`** on the Badge Vault tab (or after 20 minutes of inactivity) to perform a clean `FULL_REFRESH` and immediately enter ESP32-C3 deep sleep while retaining the E-Ink image at **0 W** power consumption—keeping boarding passes, train tickets, and contact QR badges scannable even if the battery is completely depleted.

4. **Shared CJK / Non-Latin Font Library with CrossPoint Reader**
   - Automatically discovers and shares the MicroSD font directories (`/.fonts` and `/fonts`) and user font selection (`/.crosspoint/settings.json` → `"sdFontFamilyName"`) with **CrossPoint Reader** (`app0`).
   - Supports both pre-rasterized **`.cpfont`** families (`SdCardFont`) and streamed **`.ttf` / `.otf` / `.ttc`** vector fonts (`TtfEpdFont`), registering size-matched CJK fallbacks for `UI_10_FONT_ID` and `UI_12_FONT_ID` so Chinese/Japanese/Korean iOS notifications and MagSafe badges render natively without duplicating font files on the SD card.

---

## Repository Layout

```text
x4-magsafe-deck/
├── platformio.ini                          # Standalone lightweight [env:x4_deck] build config
├── partitions.csv                          # X4 16 MB Dual-OTA partition table (app0 + app1)
├── LICENSE                                 # MIT License
├── freeink-sdk/                            # Board definitions & E-Ink panel drivers submodule
├── lib/                                    # Shared display, font, UTF-8, and X4 HAL libraries
├── src/
│   ├── main.cpp                            # Standalone MagSafe Deck entry point & UI loop
│   ├── fontIds.h                           # Built-in UI font IDs (UI_10, UI_12)
│   ├── network/
│   │   ├── AncsProtocol.h                  # Pure C++17 ANCS notification/attribute parser
│   │   ├── IosAncsService.{h,cpp}          # NimBLE ANCS Peripheral + GATT Client service
│   │   ├── MagSafeBadgeStore.h             # Zero-heap SD card badge/pass loader
│   │   ├── FirmwareBoardTag.{h,cpp}        # Streaming CROSSPOINT-BOARD-V1:<board>; tag scanner
│   │   ├── FirmwareImageVerify.h           # Streaming OTA partition checksum & SHA-256 verifier
│   │   ├── OtaBootSwitch.{h,cpp}           # Safe otadata dual-partition boot switcher
│   │   ├── SharedFontLoader.{h,cpp}        # Shared CrossPoint SD card CJK/.cpfont/.ttf fallback loader
│   │   └── FirmwareFlasher.{h,cpp}         # SD-card firmware.bin -> inactive OTA slot flasher
│   ├── platform/
│   │   ├── UsbSerialJtagHandoff.{h,cpp}    # ESP32-C3 USB Serial/JTAG boot handoff
│   │   └── skip_efuse_blk_check.c          # Linker wrapper for X4 efuse block revision check
│   └── util/
│       ├── QrUtils.{h,cpp}                 # Byte-mode auto-version E-Ink QR code renderer
│       └── StringUtils.h                   # Case-insensitive ASCII comparison helper
├── patches/
│   └── crosspoint-dualboot-switcher.patch  # Minimal patch adding dual-boot switching to CrossPoint Reader
├── scripts/
│   ├── package_dualboot.py                 # Pull-and-build packager for app0 + app1 + 16 MB factory image
│   ├── flash_app1.py                       # Flash app1 (0x650000), both slots, or 16 MB factory image
│   ├── test_qemu_dualboot.py               # Automated Espressif QEMU ESP32-C3 dual-boot test
│   ├── x4_deck_nimble.h                    # NimBLE Peripheral + 185-byte MTU + MSYS pool overrides
│   ├── patch_pioarduino_cache.py           # Shared pioarduino ESP32-C3 SDK cache hook
│   ├── patch_sdfat.py                      # SdFat FAT/data cache patcher
│   ├── patch_arduino_rom_libc.py           # ESP32-C3 ROM libc patcher
│   └── patch_arduino_ble_controller.py     # ESP32-C3 BLE controller flash-only mode patcher
└── test/
    ├── CMakeLists.txt
    ├── mbedtls/sha256.h                    # Self-contained SHA-256 implementation for host tests
    └── AncsDeckTest.cpp                    # Host GoogleTest suite (ANCS, Badges, BoardTag, ImageVerify, UTF-8)
```

---

## Build, Test & Package

### 1. Build Standalone `x4-magsafe-deck` (`app1`)

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
pio run -e x4_deck
```

### 2. Run Host Unit Tests (GoogleTest)

```bash
cmake -B build/test -S test -DCMAKE_BUILD_TYPE=Debug
cmake --build build/test -j
./build/test/AncsDeckTest
```

### 3. Pull-and-Build Dual-Boot Package (`app0` + `app1` + 16 MB Factory Image)

```bash
python3 scripts/package_dualboot.py --reader-dir /path/to/crosspoint-reader
```

This generates in `dist/`:
- `dist/x4-magsafe-deck-app1.bin` — Standalone MagSafe Deck image (`0x650000`)
- `dist/x4-crosspoint-reader-app0.bin` — Dual-boot-enabled CrossPoint Reader (`0x010000`)
- `dist/x4-dualboot-factory-16mb.bin` — Merged 16 MB dual-OTA factory image (`0x000000`)
- `dist/manifest.json` — Sizes, slot usage percentages, and SHA-256 hashes

### 4. Verify Dual-Boot in Espressif QEMU (`esp32c3`)

```bash
python3 scripts/test_qemu_dualboot.py
```

---

## Flashing to XTEINK X4 Hardware

### Option A: Flash only `app1` (`0x650000`) via USB-C (`esptool`)
Preserves your existing `app0` CrossPoint Reader and NVS settings:

```bash
python3 scripts/flash_app1.py --mode app1 --port /dev/ttyACM0
```

### Option B: Flash Both `app0` (`0x010000`) + `app1` (`0x650000`) via USB-C (`esptool`)

```bash
python3 scripts/flash_app1.py --mode both --port /dev/ttyACM0
```

### Option C: Flash Complete 16 MB Dual-Boot Factory Image (`0x000000`)

```bash
python3 scripts/flash_app1.py --mode factory --port /dev/ttyACM0
```

### Option D: SD Card Offline Slot Install (`firmware.bin`)
1. Copy `dist/x4-magsafe-deck-app1.bin` to the root of your MicroSD card as `/firmware.bin`.
2. In CrossPoint Reader (running on `app0`), open **Settings → General → Update from SD Card** to flash `/firmware.bin` into the inactive `app1` slot.
3. Hold **`Left + Power`** at boot (or use **Settings → General → Switch Boot Partition** in CrossPoint Reader, or press **`Back`** inside MagSafe Deck) to switch between `app0` and `app1`.

---

## SD Card `/badges/` Format

Create `/badges/cards.txt` on the MicroSD card using 5-field (`Tag|Title|Subtitle|Meta|QRPayload`), 4-field (`Tag|Title|Subtitle|QRPayload`), 3-field (`Tag|Title|QRPayload`), or 2-field (`Title|QRPayload`) lines:

```text
BOARDING PASS|MU5101 SHA -> PEK|GATE B12  SEAT 32A|BOARDING 14:20  ZONE 1|BC1/FDKEVIN/MU5101/SHA/PEK/32A
DIGITAL CARD|XTEINK X4 MAGSAFE DECK|BLE ANCS + ZERO-POWER VAULT|https://crosspointreader.com
EMERGENCY ICE|IF FOUND PLEASE CONTACT|REWARD IF RETURNED|tel:+8613800000000
Quick WiFi|WIFI:S:Guest;T:WPA;P:secret;;
```

Or place individual `/badges/*.json` files (`{"tag":"...","title":"...","subtitle":"...","meta":"...","qr":"..."}`; `"detail"` is also accepted as an alias for `"meta"`) or `/badges/*.bmp` BMP images.

---

## License

Released under the [MIT License](LICENSE).

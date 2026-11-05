#!/usr/bin/env python3
"""
Pull-and-Build / Dual-Boot Packager for XTEINK X4 (16 MB Dual-OTA Flash)

Produces in dist/:
  1. x4-magsafe-deck-app1.bin        - Standalone MagSafe Deck firmware (app1 @ 0x650000)
  2. x4-crosspoint-reader-app0.bin   - Dual-boot-enabled CrossPoint Reader (app0 @ 0x10000)
  3. x4-dualboot-factory-16mb.bin    - Complete 16 MB dual-OTA factory flash image (0x000000..0xFFFFFF)
  4. manifest.json                   - SHA-256 digests, sizes, and flash offsets
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

FLASH_SIZE_BYTES = 16 * 1024 * 1024
OFFSET_BOOTLOADER = 0x000000
OFFSET_PARTITIONS = 0x008000
OFFSET_OTADATA = 0x00E000
OFFSET_APP0 = 0x010000
OFFSET_APP1 = 0x650000
MAX_OTA_SLOT_BYTES = 0x640000  # 6,553,600 bytes per OTA slot


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def build_otadata_entry(seq: int) -> bytes:
    """Build a 32-byte esp_ota_select_entry_t (ESP_OTA_IMG_VALID = 2) with ESP32 ROM CRC32."""
    seq_bytes = struct.pack("<I", seq)
    crc = zlib.crc32(seq_bytes, 0xFFFFFFFF) & 0xFFFFFFFF
    return seq_bytes + (b"\xff" * 20) + struct.pack("<I", 2) + struct.pack("<I", crc)


def find_pio() -> str:
    penv_pio = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if penv_pio.is_file():
        return str(penv_pio)
    which_pio = shutil.which("pio")
    if which_pio:
        return which_pio
    raise RuntimeError("PlatformIO ('pio') not found. Expected ~/.platformio/penv/bin/pio")


def run_cmd(cmd: list[str], cwd: Path) -> None:
    env = os.environ.copy()
    penv_bin = str(Path.home() / ".platformio" / "penv" / "bin")
    env["PATH"] = f"{penv_bin}:{env.get('PATH', '')}"
    print(f"  $ (cd {cwd} && {' '.join(cmd)})")
    subprocess.run(cmd, cwd=str(cwd), env=env, check=True)


def ensure_reader_repo(repo_root: Path, reader_dir_arg: str | None, repo_url: str) -> Path:
    if reader_dir_arg:
        reader_dir = Path(reader_dir_arg).expanduser().resolve()
        if not reader_dir.is_dir():
            raise FileNotFoundError(f"Specified --reader-dir does not exist: {reader_dir}")
        return reader_dir

    env_dir = os.environ.get("CROSSPOINT_READER_DIR")
    if env_dir:
        reader_dir = Path(env_dir).expanduser().resolve()
        if reader_dir.is_dir():
            return reader_dir

    cache_dir = repo_root / ".cache" / "crosspoint-reader"
    if not cache_dir.is_dir():
        cache_dir.parent.mkdir(parents=True, exist_ok=True)
        run_cmd(["git", "clone", "--recurse-submodules", repo_url, str(cache_dir)], repo_root)
    return cache_dir


def sync_dualboot_files_and_patch(repo_root: Path, reader_dir: Path, patch_path: Path) -> None:
    # Sync shared OTA verification & boot-switcher implementation into reader_dir/src/network/
    src_net = repo_root / "src" / "network"
    dst_net = reader_dir / "src" / "network"
    dst_net.mkdir(parents=True, exist_ok=True)
    for fname in (
        "FirmwareBoardTag.h",
        "FirmwareBoardTag.cpp",
        "FirmwareImageVerify.h",
        "FirmwareFlasher.h",
        "FirmwareFlasher.cpp",
        "OtaBootSwitch.h",
        "OtaBootSwitch.cpp",
    ):
        shutil.copy2(src_net / fname, dst_net / fname)

    if not patch_path.is_file():
        return
    settings_cpp = reader_dir / "src" / "activities" / "settings" / "SettingsActivity.cpp"
    if settings_cpp.is_file() and "SwitchFirmwareSlot" in settings_cpp.read_text(encoding="utf-8"):
        print(f"[patch] Dual-boot switcher already present in {reader_dir}")
        return
    check = subprocess.run(
        ["git", "apply", "--check", str(patch_path)],
        cwd=str(reader_dir),
        capture_output=True,
        text=True,
    )
    if check.returncode == 0:
        print(f"[patch] Applying {patch_path.name} to {reader_dir}")
        run_cmd(["git", "apply", str(patch_path)], reader_dir)
    else:
        print(f"[patch] Skipping git apply ({check.stderr.strip()})")


def main() -> int:
    parser = argparse.ArgumentParser(description="Package XTEINK X4 Dual-Boot Firmware (app0 + app1)")
    parser.add_argument(
        "--reader-dir",
        default=None,
        help="Path to local crosspoint-reader checkout (or set CROSSPOINT_READER_DIR)",
    )
    parser.add_argument(
        "--reader-url",
        default="https://github.com/crosspoint-reader/crosspoint-reader.git",
        help="Git URL if cloning crosspoint-reader into .cache/crosspoint-reader",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Package existing .pio/build artifacts without invoking pio run",
    )
    parser.add_argument(
        "--default-slot",
        choices=["app0", "app1"],
        default="app0",
        help="Initial active OTA slot in the 16 MB factory image (default: app0)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    dist_dir = repo_root / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)

    reader_dir = ensure_reader_repo(repo_root, args.reader_dir, args.reader_url)
    patch_path = repo_root / "patches" / "crosspoint-dualboot-switcher.patch"
    if not args.skip_build:
        sync_dualboot_files_and_patch(repo_root, reader_dir, patch_path)

    if not args.skip_build:
        pio = find_pio()
        print("[1/3] Building standalone MagSafe Deck firmware (x4_deck -> app1)...")
        run_cmd([pio, "run", "-e", "x4_deck"], repo_root)
        print("[2/3] Building CrossPoint Reader firmware (default -> app0)...")
        run_cmd([pio, "run", "-e", "default"], reader_dir)
    else:
        print("[1/3] Using prebuilt .pio/build artifacts (--skip-build)...")

    deck_build = repo_root / ".pio" / "build" / "x4_deck"
    reader_build = reader_dir / ".pio" / "build" / "default"

    app1_bin = deck_build / "firmware.bin"
    app0_bin = reader_build / "firmware.bin"
    bootloader_bin = reader_build / "bootloader.bin"
    partitions_bin = reader_build / "partitions.bin"
    if not bootloader_bin.is_file():
        bootloader_bin = deck_build / "bootloader.bin"
    if not partitions_bin.is_file():
        partitions_bin = deck_build / "partitions.bin"

    for label, path in [
        ("app1 (x4_deck)", app1_bin),
        ("app0 (crosspoint-reader)", app0_bin),
        ("bootloader.bin", bootloader_bin),
        ("partitions.bin", partitions_bin),
    ]:
        if not path.is_file():
            raise FileNotFoundError(f"Missing required artifact {label}: {path}")

    app0_size = app0_bin.stat().st_size
    app1_size = app1_bin.stat().st_size
    if app0_size > MAX_OTA_SLOT_BYTES:
        raise ValueError(f"app0 size {app0_size} exceeds slot capacity {MAX_OTA_SLOT_BYTES}")
    if app1_size > MAX_OTA_SLOT_BYTES:
        raise ValueError(f"app1 size {app1_size} exceeds slot capacity {MAX_OTA_SLOT_BYTES}")

    out_app1 = dist_dir / "x4-magsafe-deck-app1.bin"
    out_app0 = dist_dir / "x4-crosspoint-reader-app0.bin"
    out_factory = dist_dir / "x4-dualboot-factory-16mb.bin"

    shutil.copy2(app1_bin, out_app1)
    shutil.copy2(app0_bin, out_app0)

    print("[3/3] Assembling 16 MB dual-OTA factory image...")
    flash = bytearray(b"\xff" * FLASH_SIZE_BYTES)

    boot_bytes = bootloader_bin.read_bytes()
    part_bytes = partitions_bin.read_bytes()
    app0_bytes = out_app0.read_bytes()
    app1_bytes = out_app1.read_bytes()

    flash[OFFSET_BOOTLOADER : OFFSET_BOOTLOADER + len(boot_bytes)] = boot_bytes
    flash[OFFSET_PARTITIONS : OFFSET_PARTITIONS + len(part_bytes)] = part_bytes

    # Write otadata at 0x00E000 (slot 0) and 0x00F000 (slot 1)
    entry_seq1 = build_otadata_entry(1)
    entry_seq2 = build_otadata_entry(2)
    if args.default_slot == "app0":
        flash[OFFSET_OTADATA : OFFSET_OTADATA + 32] = entry_seq1
    else:
        flash[OFFSET_OTADATA : OFFSET_OTADATA + 32] = entry_seq1
        flash[OFFSET_OTADATA + 0x1000 : OFFSET_OTADATA + 0x1000 + 32] = entry_seq2

    flash[OFFSET_APP0 : OFFSET_APP0 + len(app0_bytes)] = app0_bytes
    flash[OFFSET_APP1 : OFFSET_APP1 + len(app1_bytes)] = app1_bytes

    out_factory.write_bytes(flash)

    manifest = {
        "board": "esp32-c3-devkitm-1 (XTEINK X4 16MB Dual-OTA)",
        "default_slot": args.default_slot,
        "artifacts": {
            "x4-magsafe-deck-app1.bin": {
                "offset": f"0x{OFFSET_APP1:06x}",
                "size_bytes": app1_size,
                "slot_usage_percent": round(app1_size * 100.0 / MAX_OTA_SLOT_BYTES, 2),
                "sha256": sha256_file(out_app1),
            },
            "x4-crosspoint-reader-app0.bin": {
                "offset": f"0x{OFFSET_APP0:06x}",
                "size_bytes": app0_size,
                "slot_usage_percent": round(app0_size * 100.0 / MAX_OTA_SLOT_BYTES, 2),
                "sha256": sha256_file(out_app0),
            },
            "x4-dualboot-factory-16mb.bin": {
                "offset": "0x000000",
                "size_bytes": FLASH_SIZE_BYTES,
                "sha256": sha256_file(out_factory),
            },
        },
    }
    manifest_path = dist_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print("\n=== Dual-Boot Packaging Summary ===")
    print(
        f"  app0 (CrossPoint Reader) : {out_app0.name} "
        f"({app0_size:,} B / {manifest['artifacts']['x4-crosspoint-reader-app0.bin']['slot_usage_percent']}% of 6.25 MB slot)"
    )
    print(
        f"  app1 (X4 MagSafe Deck)   : {out_app1.name} "
        f"({app1_size:,} B / {manifest['artifacts']['x4-magsafe-deck-app1.bin']['slot_usage_percent']}% of 6.25 MB slot)"
    )
    print(f"  16 MB Dual-Boot Factory  : {out_factory.name} ({FLASH_SIZE_BYTES:,} B)")
    print(f"  Manifest                 : {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Flash X4 MagSafe Deck firmware into the app1 partition (0x650000) or flash the
full 16 MB dual-boot factory image without overwriting NVS when updating app1.
"""

import argparse
import subprocess
import sys
from pathlib import Path

OFFSET_APP1 = "0x650000"
OFFSET_APP0 = "0x010000"
OFFSET_FACTORY = "0x000000"


def find_esptool() -> list[str]:
    penv_esptool = Path.home() / ".platformio" / "penv" / "bin" / "esptool.py"
    if penv_esptool.is_file():
        return [sys.executable, str(penv_esptool)]
    pkg_esptool = (
        Path.home()
        / ".platformio"
        / "packages"
        / "tool-esptoolpy"
        / "esptool.py"
    )
    if pkg_esptool.is_file():
        return [sys.executable, str(pkg_esptool)]
    return ["esptool.py"]


def main() -> int:
    parser = argparse.ArgumentParser(description="Flash XTEINK X4 Dual-Boot partitions via esptool")
    parser.add_argument("--port", "-p", default=None, help="Serial port (e.g. /dev/ttyACM0)")
    parser.add_argument("--baud", "-b", default="921600", help="Baud rate (default: 921600)")
    parser.add_argument(
        "--mode",
        choices=["app1", "both", "factory"],
        default="app1",
        help=(
            "app1: flash only x4-magsafe-deck-app1.bin to 0x650000; "
            "both: flash app0 (0x10000) + app1 (0x650000); "
            "factory: flash full 16 MB image at 0x000000"
        ),
    )
    parser.add_argument("--dry-run", action="store_true", help="Print the esptool command without executing")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    dist_dir = repo_root / "dist"
    app1_bin = dist_dir / "x4-magsafe-deck-app1.bin"
    app0_bin = dist_dir / "x4-crosspoint-reader-app0.bin"
    factory_bin = dist_dir / "x4-dualboot-factory-16mb.bin"

    cmd = find_esptool() + ["--chip", "esp32c3", "--baud", str(args.baud)]
    if args.port:
        cmd += ["--port", args.port]
    cmd += [
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "-z",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "16MB",
    ]

    if args.mode == "app1":
        if not app1_bin.is_file():
            raise FileNotFoundError(f"Missing {app1_bin}. Run scripts/package_dualboot.py first.")
        cmd += [OFFSET_APP1, str(app1_bin)]
    elif args.mode == "both":
        for p in (app0_bin, app1_bin):
            if not p.is_file():
                raise FileNotFoundError(f"Missing {p}. Run scripts/package_dualboot.py first.")
        cmd += [OFFSET_APP0, str(app0_bin), OFFSET_APP1, str(app1_bin)]
    else:
        if not factory_bin.is_file():
            raise FileNotFoundError(f"Missing {factory_bin}. Run scripts/package_dualboot.py first.")
        cmd += [OFFSET_FACTORY, str(factory_bin)]

    print("Command:", " ".join(cmd))
    if args.dry_run:
        return 0
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())

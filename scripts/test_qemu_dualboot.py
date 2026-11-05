#!/usr/bin/env python3
"""
Automated QEMU ESP32-C3 Dual-Boot Verification for dist/x4-dualboot-factory-16mb.bin.

Uses Espressif's qemu-system-riscv32 (-machine esp32c3) + riscv32-esp-elf-gdb to
inspect the live running CPU state, partition table, and mapped DROM:
  1. Initial boot (default otadata) boots app0 (0x010000, label="app0")
     and confirms esp_ota_get_next_update_partition(0) points to app1 (0x650000).
  2. Flipping otadata to seq=2 boots app1 (0x650000, label="app1", project="x4-magsafe-deck")
     and confirms esp_ota_get_next_update_partition(0) points to app0 (0x010000).
  3. Flipping otadata to seq=3 returns cleanly to app0 (0x010000, label="app0").
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from package_dualboot import OFFSET_OTADATA, build_otadata_entry


def find_qemu() -> Path:
    candidates = [
        Path.home()
        / ".espressif/tools/qemu-riscv32/esp_develop_9.2.2_20260417/qemu/bin/qemu-system-riscv32",
    ]
    for c in candidates:
        if c.is_file():
            return c
    which = shutil.which("qemu-system-riscv32")
    if which:
        return Path(which)
    raise FileNotFoundError("qemu-system-riscv32 not found")


def find_gdb() -> Path:
    candidates = [
        Path.home() / ".platformio/packages/tool-riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb",
        Path.home() / ".platformio/tools/tool-riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb",
    ]
    for c in candidates:
        if c.is_file():
            return c
    which = shutil.which("riscv32-esp-elf-gdb")
    if which:
        return Path(which)
    raise FileNotFoundError("riscv32-esp-elf-gdb not found")


def run_qemu_gdb_session(
    qemu_bin: Path,
    gdb_bin: Path,
    flash_img: Path,
    elf_path: Path,
    gdb_port: int,
) -> str:
    env = os.environ.copy()
    slirp_env = os.environ.get("QEMU_SLIRP_LIB_DIR")
    if slirp_env and Path(slirp_env).is_dir():
        env["LD_LIBRARY_PATH"] = f"{slirp_env}:{env.get('LD_LIBRARY_PATH', '')}"

    serial_log = flash_img.parent / "serial.log"
    qemu_proc = subprocess.Popen(
        [
            str(qemu_bin),
            "-display",
            "none",
            "-machine",
            "esp32c3",
            "-drive",
            f"file={flash_img},if=mtd,format=raw",
            "-serial",
            f"file:{serial_log}",
            "-gdb",
            f"tcp::{gdb_port}",
        ],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.45)
        gdb_cmds = [
            "-batch",
            str(elf_path),
            "-ex",
            f"target remote :{gdb_port}",
            "-ex",
            "set pagination off",
            "-ex",
            "print/x esp_ota_get_running_partition()->address",
            "-ex",
            "print esp_ota_get_running_partition()->label",
            "-ex",
            "print (char*)&esp_app_desc + 48",
            "-ex",
            "print/x esp_ota_get_next_update_partition(0)->address",
            "-ex",
            "print esp_ota_get_next_update_partition(0)->label",
        ]
        res = subprocess.run(
            [str(gdb_bin)] + gdb_cmds,
            capture_output=True,
            text=True,
            timeout=10,
        )
        return res.stdout + "\n" + res.stderr
    finally:
        qemu_proc.terminate()
        try:
            qemu_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            qemu_proc.kill()


def parse_gdb_results(out: str) -> dict[str, str]:
    addr_m = re.search(r"\$1\s*=\s*(0x[0-9a-fA-F]+)", out)
    label_m = re.search(r'\$2\s*=\s*"([^"]+)"', out)
    proj_m = re.search(r'\$3\s*=\s*0x[0-9a-fA-F]+\s*<[^>]+>\s*"([^"]+)"', out)
    next_addr_m = re.search(r"\$4\s*=\s*(0x[0-9a-fA-F]+)", out)
    next_label_m = re.search(r'\$5\s*=\s*"([^"]+)"', out)
    return {
        "address": addr_m.group(1) if addr_m else "",
        "label": label_m.group(1) if label_m else "",
        "project": proj_m.group(1) if proj_m else "",
        "next_address": next_addr_m.group(1) if next_addr_m else "",
        "next_label": next_label_m.group(1) if next_label_m else "",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify 16 MB dual-boot factory image in Espressif QEMU")
    parser.add_argument(
        "--app0-elf",
        default=None,
        help="Path to app0 firmware.elf (defaults to CROSSPOINT_READER_DIR or .cache/crosspoint-reader)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    factory_img = repo_root / "dist" / "x4-dualboot-factory-16mb.bin"
    if not factory_img.is_file():
        raise FileNotFoundError(f"Missing {factory_img}. Run scripts/package_dualboot.py first.")

    app1_elf = repo_root / ".pio" / "build" / "x4_deck" / "firmware.elf"
    app0_elf_candidates: list[Path] = []
    if args.app0_elf:
        app0_elf_candidates.append(Path(args.app0_elf).expanduser().resolve())
    reader_env = os.environ.get("CROSSPOINT_READER_DIR")
    if reader_env:
        app0_elf_candidates.append(Path(reader_env).expanduser().resolve() / ".pio" / "build" / "default" / "firmware.elf")
    app0_elf_candidates.append(
        repo_root / ".cache" / "crosspoint-reader" / ".pio" / "build" / "default" / "firmware.elf"
    )
    app0_elf = next((p for p in app0_elf_candidates if p.is_file()), app1_elf)

    qemu_bin = find_qemu()
    gdb_bin = find_gdb()
    print(f"Using QEMU : {qemu_bin}")
    print(f"Using GDB  : {gdb_bin}")

    with tempfile.TemporaryDirectory(prefix="x4_qemu_") as tmp:
        work_img = Path(tmp) / "flash_16mb.bin"
        shutil.copy2(factory_img, work_img)

        # Boot 1: Default boot into app0 (0x10000)
        out1 = run_qemu_gdb_session(qemu_bin, gdb_bin, work_img, app0_elf, 1241)
        r1 = parse_gdb_results(out1)
        print(
            f"[Boot 1 - Default app0] running={r1['label']} @ {r1['address']} "
            f"(project='{r1['project']}'), next_slot={r1['next_label']} @ {r1['next_address']}"
        )
        if r1["address"] != "0x10000" or r1["label"] != "app0" or r1["next_address"] != "0x650000":
            print(out1)
            raise AssertionError(f"Boot 1 failed verification: {r1}")

        # Flip otadata slot 1 to seq=2 -> Boot 2 loads app1 (0x650000)
        with work_img.open("r+b") as f:
            f.seek(OFFSET_OTADATA + 0x1000)
            f.write(build_otadata_entry(2))

        out2 = run_qemu_gdb_session(qemu_bin, gdb_bin, work_img, app1_elf, 1242)
        r2 = parse_gdb_results(out2)
        print(
            f"[Boot 2 - Switched app1] running={r2['label']} @ {r2['address']} "
            f"(project='{r2['project']}'), next_slot={r2['next_label']} @ {r2['next_address']}"
        )
        if (
            r2["address"] != "0x650000"
            or r2["label"] != "app1"
            or r2["project"] != "x4-magsafe-deck"
            or r2["next_address"] != "0x10000"
        ):
            print(out2)
            raise AssertionError(f"Boot 2 failed verification: {r2}")

        # Flip otadata slot 0 to seq=3 -> Boot 3 returns to app0 (0x10000)
        with work_img.open("r+b") as f:
            f.seek(OFFSET_OTADATA)
            f.write(build_otadata_entry(3))

        out3 = run_qemu_gdb_session(qemu_bin, gdb_bin, work_img, app0_elf, 1243)
        r3 = parse_gdb_results(out3)
        print(
            f"[Boot 3 - Switched app0] running={r3['label']} @ {r3['address']} "
            f"(project='{r3['project']}'), next_slot={r3['next_label']} @ {r3['next_address']}"
        )
        if r3["address"] != "0x10000" or r3["label"] != "app0" or r3["next_address"] != "0x650000":
            print(out3)
            raise AssertionError(f"Boot 3 failed verification: {r3}")

    print("\nQEMU ESP32-C3 16 MB Dual-Boot & Partition Verification: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

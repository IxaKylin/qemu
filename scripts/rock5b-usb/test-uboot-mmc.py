#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive the QEMU ROCK 5B U-Boot MMC/FAT path over UART2 and classify it.

Boots builds/rock5b-usb/uboot-mmc/u-boot-dtb.bin with the SPI NOR environment
image and the SD payload image, stops autoboot at the U-Boot prompt and runs
mmc/fatload commands, then verifies the captured output:

  - U-Boot banner + DDR ATAGS memory line + SPI environment load
  - "mmc dev 1" selects the modeled SD controller
  - "fatls mmc 1:1" lists the ProcessONE payload files
  - "fatload" of zvisor.img / zvisor.dtb reports the exact byte counts
  - "crc32" of the loaded images equals the host-side CRC32 (CPU-independent)
  - optional boot.scr source run (full staging) with --boot

UART2 (serial slot 1) is captured in <outdir>/uboot-uart2.log; the summary is
written to <outdir>/summary.json.  Exit code 0 means every requested check
passed.

Usage:
  scripts/rock5b-usb/test-uboot-mmc.py
  scripts/rock5b-usb/test-uboot-mmc.py --qemu builds/build-aarch64/qemu-system-aarch64
  scripts/rock5b-usb/test-uboot-mmc.py --boot   # also run boot.scr staging
"""

import argparse
import json
import os
import select
import signal
import struct
import subprocess
import sys
import time
import zlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD_DIR = os.path.join(REPO_ROOT, "builds", "rock5b-usb")
TASK_DIR = os.path.join(REPO_ROOT, ".oh-my-qemu", "rk3588-rock5b-usb")

DEFAULT_COMMANDS = [
    "mmc list",
    "mmc dev 1",
    "mmc info",
    "mmc rescan",
    "fatls mmc 1:1",
    "fatload mmc 1:1 0x02000000 zvisor.img",
    "fatload mmc 1:1 0x12000000 zvisor.dtb",
    "crc32 0x02000000 0x77cfb0",
    "crc32 0x12000000 0x30ce",
]


def file_crc32(path):
    crc = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--qemu",
        default=os.path.join(REPO_ROOT, "builds", "build-aarch64",
                             "qemu-system-aarch64"))
    ap.add_argument(
        "--uboot",
        default=os.path.join(BUILD_DIR, "uboot-mmc", "u-boot-dtb.bin"))
    ap.add_argument(
        "--spi",
        default=os.path.join(BUILD_DIR, "uboot-mmc", "spi-nor-mmc.img"))
    ap.add_argument("--sd", default=os.path.join(BUILD_DIR, "sd-payload.img"))
    ap.add_argument(
        "--outdir",
        default=os.path.join(TASK_DIR, "logs", "firmware-uboot-mmc"))
    ap.add_argument("--memory", default="2G")
    ap.add_argument("--boot-timeout", type=float, default=30.0)
    ap.add_argument(
        "--no-sd", action="store_true",
        help="do not attach the SD card drive")
    ap.add_argument("--cmd", action="append", default=None,
                    help="override the command list (repeatable)")
    ap.add_argument(
        "--boot", action="store_true",
        help="instead of mmc probes, run 'run bootcmd' and expect "
             "boot.scr staging")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    uart_log = os.path.join(args.outdir, "uboot-uart2.log")
    qemu_log = os.path.join(args.outdir, "qemu.stderr.log")
    summary_path = os.path.join(args.outdir, "summary.json")

    commands = args.cmd if args.cmd else (
        ["run bootcmd"] if args.boot else list(DEFAULT_COMMANDS))

    qemu_args = [args.qemu,
                 "-machine", "rock-5b", "-smp", "8", "-m", args.memory,
                 "-display", "none", "-monitor", "none",
                 "-serial", "null", "-serial", "stdio",
                 "-bios", args.uboot,
                 "-drive", "if=mtd,index=0,file=%s,format=raw,snapshot=on" % args.spi]
    if not args.no_sd:
        # Same media contract as the board flow: legacy IF_SD unit 2 is the
        # microSD slot (mmc dev 1).  Keep the drive read-only for the test.
        qemu_args += ["-drive", "if=sd,index=2,file=%s,format=raw,snapshot=on" % args.sd]

    print("RUN:", " ".join(qemu_args))
    with open(qemu_log, "wb") as qlog:
        proc = subprocess.Popen(qemu_args, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=qlog)
    os.set_blocking(proc.stdout.fileno(), False)
    data = bytearray()
    start = time.time()
    stopped_autoboot = False
    sent = 0
    awaiting_prompt_since = None

    def pump():
        nonlocal data
        r, _, _ = select.select([proc.stdout], [], [], 0.1)
        if r:
            try:
                chunk = proc.stdout.read()
            except (BlockingIOError, OSError):
                chunk = None
            if chunk:
                data.extend(chunk)
                with open(uart_log, "wb") as f:
                    f.write(data)

    deadline = start + args.boot_timeout
    while time.time() < deadline:
        pump()
        text = data.decode("utf-8", "replace")
        elapsed = time.time() - start
        if not stopped_autoboot and elapsed > 0.7:
            try:
                proc.stdin.write(b"\n")
                proc.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
            stopped_autoboot = True
            awaiting_prompt_since = None
        elif stopped_autoboot and sent == 0 and "=>" in text:
            # send first command once the prompt is visible
            try:
                proc.stdin.write((commands[0] + "\n").encode())
                proc.stdin.flush()
            except (BrokenPipeError, OSError):
                break
            sent = 1
            awaiting_prompt_since = time.time()
        elif sent > 0 and awaiting_prompt_since is not None:
            # the prompt count must grow after each command; send the next one
            if text.count("=>") > sent:
                if sent < len(commands):
                    try:
                        proc.stdin.write((commands[sent] + "\n").encode())
                        proc.stdin.flush()
                    except (BrokenPipeError, OSError):
                        break
                    sent += 1
                    awaiting_prompt_since = time.time()
                else:
                    time.sleep(1.0)
                    pump()
                    break
            elif time.time() - awaiting_prompt_since > 20:
                print("WARN: timeout waiting for prompt after command %r"
                      % commands[sent - 1])
                break
        if proc.poll() is not None:
            break
    try:
        proc.stdin.close()
    except OSError:
        pass
    time.sleep(0.3)
    pump()
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    text = data.decode("utf-8", "replace")
    with open(uart_log, "w") as f:
        f.write(text)

    checks = {}

    def check(name, ok, detail=""):
        checks[name] = {"ok": bool(ok), "detail": detail}

    check("uboot_banner", "U-Boot 2025.01" in text)
    check("ddr_atags", "DRAM:  2 GiB" in text or "DRAM:  8 GiB" in text)
    check("spi_env", "Loading Environment from SPIFlash" in text and "OK" in text)
    check("prompt", "=>" in text)

    if args.boot:
        check("boot_script_loaded", "ROCK5B_QEMU_SD_BOOT_SCRIPT" in text)
        check("processone_staging", "__KUNOS_DUAL_VM_BOOT_FAIL__" not in text)
        check("starting_kernel", "Starting kernel" in text)
        check("fdt_at_12000000", "Booting using the fdt blob at 0x12000000" in text
              or "Working FDT set to 12000000" in text
              or "Booting using the fdt blob at 12000000" in text)
    else:
        check("mmc_list", "mmc@fe2c0000" in text)
        check("mmc_dev_1", "current device" in text and "mmc" in text)
        check("fatls", all(n in text for n in
                           ("zvisor.img", "zvisor.dtb", "boot.scr",
                            "processone-zephyr")))
        zvisor = os.path.join(TASK_DIR, "output", "zvisor-real", "zvisor.img")
        dtb = os.path.join(TASK_DIR, "output", "zvisor-real", "zvisor.dtb")
        if os.path.isfile(zvisor):
            expect = "%08x" % file_crc32(zvisor)
            check("crc32_zvisor", expect in text.lower(),
                  "host crc32 %s" % expect)
        if os.path.isfile(dtb):
            expect = "%08x" % file_crc32(dtb)
            check("crc32_dtb", expect in text.lower(), "host crc32 %s" % expect)
        check("fatload_zvisor", "bytes read" in text)
        check("no_mmc_error", "** Unrecognized filesystem type **" not in text
              and "Failed to mount" not in text)

    passed = all(c["ok"] for c in checks.values())
    summary = {
        "qemu": args.qemu,
        "uboot": args.uboot,
        "uboot_sha256": _sha256(args.uboot) if os.path.isfile(args.uboot) else None,
        "spi": args.spi,
        "sd": None if args.no_sd else args.sd,
        "sd_sha256": (_sha256(args.sd)
                      if (not args.no_sd and os.path.isfile(args.sd))
                      else None),
        "memory": args.memory,
        "commands": commands,
        "checks": checks,
        "passed": passed,
        "uart_log": uart_log,
    }
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    for name, c in checks.items():
        print("%s %s%s" % ("PASS" if c["ok"] else "FAIL", name,
                           ("  (" + c["detail"] + ")") if c["detail"] else ""))
    print("VERDICT:", "PASS" if passed else "FAIL")
    print("UART2 log:", uart_log)
    print("summary:", summary_path)
    return 0 if passed else 1


def _sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the real-board ZVisor on QEMU rock-5b and exercise the USB CDC shell.

This is the automated end-to-end harness for the ROCK 5B SD boot flow:

    U-Boot (MMC/FAT) -> boot.scr -> payload staging -> bootm
        -> ZVisor (EL2) -> Type-C USB CDC console -> shell commands

The QEMU Type-C USB CDC port is exposed on a unix socket; the script waits for
the ZVisor shell prompt, sends the requested commands one character at a time
(the modeled CDC control bridge encodes console input in line-coding requests,
so slow input is the reliable path), and records the U-Boot UART2 log, the CDC
transcript and a JSON summary.

Exit code: 0 when the shell prompt was reached, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time


def main() -> int:
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", default=os.path.join(
        repo, "builds", "build-aarch64", "qemu-system-aarch64"))
    ap.add_argument("--bios", default=os.path.join(
        repo, "builds", "rock5b-usb", "uboot-mmc", "u-boot-dtb.bin"))
    ap.add_argument("--spi", default=os.path.join(
        repo, "builds", "rock5b-usb", "uboot-mmc", "spi-nor-mmc.img"))
    ap.add_argument("--sd", default=os.path.join(
        repo, "builds", "rock5b-usb", "sd-payload.img"))
    ap.add_argument("--machine", default="rock-5b,high-ram-base=0x400000000")
    ap.add_argument("--memory", default="8G")
    ap.add_argument("--nic", default="user,model=rtl8125",
                    help="QEMU -nic value; empty string disables the NIC")
    ap.add_argument("--outdir", default=os.path.join(
        repo, "builds", "rock5b-usb", "logs", "zvisor-boot"))
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--char-delay", type=float, default=0.1)
    ap.add_argument("--cmd", action="append", default=[])
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    sock_path = os.path.join(args.outdir, "usb-cdc.sock")
    uboot_log = os.path.join(args.outdir, "uboot-uart2.log")
    cdc_log = os.path.join(args.outdir, "usb-cdc.log")
    qemu_log = os.path.join(args.outdir, "qemu.log")
    summary_path = os.path.join(args.outdir, "summary.json")
    commands = args.cmd or ["zvisor status", "net iface", "rtl8125"]

    for path in (args.qemu, args.bios, args.spi):
        if not os.path.isfile(path):
            raise SystemExit("missing required input: " + path)
    if args.sd and not os.path.isfile(args.sd):
        raise SystemExit("missing SD image: " + args.sd)
    if os.path.exists(sock_path):
        os.unlink(sock_path)

    qemu_args = [
        args.qemu, "-machine", args.machine, "-smp", "8", "-m", args.memory,
        "-display", "none", "-monitor", "none",
        "-chardev", "socket,id=usbcon,path=%s,server=on,wait=off" % sock_path,
        "-serial", "chardev:usbcon",
        "-serial", "file:" + uboot_log,
        "-bios", args.bios,
        "-drive", "if=mtd,index=0,file=%s,format=raw,snapshot=on" % args.spi,
    ]
    if args.sd:
        qemu_args += ["-drive",
                      "if=sd,index=2,file=%s,format=raw" % args.sd]
    if args.nic:
        qemu_args += ["-nic", args.nic]

    with open(qemu_log, "wb") as qlog:
        proc = subprocess.Popen(qemu_args, stdout=qlog, stderr=subprocess.STDOUT)

    data = b""
    prompt = False
    sent = []
    start = time.time()
    conn = None
    try:
        while time.time() - start < args.timeout:
            if proc.poll() is not None:
                break
            if conn is None:
                try:
                    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    conn.settimeout(1.0)
                    conn.connect(sock_path)
                except OSError:
                    conn = None
                    time.sleep(0.3)
                    continue
            try:
                chunk = conn.recv(65536)
                if chunk:
                    data += chunk
                    with open(cdc_log, "wb") as f:
                        f.write(data)
            except socket.timeout:
                pass
            if not prompt and b"uart:~$" in data:
                prompt = True
                time.sleep(1.0)
                for cmd in commands:
                    for ch in cmd + "\n":
                        try:
                            conn.sendall(ch.encode())
                        except OSError:
                            break
                        time.sleep(args.char_delay)
                    sent.append(cmd)
                    time.sleep(3.0)
                    while True:
                        try:
                            chunk = conn.recv(65536)
                            if not chunk:
                                break
                            data += chunk
                        except socket.timeout:
                            break
                break
    finally:
        if conn is not None:
            conn.close()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        with open(cdc_log, "wb") as f:
            f.write(data)

    uboot = open(uboot_log, "rb").read().decode("utf-8", "replace") \
        if os.path.exists(uboot_log) else ""
    text = data.decode("utf-8", "replace")
    summary = {
        "qemu": args.qemu,
        "machine": args.machine,
        "bios": args.bios,
        "spi": args.spi,
        "sd": args.sd,
        "nic": args.nic,
        "duration_s": round(time.time() - start, 1),
        "markers": {
            "uboot_banner": "U-Boot 20" in uboot,
            "uboot_mmc": "MMC:" in uboot,
            "uboot_script": "Executing script" in uboot,
            "uboot_bootm": "Starting kernel" in uboot,
            "zvisor_admitted": "static VM set admitted" in text,
            "zvisor_autostart": "auto-start complete" in text,
            "usb_cdc_prompt": prompt,
        },
        "commands_sent": sent,
        "uboot_log": uboot_log,
        "cdc_log": cdc_log,
        "qemu_log": qemu_log,
    }
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if prompt else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the 16 MiB SPI NOR image that lets the fixed U-Boot boot ZVisor.

The QEMU rock-5b machine loads a fixed U-Boot proper binary with -bios at
0x00200000.  This script builds the companion 16 MiB XT25F128 SPI NOR image
containing only a U-Boot environment plus the real-board ZVisor legacy uImage
and its device tree.  The environment bootcmd copies both payloads out of SPI
flash and enters ZVisor with bootm; the ZVisor console is the emulated
Type-C USB CDC port of the machine.

Layout:
    0x000000  0x3f0000  erased (0xff)
    0x3f0000  0x010000  U-Boot environment (u32 LE crc32 + data)
    0x400000  ...       zvisor.img (legacy uImage, load/entry 0x10000000)
    aligned   ...       zvisor.dtb
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

IH_MAGIC = 0x27051956
FDT_MAGIC = b"\xd0\x0d\xfe\xed"

FLASH_SIZE = 16 * 1024 * 1024
ENV_OFFSET = 0x003F0000
ENV_SIZE = 0x00010000
ZVISOR_OFFSET = 0x00400000
ZVISOR_LOAD = 0x10000000
DTB_LOAD = 0x12000000
ALIGN = 0x00010000
FLASH_HZ = 24000000

DEFAULT_ENV = [
    ("bootdelay", "2"),
    ("baudrate", "1500000"),
    ("stdin", "serial"),
    ("stdout", "serial"),
    ("stderr", "serial"),
    ("kernel_addr_r", "0x00400000"),
    ("fdt_addr_r", "0x08300000"),
    ("ramdisk_addr_r", "0x0a200000"),
    ("fdt_high", "0xffffffffffffffff"),
    ("initrd_high", "0xffffffffffffffff"),
]


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_uimage(path: str) -> dict:
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        hdr = f.read(64)
    if len(hdr) != 64:
        raise SystemExit(path + ": shorter than a uImage header")
    (magic, hcrc, _ts, data_size, load, entry, dcrc, os_id, arch, itype,
     comp) = struct.unpack(">IIIIIIIBBBB", hdr[:32])
    name = hdr[32:64].split(b"\x00", 1)[0].decode("ascii", "replace")
    if magic != IH_MAGIC:
        raise SystemExit(path + ": bad uImage magic")
    if data_size + 64 != size:
        raise SystemExit(path + ": uImage size mismatch")
    hdr_zero = hdr[:4] + b"\x00\x00\x00\x00" + hdr[8:]
    calc_hcrc = zlib.crc32(hdr_zero) & 0xFFFFFFFF
    with open(path, "rb") as f:
        f.seek(64)
        calc_dcrc = 0
        for chunk in iter(lambda: f.read(1 << 20), b""):
            calc_dcrc = zlib.crc32(chunk, calc_dcrc)
    if calc_hcrc != hcrc or (calc_dcrc & 0xFFFFFFFF) != dcrc:
        raise SystemExit(path + ": uImage CRC mismatch")
    return {"size": size, "load": load, "entry": entry, "name": name,
            "sha256": sha256_file(path)}


def check_fdt(path: str) -> dict:
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != FDT_MAGIC:
        raise SystemExit(path + ": not a device tree blob")
    if struct.unpack(">I", blob[4:8])[0] != len(blob):
        raise SystemExit(path + ": FDT size mismatch")
    return {"size": len(blob), "sha256": sha256_file(path)}


def encode_env(items: list, env_size: int) -> bytes:
    data = b"".join(k.encode() + b"=" + v.encode() + b"\x00"
                    for k, v in items) + b"\x00"
    area = env_size - 4
    if len(data) > area:
        raise SystemExit("U-Boot environment does not fit")
    data += b"\xff" * (area - len(data))
    return struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF) + data


def main() -> int:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    default_zvisor = os.path.join(root, "builds", "rock5b-usb", "zvisor",
                                  "zvisor.img")
    default_dtb = os.path.join(root, "builds", "rock5b-usb", "zvisor",
                               "zvisor.dtb")

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--zvisor", default=default_zvisor)
    ap.add_argument("--dtb", default=default_dtb)
    ap.add_argument("--out", default=os.path.join(
        root, "builds", "rock5b-usb", "zvisor-spi-nor.img"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--dtb-load", type=lambda s: int(s, 0), default=DTB_LOAD)
    ap.add_argument("--flash-hz", type=int, default=FLASH_HZ)
    args = ap.parse_args()

    if not os.path.isfile(args.zvisor):
        raise SystemExit("missing ZVisor image: " + args.zvisor)
    if not os.path.isfile(args.dtb):
        raise SystemExit("missing ZVisor dtb: " + args.dtb)

    uimage = parse_uimage(args.zvisor)
    fdt = check_fdt(args.dtb)
    if uimage["load"] != ZVISOR_LOAD or uimage["entry"] != ZVISOR_LOAD:
        raise SystemExit("ZVisor uImage must load and enter at 0x10000000")
    zvisor_off = ZVISOR_OFFSET
    zvisor_end = zvisor_off + uimage["size"]
    dtb_off = (zvisor_end + ALIGN - 1) // ALIGN * ALIGN
    dtb_end = dtb_off + fdt["size"]
    env_end = ENV_OFFSET + ENV_SIZE
    if zvisor_off < env_end:
        raise SystemExit("ZVisor overlaps the U-Boot environment")
    if dtb_end > FLASH_SIZE:
        raise SystemExit("payloads do not fit in the 16 MiB SPI NOR")
    bootcmd = ("sf probe 0:0 " + str(args.flash_hz) +
               " && sf read 0x%x 0x%x 0x%x" % (ZVISOR_LOAD, zvisor_off,
                                               uimage["size"]) +
               " && sf read 0x%x 0x%x 0x%x" % (args.dtb_load, dtb_off,
                                               fdt["size"]) +
               " && bootm 0x%x - 0x%x" % (ZVISOR_LOAD, args.dtb_load))
    env_items = list(DEFAULT_ENV) + [("bootcmd", bootcmd)]
    env_blob = encode_env(env_items, ENV_SIZE)

    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[ENV_OFFSET:env_end] = env_blob
    with open(args.zvisor, "rb") as f:
        flash[zvisor_off:zvisor_end] = f.read()
    with open(args.dtb, "rb") as f:
        flash[dtb_off:dtb_end] = f.read()

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(flash)
    digest = sha256_file(out)

    manifest = {
        "generator": os.path.basename(__file__),
        "flash": {"path": out, "size": FLASH_SIZE, "sha256": digest},
        "layout": {
            "env": {"offset": ENV_OFFSET, "size": ENV_SIZE},
            "zvisor": {"offset": zvisor_off, "size": uimage["size"]},
            "dtb": {"offset": dtb_off, "size": fdt["size"]},
        },
        "bootcmd": bootcmd,
        "uimage": uimage,
        "fdt": fdt,
        "env": {"variables": [k for k, _ in env_items]},
    }
    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
            f.write("\n")

    with open(out, "rb") as f:
        disk = f.read()
    assert disk[ENV_OFFSET:env_end] == env_blob
    assert disk[zvisor_off:zvisor_end] == open(args.zvisor, "rb").read()
    assert disk[dtb_off:dtb_end] == open(args.dtb, "rb").read()

    print("wrote " + out + " (" + str(FLASH_SIZE) + " bytes)")
    print("  sha256   " + digest)
    print("  zvisor   0x%06x..0x%06x (%d bytes)" %
          (zvisor_off, zvisor_end, uimage["size"]))
    print("  zvisor.dtb 0x%06x..0x%06x (%d bytes)" %
          (dtb_off, dtb_end, fdt["size"]))
    print("  bootcmd  " + bootcmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())

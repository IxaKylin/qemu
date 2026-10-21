#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the QEMU ROCK 5B SD payload image (MBR + FAT32).

The image mirrors the real-board ProcessONE SD payload layout used by the
ROCK 5B+ boot script:

    mmc dev 1 && mmc rescan
    fatload mmc 1:1 0x2000000  zvisor.img
    fatload mmc 1:1 0x12000000 zvisor.dtb
    fatload mmc 1:1 0x32000000 processone-zephyr
    fatload mmc 1:1 0x56000000 processone-linux-Image.flash.gz
    unzip   0x56000000 0x34000000 0x8000000
    fatload mmc 1:1 0x3c000000 initramfs.cpio.flash.gz
    bootm   0x2000000 - 0x12000000

Only zvisor.img and zvisor.dtb are available locally (real-board artifacts).
Until the remote ProcessONE build host is reachable, the three guest/Linux
payloads are generated from the cached qemu-machine-images helper images and
are clearly marked as PLACEHOLDERS.  Re-run with --real-payloads DIR to build
the same media from the real payloads.

The script also writes the 16 MiB SPI NOR environment image that makes U-Boot
load boot.scr from the SD card (CONFIG_ENV_OFFSET=0x3f0000, size 0x10000).

Requires (macOS friendly): mkfs.fat (dosfstools), mtools (mcopy/mdir/mdel),
mkimage/mkenvimage (u-boot-tools), gzip.  Every FAT write is read back through
mtools (offset-addressed) and compared byte-for-byte by SHA-256.

Usage:
    scripts/rock5b-usb/make-sd-payload.py
    scripts/rock5b-usb/make-sd-payload.py --sd-size-mib 256
    scripts/rock5b-usb/make-sd-payload.py --real-payloads /path/to/payloads
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TASK_DIR = os.path.join(REPO_ROOT, ".oh-my-qemu", "rk3588-rock5b-usb")
BUILD_DIR = os.path.join(REPO_ROOT, "builds", "rock5b-usb")

DEFAULT_ZVISOR_DIR = os.path.join(TASK_DIR, "output", "zvisor-real")
DEFAULT_HELPER_DIR = os.path.expanduser(
    "~/oss/qemu-machine-images/.cache/rock-5b-radxa/helper-images")
DEFAULT_OUT = os.path.join(BUILD_DIR, "sd-payload.img")
DEFAULT_MANIFEST = os.path.join(BUILD_DIR, "sd-payload.manifest.json")
DEFAULT_SPI_ENV_OUT = os.path.join(BUILD_DIR, "uboot-mmc", "spi-nor-mmc.img")

# Real-board artifacts already copied from the remote PM build host.
PINNED = {
    "zvisor.img": "b94854354863702dbf3ddc73f85e5f3c36cfc0f9834f3854a239beced6211f94",
    "zvisor.dtb": "784320e70d37899139de5edcf55c549b77b6f651439c14263a666cf472a3c640",
}
HELPER_PINNED = {
    "Image": "605c5a6c6ae2aab74e298967c2fab3c40f23bc5bf77f52280b381ca8c245f384",
    "rootfs.cpio.gz": "86e1715edabf35da0423875b3661c960e38e78922378c99e7e305187909d8a6a",
}

# Real ProcessONE payload sizes measured on the remote host (evidence/
# zvisor-runtime-contract.md, remote/poc-key-files.txt).  The Zephyr guest
# placeholder uses the measured real size so the staging window is realistic.
REAL_PROCESSONE_ZEPHYR_SIZE = 302160
REAL_PROCESSONE_LINUX_GZ_SIZE = 8929133
REAL_PROCESSONE_INITRAMFS_GZ_SIZE = 108387320

FAT_VOLUME_ID = 0x524B3542  # "RK5B"
FAT_VOLUME_LABEL = "PAYLOAD"
PARTITION_START_LBA = 2048  # 1 MiB, keeps the FAT partition 512-byte aligned
SOURCE_DATE_EPOCH = 315532800  # 1980-01-01, deterministic FAT timestamps

_BOOT_ZEPHYR = (
    "setenv boot_processone_zephyr 'if mmc dev 1 && mmc rescan && "
    "fatload mmc 1:1 0x2000000 zvisor.img && "
    "fatload mmc 1:1 0x12000000 zvisor.dtb && "
    "fatload mmc 1:1 0x32000000 processone-zephyr && "
    "fatload mmc 1:1 0x56000000 processone-linux-Image.flash.gz && "
    "unzip 0x56000000 0x34000000 0x8000000 && "
    "fatload mmc 1:1 0x3c000000 initramfs.cpio.flash.gz; then "
    "bootm 0x2000000 - 0x12000000; else echo "
    "__KUNOS_DUAL_VM_BOOT_FAIL__; fi'"
)
_BOOT_HOST_ONLY = (
    "setenv boot_host_only 'if mmc dev 1 && mmc rescan && "
    "fatload mmc 1:1 0x2000000 zvisor.img && "
    "fatload mmc 1:1 0x12000000 zvisor.dtb; then "
    "bootm 0x2000000 - 0x12000000; else echo "
    "__HOST_ONLY_BOOT_FAIL__; fi'"
)
_BOOT_MODE = (
    "if test \"$" + "{sd_boot_mode}\" = \"host\"; then "
    "run boot_host_only; else run boot_processone_zephyr; fi"
)

BOOT_CMD = "\n".join([
    "# ROCK 5B QEMU SD boot script.",
    "# Implements the real-board ProcessONE staging contract"
    " (payload_layout.py):",
    "#   zvisor.img -> 0x02000000, zvisor.dtb -> 0x12000000,",
    "#   processone-zephyr -> 0x32000000,",
    "#   processone-linux-Image.flash.gz -> 0x56000000,"
    " unzip -> 0x34000000,",
    "#   initramfs.cpio.flash.gz -> 0x3c000000,",
    "#   bootm 0x02000000 - 0x12000000.",
    "# Set sd_boot_mode=host from the U-Boot prompt and re-source this"
    " script to",
    "# skip the guest payloads and boot the host (ZVisor) only.",
    "setenv fdt_high 0x34000000",
    _BOOT_ZEPHYR,
    _BOOT_HOST_ONLY,
    "echo ROCK5B_QEMU_SD_BOOT_SCRIPT",
    _BOOT_MODE,
]) + "\n"

BOOT_ENV = "\n".join([
    "bootdelay=2",
    "baudrate=1500000",
    "stdin=serial",
    "stdout=serial",
    "stderr=serial",
    "fdt_high=0x34000000",
    "bootcmd=mmc dev 1 && mmc rescan && "
    "fatload mmc 1:1 0x08000000 boot.scr && source 0x08000000",
]) + "\n"


def fail(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def find_tool(*names):
    for name in names:
        if os.path.isabs(name):
            if os.access(name, os.X_OK):
                return name
            continue
        found = shutil.which(name)
        if found:
            return found
    return None


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kwargs)


def fixed_env():
    """Environment with a fixed SOURCE_DATE_EPOCH for reproducible mkimage."""
    env = dict(os.environ)
    env["SOURCE_DATE_EPOCH"] = str(SOURCE_DATE_EPOCH)
    return env


def set_fixed_mtime(path):
    os.utime(path, (SOURCE_DATE_EPOCH, SOURCE_DATE_EPOCH))


def gzip_file(src, dst):
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        subprocess.run(["gzip", "-9", "-n", "-c"], stdin=fin, stdout=fout,
                       check=True)


def build_staging(args, tools):
    """Materialise every FAT file under a staging directory."""
    staging = os.path.join(BUILD_DIR, "sd-staging")
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging)

    files = []  # dicts: name, role, placeholder, size, sha256, provenance

    def add(name, role, src, placeholder, provenance):
        dst = os.path.join(staging, name)
        shutil.copyfile(src, dst)
        set_fixed_mtime(dst)
        files.append({
            "name": name,
            "role": role,
            "placeholder": placeholder,
            "size": os.path.getsize(dst),
            "sha256": sha256_file(dst),
            "provenance": provenance,
        })

    # Host payloads: real-board artifacts, hash pinned.
    zvisor_img = os.path.join(args.zvisor_dir, "zvisor.img")
    zvisor_dtb = os.path.join(args.zvisor_dir, "zvisor.dtb")
    for path in (zvisor_img, zvisor_dtb):
        if not os.path.isfile(path):
            fail("missing host artifact: " + path)
    add("zvisor.img", "ZVisor host legacy uImage", zvisor_img, False,
        "remote ProcessONE build; output/zvisor-real/zvisor.img")
    add("zvisor.dtb", "ZVisor host device tree", zvisor_dtb, False,
        "remote ProcessONE build; output/zvisor-real/zvisor.dtb")

    # Guest / Linux payloads: real if provided, otherwise clearly marked
    # placeholders derived from the cached qemu-machine-images helper set.
    if args.real_payloads:
        real = args.real_payloads
        zephyr = os.path.join(real, "processone-zephyr")
        linux_gz = os.path.join(real, "processone-linux-Image.flash.gz")
        initramfs_gz = os.path.join(real, "initramfs.cpio.flash.gz")
        if not os.path.isfile(initramfs_gz):
            alt = os.path.join(real, "processone-initramfs.cpio.flash.gz")
            if os.path.isfile(alt):
                initramfs_gz = alt
        for path in (zephyr, linux_gz, initramfs_gz):
            if not os.path.isfile(path):
                fail("missing real payload: " + path)
        add("processone-zephyr", "Zephyr guest binary (ProcessONE)", zephyr,
            False, "real ProcessONE payload; " + zephyr)
        add("processone-linux-Image.flash.gz", "Linux Image (gzip, ProcessONE)",
            linux_gz, False, "real ProcessONE payload; " + linux_gz)
        add("initramfs.cpio.flash.gz", "initramfs cpio (gzip, ProcessONE)",
            initramfs_gz, False, "real ProcessONE payload; " + initramfs_gz)
        placeholders = False
    else:
        helper = args.helper_dir
        helper_image = os.path.join(helper, "Image")
        helper_rootfs = os.path.join(helper, "rootfs.cpio.gz")
        for path in (helper_image, helper_rootfs):
            if not os.path.isfile(path):
                fail("missing placeholder source: " + path)
        placeholders = True

        # Zephyr guest placeholder: measured real size, marked content.
        zephyr_dst = os.path.join(staging, "processone-zephyr")
        header = (
            "PLACEHOLDER: not a real ProcessONE RTOS guest image.\n"
            "Real artifact: processone-zephyr (%d bytes, ProcessONE RTOS 4.3.1\n"
            "micro-ROS guest, remote devstation1 build host).\n"
            "This file only exercises the U-Boot FAT staging path in QEMU.\n"
            "Regenerate with make-sd-payload.py --real-payloads DIR.\n"
            % REAL_PROCESSONE_ZEPHYR_SIZE
        ).encode()
        if len(header) > REAL_PROCESSONE_ZEPHYR_SIZE:
            fail("zephyr placeholder header does not fit")
        with open(zephyr_dst, "wb") as f:
            f.write(header)
            f.write(b"\x00" * (REAL_PROCESSONE_ZEPHYR_SIZE - len(header)))
        set_fixed_mtime(zephyr_dst)
        files.append({
            "name": "processone-zephyr",
            "role": "PLACEHOLDER Zephyr guest binary (real size kept)",
            "placeholder": True,
            "size": os.path.getsize(zephyr_dst),
            "sha256": sha256_file(zephyr_dst),
            "provenance": "generated placeholder, real size %d bytes"
                          % REAL_PROCESSONE_ZEPHYR_SIZE,
        })

        # Linux Image placeholder: gzip of the cached helper Image.  U-Boot
        # "unzip" decompresses it to 0x34000000 inside the 128 MiB window.
        linux_dst = os.path.join(staging, "processone-linux-Image.flash.gz")
        gzip_file(helper_image, linux_dst)
        set_fixed_mtime(linux_dst)
        files.append({
            "name": "processone-linux-Image.flash.gz",
            "role": "PLACEHOLDER Linux Image (gzip of helper-images/Image)",
            "placeholder": True,
            "size": os.path.getsize(linux_dst),
            "sha256": sha256_file(linux_dst),
            "provenance": "gzip -9 -n of qemu-machine-images helper-images/Image "
                          "(sha256 %s)" % HELPER_PINNED["Image"],
        })

        # initramfs placeholder: the cached helper rootfs cpio.gz already is a
        # gzip stream, which is the transport the boot script expects.
        initramfs_dst = os.path.join(staging, "initramfs.cpio.flash.gz")
        shutil.copyfile(helper_rootfs, initramfs_dst)
        set_fixed_mtime(initramfs_dst)
        files.append({
            "name": "initramfs.cpio.flash.gz",
            "role": "PLACEHOLDER initramfs cpio (gzip of helper rootfs)",
            "placeholder": True,
            "size": os.path.getsize(initramfs_dst),
            "sha256": sha256_file(initramfs_dst),
            "provenance": "qemu-machine-images helper-images/rootfs.cpio.gz "
                          "(sha256 %s)" % HELPER_PINNED["rootfs.cpio.gz"],
        })

    # boot.cmd / boot.scr
    boot_cmd_path = os.path.join(staging, "boot.cmd")
    with open(boot_cmd_path, "w") as f:
        f.write(BOOT_CMD)
    set_fixed_mtime(boot_cmd_path)
    boot_scr_path = os.path.join(staging, "boot.scr")
    # SOURCE_DATE_EPOCH keeps the mkimage header timestamp fixed, so the
    # boot.scr (and therefore the whole SD image) is byte-reproducible.
    run([tools["mkimage"], "-A", "arm64", "-O", "linux", "-T", "script",
         "-C", "none", "-n", "ROCK 5B QEMU SD boot", "-d",
         boot_cmd_path, boot_scr_path], env=fixed_env())
    set_fixed_mtime(boot_scr_path)
    files.append({
        "name": "boot.cmd",
        "role": "U-Boot boot script source (audit copy)",
        "placeholder": False,
        "size": os.path.getsize(boot_cmd_path),
        "sha256": sha256_file(boot_cmd_path),
        "provenance": "scripts/rock5b-usb/make-sd-payload.py BOOT_CMD",
    })
    files.append({
        "name": "boot.scr",
        "role": "U-Boot script image (mkimage -T script)",
        "placeholder": False,
        "size": os.path.getsize(boot_scr_path),
        "sha256": sha256_file(boot_scr_path),
        "provenance": "mkimage -A arm64 -T script of boot.cmd",
    })

    # Human-readable placeholder warning.
    placeholder_txt = os.path.join(staging, "PAYLOAD-PLACEHOLDERS.txt")
    with open(placeholder_txt, "w") as f:
        f.write("ROCK 5B QEMU SD payload image - payload provenance\n")
        f.write("==================================================\n\n")
        if placeholders:
            f.write("WARNING: this image contains PLACEHOLDER payloads for the\n")
            f.write("ProcessONE Zephyr guest and Linux/initramfs transport.  Only\n")
            f.write("zvisor.img / zvisor.dtb are real remote-built artifacts.\n\n")
            f.write("Real payload sizes measured on the remote PM host:\n")
            f.write("  processone-zephyr                  %d bytes\n"
                    % REAL_PROCESSONE_ZEPHYR_SIZE)
            f.write("  processone-linux-Image.flash.gz    %d bytes\n"
                    % REAL_PROCESSONE_LINUX_GZ_SIZE)
            f.write("  processone-initramfs.cpio.flash.gz %d bytes\n"
                    % REAL_PROCESSONE_INITRAMFS_GZ_SIZE)
            f.write("\nRegenerate with:\n")
            f.write("  scripts/rock5b-usb/make-sd-payload.py --real-payloads DIR\n\n")
        else:
            f.write("This image carries real ProcessONE guest/Linux payloads.\n\n")
        f.write("Files in this image:\n")
        for item in sorted(files, key=lambda x: x["name"]):
            f.write("  %-36s %10d  %s%s\n" % (
                item["name"], item["size"], item["sha256"],
                "  [PLACEHOLDER]" if item["placeholder"] else ""))
    set_fixed_mtime(placeholder_txt)
    files.append({
        "name": "PAYLOAD-PLACEHOLDERS.txt",
        "role": "payload provenance / placeholder warning",
        "placeholder": False,
        "size": os.path.getsize(placeholder_txt),
        "sha256": sha256_file(placeholder_txt),
        "provenance": "generated by make-sd-payload.py",
    })

    on_media_manifest = {
        "generated_by": "scripts/rock5b-usb/make-sd-payload.py",
        "machine": "rock-5b (QEMU)",
        "boot_contract": {
            "controller": "mmc 1 (sdmmc@fe2c0000)",
            "partition": "mmc 1:1",
            "boot_script": "boot.scr",
            "bootcmd_default":
                "mmc dev 1 && mmc rescan && "
                "fatload mmc 1:1 0x08000000 boot.scr && "
                "source 0x08000000",
            "staging": {
                "zvisor.img": "0x02000000",
                "zvisor.dtb": "0x12000000",
                "processone-zephyr": "0x32000000",
                "processone-linux-Image.flash.gz": "0x56000000",
                "linux_unzip": "0x34000000",
                "initramfs.cpio.flash.gz": "0x3c000000",
                "bootm": "bootm 0x02000000 - 0x12000000",
                "fdt_high": "0x34000000",
            },
        },
        "placeholders_present": placeholders,
        "files": [{k: item[k] for k in
                   ("name", "role", "placeholder", "size", "sha256")}
                  for item in sorted(files, key=lambda x: x["name"])],
    }
    manifest_path = os.path.join(staging, "sd-manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(on_media_manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    set_fixed_mtime(manifest_path)
    files.append({
        "name": "sd-manifest.json",
        "role": "on-media payload manifest",
        "placeholder": False,
        "size": os.path.getsize(manifest_path),
        "sha256": sha256_file(manifest_path),
        "provenance": "generated by make-sd-payload.py",
    })

    return staging, files, placeholders


def build_fat_partition(staging, files, part_path, tools):
    run([tools["mkfs_fat"], "-F", "32", "-n", FAT_VOLUME_LABEL,
         "-i", "%08x" % FAT_VOLUME_ID, part_path])
    for item in sorted(files, key=lambda x: x["name"]):
        src = os.path.join(staging, item["name"])
        run([tools["mcopy"], "-i", part_path, "-o", src, "::/" + item["name"]])
    listing = subprocess.run([tools["mdir"], "-i", part_path, "::/"],
                             capture_output=True, text=True, check=True).stdout
    # mdir prints short 8.3 names without a separate long-name entry, so check
    # each file with an explicit path lookup instead of a substring match.
    for item in files:
        check = subprocess.run([tools["mdir"], "-i", part_path,
                                "::/" + item["name"]],
                               capture_output=True, text=True)
        if check.returncode != 0:
            fail("FAT lookup failed for " + item["name"] + ": " + check.stderr)
    print(listing)


def normalize_fat_dir_timestamps(part_path):
    """Make FAT directory entries byte-reproducible.

    mtools fills each file's creation time and last-access date from the host
    clock, which would make the image hash change between runs.  The file
    contents and write timestamps are already deterministic; rewrite only the
    creation/access fields of the root-directory entries to fixed values.
    (All files live in the FAT32 root directory; fail loudly if that changes.)
    """
    with open(part_path, "r+b") as f:
        bs = f.read(512)
        if bs[0x52:0x5A] != b"FAT32   ":
            fail("normalize_fat_dir_timestamps: not a FAT32 volume")
        bps = struct.unpack_from("<H", bs, 0x0B)[0]
        spc = bs[0x0D]
        rsvd = struct.unpack_from("<H", bs, 0x0E)[0]
        nfats = bs[0x10]
        root_entries = struct.unpack_from("<H", bs, 0x11)[0]
        fat_sz = struct.unpack_from("<I", bs, 0x24)[0]
        root_clu = struct.unpack_from("<I", bs, 0x2C)[0]
        if bps not in (512, 1024, 2048, 4096) or not spc or not fat_sz:
            fail("normalize_fat_dir_timestamps: unsupported FAT parameters")
        if root_entries != 0 or root_clu < 2:
            fail("normalize_fat_dir_timestamps: expected a FAT32 root cluster")
        cluster_bytes = bps * spc
        fat_off = rsvd * bps
        data_start = (rsvd + nfats * fat_sz) * bps

        def cluster_offset(clu):
            return data_start + (clu - 2) * cluster_bytes

        def next_cluster(clu):
            f.seek(fat_off + clu * 4)
            return struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF

        chain = []
        clu = root_clu
        while 2 <= clu < 0x0FFFFFF8:
            if clu in chain or len(chain) > 4096:
                fail("normalize_fat_dir_timestamps: corrupt root chain")
            chain.append(clu)
            clu = next_cluster(clu)

        # 00:00:00, 1980-01-01: creation time/date + last-access date and
        # write time/date.  mtools takes creation/access from the host clock.
        fixed_crt = struct.pack("<BHHH", 0, 0, 0x0021, 0x0021)
        fixed_wrt = struct.pack("<HH", 0, 0x0021)
        patched = 0
        for clu in chain:
            base = cluster_offset(clu)
            f.seek(base)
            buf = bytearray(f.read(cluster_bytes))
            for off in range(0, len(buf) - 31, 32):
                entry = buf[off:off + 32]
                if entry[0] == 0x00:
                    break
                if entry[0] == 0xE5:
                    continue
                attr = entry[11]
                if attr & 0x3F == 0x0F:  # long-file-name slot, no timestamps
                    continue
                # CrtTimeTenth + CrtTime + CrtDate + LstAccessDate = 7 bytes
                buf[off + 13:off + 20] = fixed_crt
                # WrtTime + WrtDate = 4 bytes (mcopy writes the host clock)
                buf[off + 22:off + 26] = fixed_wrt
                patched += 1
            f.seek(base)
            f.write(buf)
        if patched < 1:
            fail("normalize_fat_dir_timestamps: no root entries patched")
        return patched


def verify_fat_readback(image_path, part_offset, files, tools):
    for item in files:
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out.bin")
            run([tools["mcopy"], "-i", "%s@@%d" % (image_path, part_offset),
                 "-o", "::/" + item["name"], out])
            got = sha256_file(out)
            if got != item["sha256"]:
                fail("FAT readback hash mismatch for %s: %s != %s"
                     % (item["name"], got, item["sha256"]))
    print("readback: %d/%d FAT files verified by SHA-256" %
          (len(files), len(files)))


def build_mbr_image(out_path, total_bytes, part_offset, part_size):
    if total_bytes % 512 != 0 or part_offset % 512 != 0 or part_size % 512 != 0:
        fail("image/partition geometry must be 512-byte aligned")
    if part_offset + part_size > total_bytes:
        fail("partition exceeds image size")
    start_lba = part_offset // 512
    sectors = part_size // 512
    with open(out_path, "wb") as f:
        f.truncate(total_bytes)
    mbr = bytearray(512)
    # Disk signature + one FAT32 LBA (type 0x0c) partition entry.
    struct.pack_into("<I", mbr, 440, 0x514B4646)  # "QKFF"
    entry = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x0C,
                        b"\xfe\xff\xff", start_lba, sectors)
    mbr[446:446 + len(entry)] = entry
    mbr[510:512] = b"\x55\xaa"
    with open(out_path, "r+b") as f:
        f.seek(0)
        f.write(mbr)


def copy_partition_into_image(out_path, part_path, part_offset):
    with open(out_path, "r+b") as fout, open(part_path, "rb") as fin:
        fout.seek(part_offset)
        shutil.copyfileobj(fin, fout, 1 << 20)


def write_spi_env_image(out_path, staging, tools, env_extra=()):
    env_txt = os.path.join(staging, "boot.env")
    with open(env_txt, "w") as f:
        f.write(BOOT_ENV)
        for line in env_extra:
            f.write(line.rstrip("\n") + "\n")
    set_fixed_mtime(env_txt)
    env_bin = os.path.join(staging, "spi-env.bin")
    run([tools["mkenvimage"], "-s", "0x10000", "-o", env_bin, env_txt],
        env=fixed_env())
    blob = open(env_bin, "rb").read()
    if len(blob) > 0x10000:
        fail("environment blob exceeds CONFIG_ENV_SIZE")
    image = bytearray(b"\xff" * (16 * 1024 * 1024))
    offset = 0x3F0000  # CONFIG_ENV_OFFSET in uboot-mmc.config
    image[offset:offset + len(blob)] = blob
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(image)
    # Read back the environment bytes from the image.
    with open(out_path, "rb") as f:
        f.seek(offset)
        got = f.read(len(blob))
    if got != blob:
        fail("SPI NOR environment readback mismatch")
    if os.path.getsize(out_path) != 16 * 1024 * 1024:
        fail("SPI NOR image must be exactly 16 MiB")
    return {
        "path": out_path,
        "size": os.path.getsize(out_path),
        "sha256": sha256_file(out_path),
        "env_offset": "0x3f0000",
        "env_size": len(blob),
        "env_sha256": sha256_bytes(blob),
        "bootcmd": "mmc dev 1 && mmc rescan && "
                   "fatload mmc 1:1 0x08000000 boot.scr && "
                   "source 0x08000000",
    }


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--zvisor-dir", default=DEFAULT_ZVISOR_DIR,
                        help="directory with real zvisor.img/zvisor.dtb")
    parser.add_argument("--helper-dir", default=DEFAULT_HELPER_DIR,
                        help="cached qemu-machine-images helper-images directory")
    parser.add_argument("--real-payloads", default=None,
                        help="directory with real ProcessONE payloads")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="output SD image (default builds/rock5b-usb/sd-payload.img)")
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST,
                        help="output JSON manifest")
    parser.add_argument("--sd-size-mib", type=int, default=256,
                        help="SD image size in MiB (default 256)")
    parser.add_argument("--spi-env-out", default=DEFAULT_SPI_ENV_OUT,
                        help="output 16 MiB SPI NOR env image ('' to skip)")
    parser.add_argument("--no-spi-env", action="store_true",
                        help="do not write the SPI NOR environment image")
    parser.add_argument("--env-extra", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="extra line(s) for the SPI NOR environment "
                             "(e.g. --env-extra sd_boot_mode=host)")
    args = parser.parse_args()

    if args.sd_size_mib < 64 or args.sd_size_mib > 2048:
        fail("--sd-size-mib must be between 64 and 2048")

    tools = {
        "mkfs_fat": find_tool("mkfs.fat", "mkfs.vfat",
                              "/opt/homebrew/sbin/mkfs.fat",
                              "/opt/homebrew/sbin/mkfs.vfat"),
        "mcopy": find_tool("mcopy"),
        "mdir": find_tool("mdir"),
        "mkimage": find_tool("mkimage"),
        "mkenvimage": find_tool("mkenvimage"),
    }
    missing = [name for name, path in tools.items() if not path]
    if missing:
        fail("missing host tools: " + ", ".join(missing) +
             " (install dosfstools, mtools, u-boot-tools)")

    # Hash-pin the real-board artifacts that must not change silently.
    for name, pinned in PINNED.items():
        path = os.path.join(args.zvisor_dir, name)
        if os.path.isfile(path):
            got = sha256_file(path)
            if got != pinned:
                fail("%s sha256 mismatch: %s != pinned %s" % (path, got, pinned))
    if not args.real_payloads:
        for name, pinned in HELPER_PINNED.items():
            path = os.path.join(args.helper_dir, name)
            if not os.path.isfile(path):
                fail("missing helper image: " + path)
            got = sha256_file(path)
            if got != pinned:
                fail("%s sha256 mismatch: %s != pinned %s" % (path, got, pinned))

    os.makedirs(BUILD_DIR, exist_ok=True)
    staging, files, placeholders = build_staging(args, tools)

    total = args.sd_size_mib * 1024 * 1024
    part_offset = PARTITION_START_LBA * 512
    part_size = total - part_offset
    out_path = os.path.abspath(args.out)
    part_tmp = os.path.join(BUILD_DIR, "sd-payload.part")
    for path in (out_path, part_tmp, args.manifest):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)

    with open(part_tmp, "wb") as f:
        f.truncate(part_size)
    build_fat_partition(staging, files, part_tmp, tools)
    patched = normalize_fat_dir_timestamps(part_tmp)
    print("normalized %d FAT root-directory entry timestamps" % patched)
    part_sha = sha256_file(part_tmp)

    build_mbr_image(out_path, total, part_offset, part_size)
    copy_partition_into_image(out_path, part_tmp, part_offset)

    # Structural readback: MBR signature/partition and byte-identical payload.
    with open(out_path, "rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        fail("MBR signature readback failed")
    if mbr[450] != 0x0C:
        fail("MBR partition type readback failed: 0x%02x" % mbr[450])
    lba, sectors = struct.unpack_from("<II", mbr, 454)
    if lba * 512 != part_offset or sectors * 512 != part_size:
        fail("MBR partition entry readback failed (lba=%d sectors=%d)"
             % (lba, sectors))
    with open(out_path, "rb") as f:
        f.seek(part_offset)
        data = f.read(part_size)
    if sha256_bytes(data) != part_sha:
        fail("partition readback hash mismatch inside SD image")
    verify_fat_readback(out_path, part_offset, files, tools)

    image_sha = sha256_file(out_path)

    spi_info = None
    if not args.no_spi_env and args.spi_env_out:
        spi_info = write_spi_env_image(os.path.abspath(args.spi_env_out),
                                       staging, tools, args.env_extra)
        shutil.copyfile(os.path.join(staging, "boot.env"),
                        os.path.join(BUILD_DIR, "sd-boot.env"))
        set_fixed_mtime(os.path.join(BUILD_DIR, "sd-boot.env"))
    # Keep the generated boot script next to the image for audit.
    shutil.copyfile(os.path.join(staging, "boot.cmd"),
                    os.path.join(BUILD_DIR, "sd-boot.cmd"))
    shutil.copyfile(os.path.join(staging, "boot.scr"),
                    os.path.join(BUILD_DIR, "sd-boot.scr"))

    manifest = {
        "generated_by": "scripts/rock5b-usb/make-sd-payload.py",
        "task": "rk3588-rock5b-usb task-10 (MMC/FAT U-Boot + SD payload media)",
        "machine": "rock-5b (QEMU)",
        "image": {
            "path": out_path,
            "size": os.path.getsize(out_path),
            "sha256": image_sha,
            "partition_table": "MBR",
            "partition_type": "0x0c FAT32 LBA",
            "partition_start_lba": PARTITION_START_LBA,
            "partition_offset_bytes": part_offset,
            "partition_size_bytes": part_size,
            "partition_sha256": part_sha,
            "fat_type": "FAT32",
            "fat_volume_id": "%08x" % FAT_VOLUME_ID,
            "fat_volume_label": FAT_VOLUME_LABEL,
        },
        "placeholders_present": placeholders,
        "real_payloads_available": bool(args.real_payloads),
        "files": files,
        "spi_env_image": spi_info,
        "inputs": {
            "zvisor_dir": os.path.abspath(args.zvisor_dir),
            "helper_dir": (None if args.real_payloads
                           else os.path.abspath(args.helper_dir)),
            "real_payloads": (os.path.abspath(args.real_payloads)
                              if args.real_payloads else None),
        },
    }
    with open(args.manifest, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")

    print()
    print("SD payload image: %s" % out_path)
    print("  size:       %d bytes (%d MiB)"
          % (os.path.getsize(out_path), args.sd_size_mib))
    print("  sha256:     %s" % image_sha)
    print("  partition:  MBR type 0x0c at LBA %d (%d bytes), FAT32"
          % (PARTITION_START_LBA, part_size))
    print("  placeholders present: %s" % placeholders)
    for item in sorted(files, key=lambda x: x["name"]):
        print("  %-36s %10d  %s%s" % (
            item["name"], item["size"], item["sha256"],
            "  [PLACEHOLDER]" if item["placeholder"] else ""))
    if spi_info:
        print("SPI NOR env image: %s" % spi_info["path"])
        print("  sha256: %s (env offset 0x3f0000, %d bytes)"
              % (spi_info["sha256"], spi_info["env_size"]))
    print("manifest: %s" % os.path.abspath(args.manifest))


if __name__ == "__main__":
    main()

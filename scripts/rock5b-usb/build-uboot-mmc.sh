#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the MMC/FAT-capable U-Boot proper used as firmware (-bios) for the
# QEMU "rock-5b" machine.
#
# Source contract
#   U-Boot v2025.01 release tarball, ftp.denx.de, sha256 pinned below.
#   Base defconfig: rock5b-rk3588_defconfig.
#   QEMU deltas, all applied to this build tree only:
#     scripts/rock5b-usb/uboot-mmc/rock5b-qemu.dts
#     scripts/rock5b-usb/uboot-mmc/rock5b-qemu-u-boot.dtsi
#     scripts/rock5b-usb/uboot-mmc/uboot-mmc.config
#     scripts/rock5b-usb/uboot-mmc/0001-rockchip-use-ddr-atags-for-memory-size.patch
#   The DDR ATAGS patch comes from qemu-machine-images upstream/rock-5b and is
#   never applied to a real-board U-Boot source/config.
#
# Outputs
#   builds/rock5b-usb/uboot-mmc/u-boot-dtb.bin     (raw U-Boot proper + control DT)
#   builds/rock5b-usb/uboot-mmc/u-boot-dtb.bin.sha256
#   builds/rock5b-usb/uboot-mmc/.config, build.log, provenance.txt
#
# Usage
#   scripts/rock5b-usb/build-uboot-mmc.sh            # full (re)build
#   JOBS=8 scripts/rock5b-usb/build-uboot-mmc.sh
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ASSETS="$REPO_ROOT/scripts/rock5b-usb/uboot-mmc"
WORK="$REPO_ROOT/builds/rock5b-usb"
SRC_ROOT="$WORK/uboot-src"
BUILD="$WORK/uboot-mmc/build"
OUT="$WORK/uboot-mmc"
LOG="$OUT/build.log"

UBOOT_VERSION=2025.01
TARBALL="u-boot-${UBOOT_VERSION}.tar.bz2"
TARBALL_SHA256=cdef7d507c93f1bbd9f015ea9bc21fa074268481405501945abc6f854d5b686f
TARBALL_URL="https://ftp.denx.de/pub/u-boot/${TARBALL}"
CROSS_COMPILE="${CROSS_COMPILE:-/opt/homebrew/bin/aarch64-elf-}"
if [ -z "${JOBS:-}" ]; then
    if command -v sysctl >/dev/null 2>&1; then
        JOBS=$(sysctl -n hw.ncpu)
    else
        JOBS=$(getconf _NPROCESSORS_ONLN)
    fi
fi
MAKE="${MAKE:-}"
if [ -z "$MAKE" ]; then
    if command -v gmake >/dev/null 2>&1; then
        MAKE=gmake
    else
        MAKE=make
    fi
fi
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:${PATH}"

log() { printf '%s\n' "$*" | tee -a "$LOG"; }

mkdir -p "$SRC_ROOT" "$OUT"
: > "$LOG"

# 1. Fetch and verify the pinned U-Boot source tarball.
if [ ! -f "$SRC_ROOT/$TARBALL" ]; then
    log "fetching $TARBALL_URL"
    curl -fL --connect-timeout 15 -m 600 -o "$SRC_ROOT/$TARBALL.part" "$TARBALL_URL"
    mv "$SRC_ROOT/$TARBALL.part" "$SRC_ROOT/$TARBALL"
fi
echo "$TARBALL_SHA256  $SRC_ROOT/$TARBALL" | shasum -a 256 -c - | tee -a "$LOG"

# 2. Fresh extraction and QEMU-only deltas.
rm -rf "$SRC_ROOT/build" "$BUILD"
mkdir -p "$SRC_ROOT/build" "$BUILD"
tar -xjf "$SRC_ROOT/$TARBALL" -C "$SRC_ROOT/build"
SRC="$SRC_ROOT/build/u-boot-${UBOOT_VERSION}"
log "U-Boot source: $SRC"
cp "$ASSETS/rock5b-qemu.dts" "$ASSETS/rock5b-qemu-u-boot.dtsi" "$SRC/arch/arm/dts/"
patch -d "$SRC" -p1 < \
    "$ASSETS/0001-rockchip-use-ddr-atags-for-memory-size.patch" | \
    tee -a "$LOG"

# 3. Configure.  A plain "make rock5b-rk3588_defconfig" is not usable for
#    this U-Boot: the board defconfig contains "# CONFIG_X is not set" lines
#    that the U-Boot defconfig reader ignores, leaving invalid empty hex
#    symbols (e.g. FASTBOOT_BUF_ADDR) that make syncconfig prompt forever.
#    Buildroot configures this target through kconfig merge (alldefconfig),
#    so reproduce that: concatenate the board defconfig and the QEMU fragment
#    into KCONFIG_ALLCONFIG and let alldefconfig resolve every symbol.
cat "$SRC/configs/rock5b-rk3588_defconfig" \
    "$ASSETS/uboot-mmc.config" > "$OUT/alldef.config"
"$MAKE" -C "$SRC" O="$BUILD" KCONFIG_ALLCONFIG="$OUT/alldef.config" \
    alldefconfig 2>&1 | tee -a "$LOG"
"$MAKE" -C "$SRC" O="$BUILD" olddefconfig 2>&1 | tee -a "$LOG"
cp "$BUILD/.config" "$OUT/.config"

# Fail early if a required QEMU-machine option was dropped by Kconfig.
missing=0
for opt in \
    CONFIG_TEXT_BASE=0x00200000 \
    CONFIG_DEFAULT_DEVICE_TREE=\"rock5b-qemu\" \
    CONFIG_MMC=y CONFIG_DM_MMC=y CONFIG_MMC_DW_ROCKCHIP=y CONFIG_CMD_MMC=y \
    CONFIG_FS_FAT=y CONFIG_CMD_FAT=y CONFIG_CMD_SOURCE=y CONFIG_CMD_UNZIP=y \
    CONFIG_DOS_PARTITION=y CONFIG_ENV_IS_IN_SPI_FLASH=y CONFIG_FIT=y \
    '# CONFIG_FIT_SIGNATURE is not set' \
    '# CONFIG_TOOLS_LIBCRYPTO is not set' \
    '# CONFIG_TOOLS_KWBIMAGE is not set'; do
    if ! grep -qx "$opt" "$BUILD/.config"; then
        log "ERROR: required config option missing after olddefconfig: $opt"
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    exit 1
fi
cp "$BUILD/.config" "$OUT/.config"

# 4. Build U-Boot proper + appended control device tree.
# NO_PYTHON=1: the host /opt/homebrew python3 has no setuptools, so U-Boot's
# pylibfdt (and binman) Python host tools cannot build.  This artifact
# (u-boot-dtb.bin plus control DT) does not use them; U-Boot documents
# NO_PYTHON=1 for exactly this case.
# Build only U-Boot proper + its appended control DT.  The default "all"
# target also builds SPL and runs binman's Python step, which this QEMU
# artifact does not need (Buildroot sets UBOOT_MAKE_TARGET=u-boot-dtb.bin for
# the same reason).
"$MAKE" -C "$SRC" O="$BUILD" NO_PYTHON=1 \
    CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" u-boot-dtb.bin 2>&1 | \
    tee -a "$LOG"

# 5. Publish and verify the artifact.
cp "$BUILD/u-boot-dtb.bin" "$OUT/u-boot-dtb.bin"
HASH=$(shasum -a 256 "$OUT/u-boot-dtb.bin" | cut -d' ' -f1)
echo "$HASH  u-boot-dtb.bin" > "$OUT/u-boot-dtb.bin.sha256"

fragment_sha=$(shasum -a 256 "$ASSETS/uboot-mmc.config" | cut -d' ' -f1)
dts_sha=$(shasum -a 256 "$ASSETS/rock5b-qemu.dts" | cut -d' ' -f1)
patch_sha=$(shasum -a 256 \
    "$ASSETS/0001-rockchip-use-ddr-atags-for-memory-size.patch" | \
    cut -d' ' -f1)
version_str=$(strings "$OUT/u-boot-dtb.bin" | grep -m1 'U-Boot 2025' || echo MISSING)
{
    echo "U-Boot version: v$UBOOT_VERSION (release tarball $TARBALL)"
    echo "tarball sha256: $TARBALL_SHA256"
    echo "tarball url: $TARBALL_URL"
    echo "base defconfig: rock5b-rk3588_defconfig"
    echo "fragment: scripts/rock5b-usb/uboot-mmc/uboot-mmc.config" \
         "sha256 $fragment_sha"
    echo "control dts: scripts/rock5b-usb/uboot-mmc/rock5b-qemu.dts" \
         "sha256 $dts_sha"
    echo "ddr atags patch: scripts/rock5b-usb/uboot-mmc/" \
         "0001-rockchip-use-ddr-atags-for-memory-size.patch" \
         "sha256 $patch_sha"
    echo "source: qemu-machine-images branch upstream/rock-5b commit"
    echo "        eacd4aea1879382428bfd87836aec41a869b6459 (v1.3.0)"
    echo "cross compiler: $CROSS_COMPILE"
    "$CROSS_COMPILE"gcc --version | head -1
    echo "u-boot-dtb.bin sha256: $HASH"
    echo "u-boot-dtb.bin size: $(wc -c < "$OUT/u-boot-dtb.bin") bytes"
    echo "u-boot-dtb.bin version string: $version_str"
} > "$OUT/provenance.txt"

# 6. Independent checks: banner, appended DT contents (mmc1/sdmmc).
if ! strings "$OUT/u-boot-dtb.bin" | grep -q "U-Boot ${UBOOT_VERSION}"; then
    log "ERROR: U-Boot version banner missing from artifact"
    exit 1
fi
python3 - "$OUT/u-boot-dtb.bin" "$WORK/uboot-mmc/dt.dtb" <<'PYDTB' | tee -a "$LOG"
import struct
import sys

data = open(sys.argv[1], "rb").read()
off = data.rfind(b"\xd0\x0d\xfe\xed")
if off < 0:
    raise SystemExit("ERROR: no appended FDT found in u-boot-dtb.bin")
total = struct.unpack(">I", data[off + 4:off + 8])[0]
open(sys.argv[2], "wb").write(data[off:off + total])
print(f"appended control DTB: offset 0x{off:x}, {total} bytes")
PYDTB
dtc -I dtb -O dts "$WORK/uboot-mmc/dt.dtb" 2>/dev/null > "$WORK/uboot-mmc/dt.dts"
for needle in \
    'model = "Radxa ROCK 5B QEMU"' \
    'mmc1' \
    'mmc@fe2c0000' \
    'rockchip,rk3288-dw-mshc' \
    'feb50000'; do
    if ! grep -q "$needle" "$WORK/uboot-mmc/dt.dts"; then
        log "ERROR: control DT verification failed: $needle"
        exit 1
    fi
done
log "OK: control DT has model/mmc1/sdmmc/rk3288 fallback compatible/UART2"
log "OK: $OUT/u-boot-dtb.bin sha256 $HASH"
log "build complete"

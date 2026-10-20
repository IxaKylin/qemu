#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Install the fixed U-Boot proper binary used by the QEMU rock-5b machine.
#
# The reference machine boots U-Boot proper directly from -bios at 0x00200000
# (BootROM, DDR training, SPL and BL31 are not modeled).  The validated image
# is the qemu-machine-images aarch64 rock-5b v1.3.0 u-boot-dtb.bin (U-Boot
# 2025.01, sha256 f1bb1d0e33712444877c32011fa0a067744ab2f52a8276f0950d7d3a195651f8).
#
# Sources, in order:
#   1. QEMU_MACHINE_IMAGES_CACHE  (an extracted release image directory)
#   2. $HOME/oss/qemu-machine-images .cache/releases/<VERSION>/aarch64/rock-5b
#   3. the dist tarball in $HOME/oss/qemu-machine-images/dist
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/builds/rock5b-usb/uboot}"
VERSION="${MACHINE_IMAGES_VERSION:-v1.3.0}"
CACHE_DIR="${QEMU_MACHINE_IMAGES_CACHE:-}"
MACHINE_IMAGES="${QEMU_MACHINE_IMAGES_DIR:-$HOME/oss/qemu-machine-images}"
EXPECTED="f1bb1d0e33712444877c32011fa0a067744ab2f52a8276f0950d7d3a195651f8"

mkdir -p "$OUT_DIR"

source_path=""
cached_dir="$MACHINE_IMAGES/.cache/releases/$VERSION/aarch64/rock-5b"
cached_uboot="$cached_dir/images/u-boot-dtb.bin"
if [ -n "$CACHE_DIR" ] && [ -f "$CACHE_DIR/u-boot-dtb.bin" ]; then
    source_path="$CACHE_DIR/u-boot-dtb.bin"
elif [ -f "$cached_uboot" ]; then
    source_path="$cached_uboot"
elif [ -f "$MACHINE_IMAGES/dist/aarch64-rock-5b-$VERSION.tar.zst" ]; then
    tmp="$(mktemp -d)"
    tar --zstd -xf "$MACHINE_IMAGES/dist/aarch64-rock-5b-$VERSION.tar.zst" \
        -C "$tmp" images/u-boot-dtb.bin
    cp "$tmp/images/u-boot-dtb.bin" "$OUT_DIR/u-boot-dtb.bin"
    rm -rf "$tmp"
    source_path="$OUT_DIR/u-boot-dtb.bin"
fi

if [ -z "$source_path" ]; then
    echo "could not find the rock-5b u-boot-dtb.bin image" >&2
    echo "set QEMU_MACHINE_IMAGES_CACHE to an extracted image directory, or" >&2
    echo "QEMU_MACHINE_IMAGES_DIR to a qemu-machine-images checkout" >&2
    exit 66
fi
if [ "$source_path" != "$OUT_DIR/u-boot-dtb.bin" ]; then
    cp "$source_path" "$OUT_DIR/u-boot-dtb.bin"
fi

if command -v sha256sum >/dev/null 2>&1; then
    digest="$(sha256sum "$OUT_DIR/u-boot-dtb.bin" | cut -d' ' -f1)"
else
    digest="$(shasum -a 256 "$OUT_DIR/u-boot-dtb.bin" | cut -d' ' -f1)"
fi
echo "u-boot-dtb.bin -> $OUT_DIR/u-boot-dtb.bin"
echo "sha256 $digest"
if [ "$digest" != "$EXPECTED" ]; then
    echo "warning: sha256 differs from the validated v1.3.0 image" >&2
    echo "expected $EXPECTED" >&2
    exit 1
fi
echo "next: scripts/rock5b-usb/make-zvisor-spi.py"

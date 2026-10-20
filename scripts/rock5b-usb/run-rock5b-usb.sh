#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot the real-board ZVisor on the QEMU rock-5b machine and expose the
# emulated Type-C USB CDC console on a local TCP serial bridge
# (127.0.0.1:17000 by default), like the real board PoC CDC bridge.
#
# Default flow: U-Boot (MMC/FAT) -> boot.scr -> ProcessONE payload staging
# -> bootm -> ZVisor -> USB CDC shell.  When the MMC artifacts are absent the
# script falls back to the fixed reference U-Boot + SPI-NOR env media (ZVisor
# boots without guest payloads).
#
# Environment overrides:
#   QEMU_BIN      qemu-system-aarch64 (default: builds/build-aarch64/...)
#   UBOOT_IMAGE   U-Boot proper u-boot-dtb.bin
#   SPI_IMAGE     SPI NOR image (env-only or MMC bootcmd)
#   SD_IMAGE      SD FAT payload image (empty string disables the SD card)
#   MACHINE       -machine value (default rock-5b,high-ram-base=0x400000000)
#   MEMORY        guest RAM size (default 8G)
#   NIC           -nic value (default user,model=rtl8125; empty disables)
#   TCP_PORT      USB CDC TCP bridge port (default 17000)
#   ATTACH=1      attach with socat after start (socat must exist)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU_BIN="${QEMU_BIN:-$REPO_ROOT/builds/build-aarch64/qemu-system-aarch64}"

MMC_UBOOT="$REPO_ROOT/builds/rock5b-usb/uboot-mmc/u-boot-dtb.bin"
MMC_SPI="$REPO_ROOT/builds/rock5b-usb/uboot-mmc/spi-nor-mmc.img"
MMC_SD="$REPO_ROOT/builds/rock5b-usb/sd-payload.img"
if [ -f "$MMC_UBOOT" ] && [ -f "$MMC_SPI" ]; then
    UBOOT_IMAGE="${UBOOT_IMAGE:-$MMC_UBOOT}"
    SPI_IMAGE="${SPI_IMAGE:-$MMC_SPI}"
    SD_IMAGE="${SD_IMAGE:-$MMC_SD}"
else
    UBOOT_IMAGE="${UBOOT_IMAGE:-$REPO_ROOT/builds/rock5b-usb/uboot/u-boot-dtb.bin}"
    SPI_IMAGE="${SPI_IMAGE:-$REPO_ROOT/builds/rock5b-usb/zvisor-spi-nor.img}"
    SD_IMAGE="${SD_IMAGE:-}"
fi
MACHINE="${MACHINE:-rock-5b,high-ram-base=0x400000000}"
MEMORY="${MEMORY:-8G}"
NIC="${NIC-user,model=rtl8125}"
TCP_PORT="${TCP_PORT:-17000}"
ATTACH="${ATTACH:-0}"
LOG_DIR="$REPO_ROOT/builds/rock5b-usb/logs"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
UBOOT_LOG="$LOG_DIR/uboot-uart2-$STAMP.log"

for required in "$QEMU_BIN" "$UBOOT_IMAGE" "$SPI_IMAGE"; do
    if [ ! -f "$required" ]; then
        echo "missing input: $required" >&2
        echo "run the scripts/rock5b-usb helpers first" \
             "(fetch-uboot-image, remote-build-zvisor, make-zvisor-spi," \
             "build-uboot-mmc, make-sd-payload)" >&2
        exit 66
    fi
done
mkdir -p "$LOG_DIR"

cleanup() {
    if [ -n "${QEMU_PID:-}" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup INT TERM EXIT

QEMU_ARGS=(
    -machine "$MACHINE" -smp 8 -m "$MEMORY"
    -display none -monitor none
    -chardev "socket,id=usbcon,host=127.0.0.1,port=$TCP_PORT,server=on,wait=off"
    -serial chardev:usbcon
    -serial "file:$UBOOT_LOG"
    -bios "$UBOOT_IMAGE"
    -drive "if=mtd,index=0,file=$SPI_IMAGE,format=raw,snapshot=on"
)
if [ -n "$SD_IMAGE" ] && [ -f "$SD_IMAGE" ]; then
    QEMU_ARGS+=(-drive "if=sd,index=2,file=$SD_IMAGE,format=raw")
fi
if [ -n "$NIC" ]; then
    QEMU_ARGS+=(-nic "$NIC")
fi

"$QEMU_BIN" "${QEMU_ARGS[@]}" &
QEMU_PID=$!

echo "QEMU rock-5b started (pid $QEMU_PID)"
echo "  machine: $MACHINE"
echo "  U-Boot: $UBOOT_IMAGE"
echo "  SPI NOR: $SPI_IMAGE"
if [ -n "$SD_IMAGE" ] && [ -f "$SD_IMAGE" ]; then
    echo "  SD card: $SD_IMAGE"
fi
if [ -n "$NIC" ]; then
    echo "  NIC: -nic $NIC"
fi
echo "  U-Boot UART2 log: $UBOOT_LOG"
echo "  ZVisor console (emulated Type-C USB CDC): TCP 127.0.0.1:$TCP_PORT"
echo "connect with: socat -,raw,echo=0 TCP:127.0.0.1:$TCP_PORT"
echo "or:           nc 127.0.0.1 $TCP_PORT"
echo "press Ctrl-C to stop QEMU"

if [ "$ATTACH" = "1" ]; then
    if ! command -v socat >/dev/null 2>&1; then
        echo "socat not found; connect manually" >&2
    else
        sleep 3
        socat -,raw,echo=0 "TCP:127.0.0.1:$TCP_PORT" || true
        echo "console detached; QEMU keeps running (Ctrl-C to stop)"
    fi
fi

wait "$QEMU_PID"

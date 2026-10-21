<!-- SPDX-License-Identifier: GPL-2.0-or-later
# ROCK 5B: U-Boot -> ZVisor over emulated Type-C USB CDC

This directory contains the host-side helpers for booting the real-board
ProcessONE ZVisor image on the QEMU rock-5b machine, with the ZVisor console
on the emulated Type-C USB CDC port instead of a UART.

The boot chain is the same as on the board, minus the parts QEMU does not
model:

    BootROM/SPL/BL31  (not modeled)
      -> U-Boot proper   fixed u-boot-dtb.bin, -bios at 0x00200000
      -> ZVisor          fixed real-board zvisor.img + zvisor.dtb, bootm
      -> console         Type-C USB CDC (DWC3 device mode + CDC ACM)

Nothing in U-Boot or ZVisor is modified.  Only the QEMU machine and the
16 MiB SPI NOR media contents are built here.

## Prerequisites

- This QEMU tree, configured and built for aarch64:

      ninja -C builds/build-aarch64 qemu-system-aarch64

- SSH access to the ProcessONE PM server, the same host as ~/remote-pm.sh
  (zevorn@devstation1.sh1.jcsmt.com), with the
  ~/work/project/processone-poc workspace and its builder container.
- A qemu-machine-images checkout or cache containing the aarch64 rock-5b
  v1.3.0 U-Boot/Linux image set (used for u-boot-dtb.bin).
- Python 3, tar with zstd support, and optionally socat for attaching to the
  USB CDC console.

## Quick start

    # 1. QEMU
    ninja -C builds/build-aarch64 qemu-system-aarch64

    # 2. fixed U-Boot proper image (qemu-machine-images v1.3.0)
    scripts/rock5b-usb/fetch-uboot-image.sh

    # 3. real-board ZVisor: rebuild on the remote PM server and copy it
    scripts/rock5b-usb/remote-build-zvisor.sh

    # 4. assemble the 16 MiB SPI NOR boot media
    scripts/rock5b-usb/make-zvisor-spi.py --manifest         builds/rock5b-usb/zvisor-spi-nor.manifest.json

    # 5. run QEMU and connect to the Type-C USB CDC console
    scripts/rock5b-usb/run-rock5b-usb.sh
    # in another terminal:
    socat -,raw,echo=0 TCP:127.0.0.1:17000

Wait for the ZVisor shell prompt (uart:~$), then try "zvisor status".

## Inputs and provenance

- builds/rock5b-usb/uboot/u-boot-dtb.bin: qemu-machine-images aarch64 rock-5b
  v1.3.0 U-Boot 2025.01 proper image, sha256 f1bb1d0e... (validated for the
  rock-5b machine; loaded at 0x00200000 by -bios).
- builds/rock5b-usb/zvisor/: zvisor.img (legacy uImage, load/entry
  0x10000000), zvisor.dtb, zephyr.elf, zvisor_system.json and
  build-manifest.json, produced by the documented remote build:

      ssh zevorn@devstation1.sh1.jcsmt.com
      cd ~/work/project/processone-poc
      just build          # == scripts/zephyr-build-container.sh host-sd

  The remote build is not modified by this workflow; the script records the
  git provenance and verifies every copied artifact against
  build-manifest.json.
- builds/rock5b-usb/zvisor-spi-nor.img: generated media.  Layout:

      0x000000  0x3f0000  erased
      0x3f0000  0x010000  U-Boot environment (crc32 + data)
      0x400000  ...       zvisor.img
      aligned   ...       zvisor.dtb

  The environment bootcmd is:

      sf probe 0:0 24000000 &&
      sf read 0x10000000 0x400000 <zvisor size> &&
      sf read 0x12000000 <dtb offset> <dtb size> &&
      bootm 0x10000000 - 0x12000000

## MMC/FAT SD payload boot (round 3)

The reference v1.3.0 U-Boot has MMC disabled and the SPI-only bootcmd cannot
load the ProcessONE guest payloads from an SD FAT image.  For the real-board
SD boot flow (`mmc dev 1` -> `fatload boot.scr` -> ProcessONE staging ->
`bootm`) this directory now also provides a QEMU-built MMC/FAT U-Boot and
the SD media:

    # 1. U-Boot 2025.01 proper with MMC/DW-MMC/FAT (reproducible, pinned
    #    source tarball; QEMU control DTS with sdmmc@fe2c0000 / aliases mmc1;
    #    Rockchip DDR ATAGS patch applied to the QEMU build tree only)
    scripts/rock5b-usb/build-uboot-mmc.sh
    #    -> builds/rock5b-usb/uboot-mmc/u-boot-dtb.bin (sha256 df2c47f4...)

    # 2. SD FAT media (256 MiB, MBR + FAT32) and the SPI NOR env image that
    #    makes U-Boot load boot.scr from the card (deterministic builder)
    scripts/rock5b-usb/make-sd-payload.py
    #    -> builds/rock5b-usb/sd-payload.img (sha256 38d7f43b...)
    #    -> builds/rock5b-usb/uboot-mmc/spi-nor-mmc.img (sha256 96c4b6d0...)
    #    real ProcessONE payloads instead of the marked placeholders:
    #    scripts/rock5b-usb/make-sd-payload.py --real-payloads DIR

    # 3. verify the MMC controller + FAT read path at the U-Boot prompt
    scripts/rock5b-usb/test-uboot-mmc.py

    # 4. run the full chain (U-Boot -> boot.scr -> ZVisor on USB CDC)
    QEMU=builds/build-aarch64/qemu-system-aarch64
    $QEMU -machine rock-5b,high-ram-base=0x400000000 -smp 8 -m 8G \
      -display none -monitor none \
      -chardev socket,id=usbcon,path=/tmp/rock5b-cdc.sock,server=on,wait=off \
      -serial chardev:usbcon -serial file:/tmp/rock5b-uart2.log \
      -bios builds/rock5b-usb/uboot-mmc/u-boot-dtb.bin \
      -drive if=mtd,index=0,file=builds/rock5b-usb/uboot-mmc/spi-nor-mmc.img,format=raw,snapshot=on \
      -drive if=sd,index=2,file=builds/rock5b-usb/sd-payload.img,format=raw
    # connect to the Type-C USB CDC console:
    socat -,raw,echo=0 UNIX-CONNECT:/tmp/rock5b-cdc.sock

The SD image contains the real remote-built zvisor.img/zvisor.dtb and the
real-board ProcessONE boot.scr staging contract.  Until the remote PM host is
reachable, the three guest/Linux payloads are clearly marked PLACEHOLDERS
(see PAYLOAD-PLACEHOLDERS.txt inside the FAT image and
evidence/firmware-media.md); ZVisor's payload descriptor expects the real
sizes, so guest VM admission needs the real payloads.  `sd_boot_mode=host`
in the U-Boot environment runs only the host staging path.  The
`high-ram-base=0x400000000` machine option is required for ZVisor's stage-2
high-memory guest windows.

## QEMU machine and serial layout

The machine is rock-5b, TCG only, eight CPUs, 8 GiB for the ZVisor image.
Serial slots:

    0  Type-C USB CDC console   (DWC3 device mode + CDC ACM bridge)
    1  UART2 debug header       (U-Boot console, log file)
    2+ UART1, UART3..UART9

run-rock5b-usb.sh exposes slot 0 as a local TCP serial bridge on
127.0.0.1:17000 (the same style of bridge used for the real board CDC) and
writes the U-Boot UART2 log under builds/rock5b-usb/logs/.

## Automated boot and console check

    # full chain: U-Boot MMC/FAT -> boot.scr -> ZVisor -> USB CDC shell
    # sends zvisor status / net iface / rtl8125 and records logs + summary
    scripts/rock5b-usb/boot-zvisor-usb.py

    # SPI-only reference media (no SD card, no guest payloads):
    scripts/rock5b-usb/boot-zvisor-usb.py --sd '' --nic '' \
        --bios builds/rock5b-usb/uboot/u-boot-dtb.bin \
        --spi builds/rock5b-usb/zvisor-spi-nor.img

The harness exits 0 once the ZVisor shell prompt is reached and writes
uboot-uart2.log, usb-cdc.log and summary.json under
builds/rock5b-usb/logs/zvisor-boot/.

## Environment variables

    QEMU_BIN                   qemu-system-aarch64 binary
    UBOOT_IMAGE                u-boot-dtb.bin (MMC or reference image)
    SPI_IMAGE                  SPI NOR image (env-only or MMC bootcmd)
    SD_IMAGE                   SD FAT payload image; empty disables the card
    MACHINE                    -machine value (default rock-5b,high-ram-base=0x400000000)
    MEMORY                     guest RAM (default 8G)
    NIC                        -nic value (default user,model=rtl8125; empty disables)
    TCP_PORT                   USB CDC TCP port (default 17000)
    ATTACH=1                   attach with socat after starting QEMU
    REMOTE_PM_TARGET           ssh target (default zevorn@devstation1...)
    REMOTE_PM_WORKSPACE        remote workspace (default work/project/processone-poc)
    QEMU_MACHINE_IMAGES_DIR    qemu-machine-images checkout
    QEMU_MACHINE_IMAGES_CACHE  extracted rock-5b image directory

## Files

    fetch-uboot-image.sh     install the fixed u-boot-dtb.bin and verify it
    remote-build-zvisor.sh   run just build remotely, copy and verify artifacts
    make-zvisor-spi.py       assemble and verify the 16 MiB SPI NOR image
    run-rock5b-usb.sh        start QEMU and expose the USB CDC console (TCP)
    boot-zvisor-usb.py       run the full chain, send shell commands, verify
    build-uboot-mmc.sh       build the MMC/FAT U-Boot 2025.01 proper for QEMU
    make-sd-payload.py       build the SD FAT payload image + SPI NOR env image
    test-uboot-mmc.py        drive/verify U-Boot mmc dev/fatload/crc32 over UART2
    uboot-mmc/               QEMU-only U-Boot fragment, control DTS and DDR patch

## Limitations

- BootROM, DDR training, SPL and BL31 are not modeled; U-Boot proper is
  entered directly from -bios.
- ZVisor boots only with the high-ram-base=0x400000000 machine option; its
  stage-2 guest windows are tied to that physical RAM bank.
- The SD media carries the real zvisor.img/zvisor.dtb and the real ProcessONE
  boot.scr staging, but the three guest/Linux payloads are placeholders when
  the remote build host is unavailable (make-sd-payload.py --real-payloads
  swaps them in). ZVisor still admits and auto-starts both VMs, but VM
  readiness requires the real payloads, so with placeholders the VM state is
  FAULTED while the host USB CDC shell is fully usable.
- The QEMU machine models PCIe2x1l2 and a functional RTL8125 endpoint (RX/TX
  descriptor rings, MSI, PHY link state) with a QEMU network backend; the PHY
  and MAC calibration values are synthetic and the device has no real
  EEPROM/PHY firmware.
- The modeled DWC3 CDC console buffers transmit data across the host socket
  connection and paces console input at 50 ms per byte; under heavy host
  CPU oversubscription the guest driver can still log rare endpoint-command
  deadline warnings.

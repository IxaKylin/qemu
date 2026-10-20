Radxa ROCK 5B (``rock-5b``)
===========================

The ``rock-5b`` machine models the Radxa ROCK 5B single-board computer,
which is built around the Rockchip RK3588 SoC.

Supported devices
-----------------

The ``rock-5b`` machine supports the following devices:

* 4 Cortex-A55 and 4 Cortex-A76 CPUs
* GICv3 with two ITS blocks, including LPIs and MSI
* the architectural timers
* UART1 through UART9
* Rockchip SFC with a 16 MiB SPI NOR flash (XTX XT25F128)
* Clock and reset unit (CRU); clock, soft-reset and vendor reset-status
  registers are modeled as storage registers with the Rockchip high-word
  write mask, including the reset read-back used by real firmware
* OTP controller
* Type-C USB port: DWC3 device-mode controller with a CDC ACM console bridge
* PCIe2x1l2 host bridge with an RTL8125 2.5GbE endpoint (RX/TX, MSI and
  PHY link state; attach a QEMU network backend with ``-nic``)
* SD/MMC controller (dw_mmc) with an SD card on ``if=sd,index=2``

The machine always uses all eight CPUs and supports 128 MiB to 16 GiB
of RAM, with 2 GiB as the default.

Boot options
------------

The ``rock-5b`` machine boots U-Boot proper when ``-bios`` is given and
Linux directly when ``-kernel`` is given; one of the two is required.
Both methods start the primary CPU in non-secure EL2 and provide PSCI
through SMC calls. DRAM and clocks are assumed to be initialized;
BootROM, DDR training firmware, SPL and BL31 are not modeled.

U-Boot proper is supplied as a raw binary with ``-bios``. QEMU loads it
at ``0x00200000``, so it must be linked for that address. The device tree
is taken from the Linux FIT image loaded by U-Boot, so ``-dtb`` cannot be
used with this method.

By default RAM above the 4 GiB peripheral hole is mapped at
``0x100000000``, matching the reference U-Boot/Linux image set. ZVisor's
fixed stage-2 guest windows require that bank at ``0x400000000``, so the
ZVisor boot adds ``-machine rock-5b,high-ram-base=0x400000000``. The
property may be used with either boot method.

Direct Linux boot supplies an AArch64 Linux ``Image`` with ``-kernel``
and a device tree with ``-dtb``; ``-initrd`` and ``-append`` are optional.
QEMU does not generate a device tree for this machine.

Running
-------

Serial ports are assigned as follows:

* ``-serial`` slot 0: the Type-C USB CDC console implemented by the DWC3
  device-mode controller (the console used by ZVisor firmware).
* ``-serial`` slot 1: UART2, the debug console on the board header and the
  console used by U-Boot and Linux.
* further ``-serial`` options connect to UART1 and UART3 through UART9 in
  that order.

Because UART2 is on slot 1, a Linux or U-Boot console on stdio is selected
with ``-serial null -serial mon:stdio``. The USB CDC console can be attached
to a host socket with a separate character backend, for example
``-chardev socket,id=usbcon,path=/tmp/rock5b-usb-cdc.sock,server=on,wait=off
-serial chardev:usbcon``; the host side is then a plain serial port over the
emulated Type-C connection.

U-Boot proper boot
~~~~~~~~~~~~~~~~~~

With ``u-boot-dtb.bin`` and ``spi-nor.img``:

.. code-block:: bash

   $ qemu-system-aarch64 \
       -machine rock-5b -m 2G -smp 8 \
       -display none -monitor none \
       -serial null -serial mon:stdio \
       -bios u-boot-dtb.bin \
       -drive if=mtd,index=0,file=spi-nor.img,format=raw

The SPI NOR image must be exactly 16 MiB. Add ``snapshot=on`` to the
drive option to discard guest writes.

ZVisor boot from SD over the Type-C USB CDC console
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The machine boots the unmodified ZVisor firmware built for the real board
with a U-Boot that can read the ProcessONE SD card layout: U-Boot scans the
modeled ``dw_mmc`` controller, loads ``boot.scr``, stages the payloads in
DRAM and enters ZVisor with ``bootm``. ZVisor's console and shell then
appear on the emulated Type-C USB CDC port. An RTL8125 user-mode network is
attached with ``-nic`` for its DHCP/network stack:

.. code-block:: bash

   $ qemu-system-aarch64 \
       -machine rock-5b,high-ram-base=0x400000000 -m 8G -smp 8 \
       -display none -monitor none \
       -chardev socket,id=usbcon,path=/tmp/rock5b-usb-cdc.sock,server=on,wait=off \
       -serial chardev:usbcon \
       -serial file:uboot-uart2.log \
       -bios u-boot-dtb.bin \
       -drive if=mtd,index=0,file=spi-nor-mmc.img,format=raw,snapshot=on \
       -drive if=sd,index=2,file=sd-payload.img,format=raw \
       -nic user,model=rtl8125

Connect the host side with ``socat - UNIX-CONNECT:/tmp/rock5b-usb-cdc.sock``
and wait for the ``uart:~$`` prompt. See ``scripts/rock5b-usb/`` for the
remote-build, U-Boot, SD-media and boot helpers used to produce and run
these images.

Direct Linux boot
~~~~~~~~~~~~~~~~~

With ``Image``, ``rootfs.cpio.gz`` and ``rk3588-rock-5b.dtb``:

.. code-block:: bash

   $ qemu-system-aarch64 \
       -machine rock-5b -m 2G -smp 8 \
       -display none -monitor none \
       -serial null -serial mon:stdio \
       -kernel Image -initrd rootfs.cpio.gz -dtb rk3588-rock-5b.dtb \
       -append "console=ttyS2,1500000n8 rdinit=/init"

Limitations
-----------

The machine is TCG-only; KVM is not supported. BootROM, DDR training,
SPL and BL31 are not modeled; firmware boot starts U-Boot proper directly.
The remaining RK3588 peripherals are unimplemented. ZVisor boot requires
the ``high-ram-base=0x400000000`` option because its stage-2 guest windows
are tied to that physical RAM bank; its virtual machines only run with the
matching real ProcessONE guest payloads staged by U-Boot (the SD helper can
use placeholders when the remote build host is unavailable). The SD
controller supports one card on ``if=sd,index=2`` and the RTL8125 has no
PHY/MAC calibration data (link state and packet path are modeled, the PHY
data values are synthetic). The modeled DWC3 CDC console buffers transmit
data across host connection establishment and paces console input so that
the firmware's line-coding control bridge does not coalesce characters;
under heavy host CPU oversubscription the guest can still log rare
endpoint-command deadline warnings. Migration and VM snapshots are
unsupported.

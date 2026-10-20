/*
 * Synopsys DWC3 USB device-mode controller (UDC) with CDC ACM bridge
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_DWC3_UDC_H
#define HW_USB_DWC3_UDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/*
 * Generic DWC3 device-mode controller.  The device implements the DWC3
 * register/TRB/event interface used by a device-mode UDC driver and exposes
 * CDC ACM bulk IN data through its "chardev" property.
 *
 * The controller is parameterised by nothing but its chardev: the MMIO window
 * size is fixed by the IP, and the IRQ is a single level-sensitive output.
 * SoC and board code own the address mapping, IRQ wiring and chardev choice.
 */
#define TYPE_DWC3_UDC "dwc3-udc"
OBJECT_DECLARE_SIMPLE_TYPE(DWC3UDCState, DWC3_UDC)

/* Full register window of the DWC3 instance (only 64 KiB is implemented). */
#define DWC3_UDC_MMIO_SIZE 0x400000

#endif /* HW_USB_DWC3_UDC_H */

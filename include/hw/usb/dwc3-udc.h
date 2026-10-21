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

#define TYPE_DWC3_UDC "dwc3-udc"
OBJECT_DECLARE_SIMPLE_TYPE(DWC3UDCState, DWC3_UDC)

/* Full register window of the DWC3 instance (only 64 KiB is implemented). */
#define DWC3_UDC_MMIO_SIZE 0x400000

#endif /* HW_USB_DWC3_UDC_H */

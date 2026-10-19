/*
 * SiFli SF32LB52X SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SF32LB52X_H
#define HW_ARM_SF32LB52X_H

#include "hw/arm/armv7m.h"
#include "hw/core/clock.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SF32LB52X_SOC "sifli.sf32lb52x.soc"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XSoCState, SF32LB52X_SOC)

#define SF32LB52X_NUM_IRQS      (100)

/* The HCPU runs from the DLL1 output at 240 MHz. */
#define SF32LB52X_SYSCLK_HZ     (240 * 1000 * 1000)

/* External QSPI NOR on MPI2, accessed through the MPI2 XIP window. */
#define SF32LB52X_FLASH_BASE    0x12000000
#define SF32LB52X_FLASH_SIZE    (16 * 1024 * 1024)
#define SF32LB52X_CODE_BASE     0x12010000

/* HPSYS SRAM and the shared SRAM window used for inter-core data. */
#define SF32LB52X_SRAM_BASE     0x20000000
#define SF32LB52X_SRAM_SIZE     (512 * 1024)
#define SF32LB52X_SRAM1_BASE    0x20400000
#define SF32LB52X_SRAM1_SIZE    (64 * 1024)

struct SF32LB52XSoCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMv7MState armv7m;
    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion sram1;

    MemoryRegion *memory; /* system memory, linked by the board */
    Clock *sysclk;        /* input clock from the board, forwarded to the CPU */
};

#endif /* HW_ARM_SF32LB52X_H */

/*
 * Rockchip RK3588 SoC
 *
 * Copyright (C) 2026 Process Mission
 *
 * Authors:
 *    Chao Liu <chao.liu@processmission.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef HW_ARM_RK3588_H
#define HW_ARM_RK3588_H

#include "qom/object.h"
#include "target/arm/cpu.h"
#include "hw/char/dw8250.h"
#include "hw/core/qdev.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "system/memory.h"

#define TYPE_RK3588_SOC "rockchip.rk3588.soc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588SoCState, RK3588_SOC)

#define RK3588_A55_CPUS              (4)
#define RK3588_A76_CPUS_PER_CLUSTER  (2)

typedef enum RK3588CpuClusterId {
    RK3588_CLUSTER_A55,
    RK3588_CLUSTER_A76_0,
    RK3588_CLUSTER_A76_1,
    RK3588_CLUSTER_COUNT,
} RK3588CpuClusterId;

#define RK3588_NUM_CPUS (RK3588_A55_CPUS + \
    RK3588_A76_CPUS_PER_CLUSTER * (RK3588_CLUSTER_COUNT - 1))

/* UART0 in the PMU power domain is reserved; UART1-UART9 are modeled. */
#define RK3588_NUM_UARTS             (10)

/* Indices into rk3588_memmap[], see hw/arm/rk3588.c. */
enum {
    RK3588_DEV_GIC_DIST,
    RK3588_DEV_GIC_REDIST,
    RK3588_DEV_GIC_ITS0,
    RK3588_DEV_GIC_ITS1,
    RK3588_DEV_CRU,
    RK3588_DEV_OTP,
    RK3588_DEV_SFC,
    RK3588_DEV_SDMMC,
    RK3588_DEV_USB3OTG0,
    RK3588_DEV_PCIE2X1L2_APB,
    RK3588_DEV_PCIE2X1L2_DBI,
    RK3588_DEV_UART1,
    RK3588_DEV_UART2,
    RK3588_DEV_UART3,
    RK3588_DEV_UART4,
    RK3588_DEV_UART5,
    RK3588_DEV_UART6,
    RK3588_DEV_UART7,
    RK3588_DEV_UART8,
    RK3588_DEV_UART9,
};

struct RK3588SoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    ARMCPU cpu[RK3588_NUM_CPUS];
    CPUClusterState cluster[RK3588_CLUSTER_COUNT];
    GICv3State gic;
    DeviceState *its[2];
    DW8250State uart[RK3588_NUM_UARTS];
    DeviceState *usb3otg0;
    DeviceState *pcie2x1l2;
    DeviceState *cru;
    DeviceState *otp;
    DeviceState *sfc;
    DeviceState *sdmmc;
    MemoryRegion *memory;
};

#endif /* HW_ARM_RK3588_H */

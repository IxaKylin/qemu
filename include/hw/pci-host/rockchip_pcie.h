/*
 * Rockchip DesignWare PCIe host wrapper
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The RK3588 PCIe controllers are DesignWare core instances with a Rockchip
 * "PCIe client" register file (PCIE_CLIENT_*) in a separate APB window.  This
 * device subclasses TYPE_DESIGNWARE_PCIE_HOST, which already implements the
 * DWC Type-0 header / Port Logic / iATU viewport / legacy-MSI block, and adds:
 *
 *   - the Rockchip APB vendor register window (sysbus mmio[1]).  Only
 *     PCIE_CLIENT_LTSSM_STATUS (0x300) is load-bearing: it reports L0/link-up
 *     while the link-up property is set.  Every other APB offset is RAZ/WI and
 *     logged as unimplemented so the RK3588 driver's PCIE_CLIENT_* writes
 *     (GENERAL_CON RC mode / LTSSM enable, HOT_RESET_CTRL, POWER_CON,
 *     INTR_MASK_LEGACY) land without aborting.  Real hardware latches these
 *     registers; they have no behavioral effect in the substitution model.
 *
 *   - a device-owned RAZ/WI region covering the DBI window beyond the 4 KiB
 *     DesignWare core header (sysbus mmio[2], mapped at DBI + 0x1000).  The
 *     RK3588 DBI window is 4 MiB and includes DBI2 at +0x100000; the RK3588
 *     driver writes DBI2 BARs during root setup, so that address range must be
 *     mapped on AArch64 (an unassigned access aborts the CPU).
 *
 *   - three inert Rockchip interrupt lines (err / pmc / sys) on top of the
 *     parent's INTA..INTD and MSI sysbus IRQ outputs.
 *
 * The board remains responsible for mapping the SoC-specific DBI, APB, CFG and
 * memory windows and for wiring the IRQs.  The CFG window is served by the
 * DesignWare outbound CFG iATU viewports: the guest programs a viewport and
 * designware.c maps it at the programmed base, so no static alias is needed.
 */

#ifndef HW_PCI_HOST_ROCKCHIP_PCIE_H
#define HW_PCI_HOST_ROCKCHIP_PCIE_H

#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/designware.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_PCIE_HOST "rockchip-pcie-host"
OBJECT_DECLARE_TYPE(RockchipPCIEHost, RockchipPCIEHostClass,
                    ROCKCHIP_PCIE_HOST)

/*
 * SysBus IRQ indices exported by this device, in declaration order.  The
 * parent DesignWare host exports 0..3 (INTA..INTD) and 4 (MSI); the wrapper
 * appends three Rockchip-only lines.
 */
#define ROCKCHIP_PCIE_MSG_IRQ    4
#define ROCKCHIP_PCIE_ERR_IRQ    5
#define ROCKCHIP_PCIE_PMC_IRQ    6
#define ROCKCHIP_PCIE_SYS_IRQ    7
#define ROCKCHIP_PCIE_NUM_IRQS   8

/* DBI window layout: 4 KiB DesignWare core + RAZ/WI tail up to 4 MiB. */
#define ROCKCHIP_PCIE_DBI_CORE_SIZE 0x1000
#define ROCKCHIP_PCIE_DBI_SIZE      0x400000
#define ROCKCHIP_PCIE_DBI_TAIL_SIZE \
    (ROCKCHIP_PCIE_DBI_SIZE - ROCKCHIP_PCIE_DBI_CORE_SIZE)

/* APB vendor register offsets (PCIE_CLIENT_*). */
#define ROCKCHIP_PCIE_APB_LTSSM_STATUS 0x300

struct RockchipPCIEHost {
    DesignwarePCIEHost parent_obj;

    MemoryRegion apb;
    MemoryRegion dbi_tail;
    bool link_up;
    uint32_t domain;
    char root_bus_path[8];

    qemu_irq err_irq;
    qemu_irq pmc_irq;
    qemu_irq sys_irq;
};

struct RockchipPCIEHostClass {
    PCIHostBridgeClass parent_class;

    DeviceRealize parent_realize;
};

/* Flip the LTSSM link status reported in the APB window. */
void rockchip_pcie_host_set_link_up(DeviceState *dev, bool up);

/*
 * Secondary bus of the root port, for plugging the link partner endpoint.
 * Returns NULL if the host has not been realized.
 */
PCIBus *rockchip_pcie_host_secondary_bus(DeviceState *dev);

#endif /* HW_PCI_HOST_ROCKCHIP_PCIE_H */

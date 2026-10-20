/*
 * Rockchip DesignWare PCIe host wrapper
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See include/hw/pci-host/rockchip_pcie.h for the modeled contract.
 */

#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci-host/rockchip_pcie.h"

/*
 * PCIE_CLIENT_LTSSM_STATUS (APB 0x300).  Bits 17:16 are the link-up status
 * (0b11 = up); bits 5:0 carry the LTSSM state (0x11 = L0).  The RK3588
 * pcie_dw_rockchip driver polls exactly (BIT(16) | BIT(17)).
 */
#define ROCKCHIP_PCIE_LTSSM_LINK_UP 0x00030011u

static uint64_t rockchip_pcie_apb_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    RockchipPCIEHost *s = opaque;

    switch (offset) {
    case ROCKCHIP_PCIE_APB_LTSSM_STATUS:
        return s->link_up ? ROCKCHIP_PCIE_LTSSM_LINK_UP : 0;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "rockchip-pcie: unimplemented APB read at 0x%"
                      HWADDR_PRIx " size %u\n", offset, size);
        return 0;
    }
}

static void rockchip_pcie_apb_write(void *opaque, hwaddr offset, uint64_t val,
                                    unsigned size)
{
    /*
     * GENERAL_CON (RC mode / LTSSM enable), HOT_RESET_CTRL, POWER_CON and
     * INTR_MASK_LEGACY are write-only control registers from the driver's
     * point of view.  They have no behavioral effect in the substitution
     * model, but the writes MUST land: an AArch64 access to an unassigned
     * address aborts the CPU.
     */
    qemu_log_mask(LOG_UNIMP,
                  "rockchip-pcie: unimplemented APB write at 0x%"
                  HWADDR_PRIx " size %u value 0x%" PRIx64 "\n",
                  offset, size, val);
}

static const MemoryRegionOps rockchip_pcie_apb_ops = {
    .read = rockchip_pcie_apb_read,
    .write = rockchip_pcie_apb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static uint64_t rockchip_pcie_dbi_tail_read(void *opaque, hwaddr offset,
                                            unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "rockchip-pcie: unimplemented DBI read at 0x%" HWADDR_PRIx
                  " size %u\n",
                  ROCKCHIP_PCIE_DBI_CORE_SIZE + offset, size);
    return 0;
}

static void rockchip_pcie_dbi_tail_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "rockchip-pcie: unimplemented DBI write at 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 "\n",
                  ROCKCHIP_PCIE_DBI_CORE_SIZE + offset, size, value);
}

static const MemoryRegionOps rockchip_pcie_dbi_tail_ops = {
    .read = rockchip_pcie_dbi_tail_read,
    .write = rockchip_pcie_dbi_tail_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rockchip_pcie_host_realize(DeviceState *dev, Error **errp)
{
    RockchipPCIEHost *s = ROCKCHIP_PCIE_HOST(dev);
    RockchipPCIEHostClass *rkpc = ROCKCHIP_PCIE_HOST_GET_CLASS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    snprintf(s->root_bus_path, sizeof(s->root_bus_path), "%04x:%02x",
             s->domain, s->parent_obj.bus_nr);

    /* Realize the DesignWare parent first: it registers sysbus mmio[0] and
     * the five IRQ outputs (INTA..INTD, legacy MSI). */
    rkpc->parent_realize(dev, errp);
    if (errp && *errp) {
        return;
    }

    sysbus_init_irq(sbd, &s->err_irq);
    sysbus_init_irq(sbd, &s->pmc_irq);
    sysbus_init_irq(sbd, &s->sys_irq);

    memory_region_init_io(&s->apb, OBJECT(s), &rockchip_pcie_apb_ops, s,
                          "rockchip-pcie-apb", 0x10000);
    sysbus_init_mmio(sbd, &s->apb);

    memory_region_init_io(&s->dbi_tail, OBJECT(s),
                          &rockchip_pcie_dbi_tail_ops, s,
                          "rockchip-pcie-dbi-tail",
                          ROCKCHIP_PCIE_DBI_TAIL_SIZE);
    sysbus_init_mmio(sbd, &s->dbi_tail);
}

void rockchip_pcie_host_set_link_up(DeviceState *dev, bool up)
{
    RockchipPCIEHost *s = ROCKCHIP_PCIE_HOST(dev);

    s->link_up = up;
}

PCIBus *rockchip_pcie_host_secondary_bus(DeviceState *dev)
{
    RockchipPCIEHost *s = ROCKCHIP_PCIE_HOST(dev);

    return PCI_BUS(qdev_get_child_bus(DEVICE(&s->parent_obj.root),
                                      "dw-pcie"));
}

static const char *rockchip_pcie_host_root_bus_path(
    PCIHostState *host_bridge, PCIBus *rootbus)
{
    RockchipPCIEHost *s = ROCKCHIP_PCIE_HOST(host_bridge);

    return s->root_bus_path;
}

/* Preserve the dynamic link state across migration. */
static const VMStateDescription vmstate_rockchip_pcie_host = {
    .name = "rockchip-pcie-host",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, RockchipPCIEHost, 1,
                       vmstate_designware_pcie_host, DesignwarePCIEHost),
        VMSTATE_BOOL(link_up, RockchipPCIEHost),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rockchip_pcie_host_properties[] = {
    DEFINE_PROP_BOOL("link-up", RockchipPCIEHost, link_up, true),
    DEFINE_PROP_UINT32("domain", RockchipPCIEHost, domain, 0),
};

static void rockchip_pcie_host_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    RockchipPCIEHostClass *rkpc = ROCKCHIP_PCIE_HOST_CLASS(klass);

    device_class_set_parent_realize(dc, rockchip_pcie_host_realize,
                                    &rkpc->parent_realize);
    device_class_set_props(dc, rockchip_pcie_host_properties);
    dc->vmsd = &vmstate_rockchip_pcie_host;
    hc->root_bus_path = rockchip_pcie_host_root_bus_path;
    dc->user_creatable = false;
}

static const TypeInfo rockchip_pcie_host_info = {
    .name = TYPE_ROCKCHIP_PCIE_HOST,
    .parent = TYPE_DESIGNWARE_PCIE_HOST,
    .instance_size = sizeof(RockchipPCIEHost),
    .class_init = rockchip_pcie_host_class_init,
};

static void rockchip_pcie_register_types(void)
{
    type_register_static(&rockchip_pcie_host_info);
}

type_init(rockchip_pcie_register_types)

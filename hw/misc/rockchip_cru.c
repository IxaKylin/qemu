/*
 * Rockchip RK3588 clock registers
 *
 * Copyright (C) 2026 Process Mission
 *
 * Authors:
 *    Chao Liu <chao.liu@processmission.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_CRU "rockchip-cru"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipCRUState, ROCKCHIP_CRU)

#define ROCKCHIP_CRU_SIZE 0x5c000
#define ROCKCHIP_CRU_NUM_REGS (9 * 4 + 10)
#define ROCKCHIP_CRU_NUM_WORDS (ROCKCHIP_CRU_SIZE / 4)

struct RockchipCRUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    RegisterInfo regs_info[ROCKCHIP_CRU_NUM_REGS];
    uint32_t regs[ROCKCHIP_CRU_NUM_REGS];
    /*
     * Every other CRU register, including the clock gates, soft resets and
     * the vendor reset-status banks, is a plain storage register with the
     * Rockchip high-word write mask.  U-Boot and ZVisor program these banks
     * and read them back (for example reset 0x30a04 must read 0x3000 after
     * a 0x30003000 write), so ignoring the writes leaves real firmware in an
     * inconsistent state.
     */
    uint32_t generic[ROCKCHIP_CRU_NUM_WORDS];
};

#define PLL_POWER_DOWN BIT(13)
#define PLL_LOCKED BIT(15)

static uint64_t rockchip_cru_masked_write(RegisterInfo *reg, uint64_t value)
{
    uint32_t old = *(uint32_t *)reg->data;
    uint16_t mask = value >> 16;

    return (old & ~mask) | (value & mask);
}

static uint64_t rockchip_cru_pll_lock(RegisterInfo *reg, uint64_t value)
{
    RockchipCRUState *s = ROCKCHIP_CRU(reg->opaque);
    size_t index = reg - s->regs_info;
    /* Each PLL entry contains CON0, CON1, CON2 and CON6, in that order. */
    bool powered = !(s->regs[index - 2] & PLL_POWER_DOWN);
    bool configured = (s->regs[index - 3] & 0x3ff) &&
                      (s->regs[index - 2] & 0x3f);

    /* PLL settling is synchronous after configuration and power-up. */
    return powered && configured ? PLL_LOCKED : 0;
}

#define CRU_PLL(label, base) \
    { \
    { .name = label "_CON0", .addr = base, \
      .pre_write = rockchip_cru_masked_write }, \
    { .name = label "_CON1", .addr = base + 4, .reset = PLL_POWER_DOWN, \
      .pre_write = rockchip_cru_masked_write }, \
    { .name = label "_CON2", .addr = base + 8, \
      .pre_write = rockchip_cru_masked_write }, \
    { .name = label "_CON6", .addr = base + 0x18, .ro = UINT32_MAX, \
      .post_read = rockchip_cru_pll_lock } \
    }

#define CRU_CLOCK(label, offset) \
    { .name = label, .addr = offset, .pre_write = rockchip_cru_masked_write }

static const RegisterAccessInfo rockchip_cru_pll_regs[][4] = {
    CRU_PLL("B0PLL", 0x50000),
    CRU_PLL("B1PLL", 0x52020),
    CRU_PLL("LPLL", 0x58040),
    CRU_PLL("V0PLL", 0x00160),
    CRU_PLL("AUPLL", 0x00180),
    CRU_PLL("CPLL", 0x001a0),
    CRU_PLL("GPLL", 0x001c0),
    CRU_PLL("NPLL", 0x001e0),
    CRU_PLL("PPLL", 0x08200),
};

static const RegisterAccessInfo rockchip_cru_clock_regs[] = {
    CRU_CLOCK("MODE_CON0", 0x00280),
    CRU_CLOCK("CLKSEL_CON8", 0x00320),
    CRU_CLOCK("CLKSEL_CON9", 0x00324),
    CRU_CLOCK("CLKSEL_CON38", 0x00398),
    CRU_CLOCK("CLKSEL_CON78", 0x00438),
    CRU_CLOCK("CLKSEL_CON165", 0x00594),
    CRU_CLOCK("CLKGATE_CON19", 0x0084c),
    CRU_CLOCK("BIGCORE0_MODE_CON0", 0x50280),
    CRU_CLOCK("BIGCORE1_MODE_CON0", 0x52280),
    CRU_CLOCK("DSU_MODE_CON0", 0x58280),
};

G_STATIC_ASSERT(ARRAY_SIZE(rockchip_cru_pll_regs) * 4 +
                ARRAY_SIZE(rockchip_cru_clock_regs) == ROCKCHIP_CRU_NUM_REGS);

static RegisterInfo *rockchip_cru_register(RockchipCRUState *s, hwaddr offset)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        if (s->regs_info[i].access->addr == offset) {
            return &s->regs_info[i];
        }
    }
    return NULL;
}

static uint64_t rockchip_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipCRUState *s = opaque;
    RegisterInfo *reg = rockchip_cru_register(s, offset);

    if (reg) {
        return register_read(reg, UINT32_MAX, TYPE_ROCKCHIP_CRU, false);
    }
    if ((offset & (sizeof(uint32_t) - 1)) == 0 &&
        offset + sizeof(uint32_t) <= ROCKCHIP_CRU_SIZE) {
        return s->generic[offset / sizeof(uint32_t)];
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rockchip-cru: invalid read at 0x%" HWADDR_PRIx
                  " size %u\n", offset, size);
    return 0;
}

static void rockchip_cru_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RockchipCRUState *s = opaque;
    RegisterInfo *reg = rockchip_cru_register(s, offset);

    if (reg) {
        register_write(reg, value, UINT32_MAX, TYPE_ROCKCHIP_CRU, false);
        return;
    }
    if ((offset & (sizeof(uint32_t) - 1)) == 0 &&
        offset + sizeof(uint32_t) <= ROCKCHIP_CRU_SIZE) {
        uint32_t *slot = &s->generic[offset / sizeof(uint32_t)];
        uint16_t mask = value >> 16;

        /* Rockchip high-word write mask: only masked low bits are written. */
        *slot = (*slot & ~mask) | (value & mask);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rockchip-cru: invalid write at 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 " size %u\n", offset, value, size);
}

static const MemoryRegionOps rockchip_cru_ops = {
    .read = rockchip_cru_read,
    .write = rockchip_cru_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rockchip_cru_reset(DeviceState *dev)
{
    RockchipCRUState *s = ROCKCHIP_CRU(dev);

    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    memset(s->generic, 0, sizeof(s->generic));
}

static void rockchip_cru_init(Object *obj)
{
    RockchipCRUState *s = ROCKCHIP_CRU(obj);

    /* Keep sparse CRU addresses in a compact RegisterInfo array. */
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        const RegisterAccessInfo *access;
        unsigned int pll_regs = ARRAY_SIZE(rockchip_cru_pll_regs) * 4;

        if (i < pll_regs) {
            access = &rockchip_cru_pll_regs[i / 4][i % 4];
        } else {
            access = &rockchip_cru_clock_regs[i - pll_regs];
        }
        s->regs_info[i] = (RegisterInfo) {
            .data = &s->regs[i],
            .data_size = sizeof(s->regs[i]),
            .access = access,
            .opaque = s,
        };
    }
    memory_region_init_io(&s->iomem, obj, &rockchip_cru_ops, s,
                          TYPE_ROCKCHIP_CRU, ROCKCHIP_CRU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void rockchip_cru_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rockchip_cru_reset);
}

static const TypeInfo rockchip_cru_type_info = {
    .name = TYPE_ROCKCHIP_CRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipCRUState),
    .instance_init = rockchip_cru_init,
    .class_init = rockchip_cru_class_init,
};

static void rockchip_cru_register_types(void)
{
    type_register_static(&rockchip_cru_type_info);
}

type_init(rockchip_cru_register_types)

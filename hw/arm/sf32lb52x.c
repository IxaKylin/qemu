/*
 * SiFli SF32LB52X SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/sf32lb52x.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SF32LB52X_NUM_PRIO_BITS  3

static void sf32lb52x_soc_instance_init(Object *obj)
{
    SF32LB52XSoCState *s = SF32LB52X_SOC(obj);
    int i;

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk", NULL, NULL, 0);
}

static void sf32lb52x_soc_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    SF32LB52XSoCState *s = SF32LB52X_SOC(dev);
    DeviceState *armv7m;
    int i;

    if (!s->memory) {
        error_setg(errp, "sf32lb52x: memory link is required");
        return;
    }

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sf32lb52x: sysclk must be connected");
        return;
    }

    if (!memory_region_init_rom(&s->flash, OBJECT(dev), "sf32lb52x.flash",
                                SF32LB52X_FLASH_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->memory, SF32LB52X_FLASH_BASE, &s->flash);

    if (!memory_region_init_ram(&s->sram, OBJECT(dev), "sf32lb52x.sram",
                                SF32LB52X_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->memory, SF32LB52X_SRAM_BASE, &s->sram);

    if (!memory_region_init_ram(&s->sram1, OBJECT(dev), "sf32lb52x.sram1",
                                SF32LB52X_SRAM1_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->memory, SF32LB52X_SRAM1_BASE, &s->sram1);

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", SF32LB52X_NUM_IRQS);
    qdev_prop_set_uint32(armv7m, "num-prio-bits", SF32LB52X_NUM_PRIO_BITS);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_uint32(armv7m, "init-svtor", SF32LB52X_CODE_BASE);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(s->memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

}

static const Property sf32lb52x_soc_properties[] = {
    DEFINE_PROP_LINK("memory", SF32LB52XSoCState, memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription vmstate_sf32lb52x_soc = {
    .name = TYPE_SF32LB52X_SOC,
    .unmigratable = true,
};

static void sf32lb52x_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = sf32lb52x_soc_realize;
    dc->desc = "SiFli SF32LB52X";
    dc->vmsd = &vmstate_sf32lb52x_soc;
    dc->user_creatable = false;
    device_class_set_props(dc, sf32lb52x_soc_properties);
}

static const TypeInfo sf32lb52x_soc_types[] = {
    {
        .name = TYPE_SF32LB52X_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XSoCState),
        .instance_init = sf32lb52x_soc_instance_init,
        .class_init = sf32lb52x_soc_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_soc_types);

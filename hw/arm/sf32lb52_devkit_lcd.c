/*
 * SiFli SF32LB52-DevKit-LCD board
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/sf32lb52x.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/qtest.h"
#include "system/system.h"

#define TYPE_SF32LB52_DEVKIT_LCD MACHINE_TYPE_NAME("sf32lb52-devkit-lcd")
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52DevKitLCDState, SF32LB52_DEVKIT_LCD)

struct SF32LB52DevKitLCDState {
    MachineState parent;

    SF32LB52XSoCState soc;
    Clock *sysclk;
};

/*
 * The Zephyr image is linked for the code partition at 0x12010000 and
 * runs in place from the QSPI NOR window. The SiFli boot ROM and its
 * flash configuration table are not modeled; the CPU takes the reset
 * SP and PC from the image vector table instead.
 */
static void sf32lb52_devkit_lcd_init(MachineState *machine)
{
    SF32LB52DevKitLCDState *s = SF32LB52_DEVKIT_LCD(machine);
    int i;

    if (!machine->kernel_filename && !qtest_enabled()) {
        error_report("sf32lb52-devkit-lcd: a -kernel image is required");
        exit(1);
    }

    s->sysclk = clock_new(OBJECT(machine), "sysclk");
    clock_set_hz(s->sysclk, SF32LB52X_SYSCLK_HZ);

    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_SF32LB52X_SOC);
    object_property_set_link(OBJECT(&s->soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    qdev_connect_clock_in(DEVICE(&s->soc), "sysclk", s->sysclk);
    for (i = 0; i < SF32LB52X_NUM_USARTS; i++) {
        g_autofree char *name = g_strdup_printf("usart%d-chardev", i + 1);

        qdev_prop_set_chr(DEVICE(&s->soc), name, serial_hd(i));
    }
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    armv7m_load_kernel(s->soc.armv7m.cpu, machine->kernel_filename,
                       SF32LB52X_CODE_BASE,
                       SF32LB52X_FLASH_SIZE -
                       (SF32LB52X_CODE_BASE - SF32LB52X_FLASH_BASE));
}

static void sf32lb52_devkit_lcd_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"),
        NULL
    };

    mc->desc = "SiFli SF32LB52-DevKit-LCD (Cortex-M33)";
    mc->init = sf32lb52_devkit_lcd_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
}

static const TypeInfo sf32lb52_devkit_lcd_types[] = {
    {
        .name = TYPE_SF32LB52_DEVKIT_LCD,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(SF32LB52DevKitLCDState),
        .class_init = sf32lb52_devkit_lcd_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(sf32lb52_devkit_lcd_types);

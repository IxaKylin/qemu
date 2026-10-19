/*
 * SiFli SF32LB USART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/sf32lb_usart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void sf32lb_usart_update_irq(SF32LBUSARTState *s)
{
    uint32_t pending = s->isr & s->cr1 &
        (SF32LB_USART_CR1_RXNEIE | SF32LB_USART_CR1_TCIE |
         SF32LB_USART_CR1_TXEIE);

    qemu_set_irq(s->irq, pending != 0);
}

static int sf32lb_usart_can_receive(void *opaque)
{
    SF32LBUSARTState *s = opaque;

    return !(s->isr & SF32LB_USART_ISR_RXNE);
}

static void sf32lb_usart_receive(void *opaque, const uint8_t *buf, int size)
{
    SF32LBUSARTState *s = opaque;

    if (!(s->cr1 & SF32LB_USART_CR1_UE) || !(s->cr1 & SF32LB_USART_CR1_RE)) {
        return;
    }

    s->rdr = buf[0];
    s->isr |= SF32LB_USART_ISR_RXNE;

    sf32lb_usart_update_irq(s);
}

static uint64_t sf32lb_usart_read(void *opaque, hwaddr addr,
                                  unsigned int size)
{
    SF32LBUSARTState *s = opaque;
    uint64_t value;

    switch (addr) {
    case SF32LB_USART_CR1:
        value = s->cr1;
        break;
    case SF32LB_USART_CR2:
        value = s->cr2;
        break;
    case SF32LB_USART_CR3:
        value = s->cr3;
        break;
    case SF32LB_USART_BRR:
        value = s->brr;
        break;
    case SF32LB_USART_ISR:
        value = s->isr;
        break;
    case SF32LB_USART_RDR:
        value = s->rdr;
        s->isr &= ~SF32LB_USART_ISR_RXNE;
        qemu_chr_fe_accept_input(&s->chr);
        sf32lb_usart_update_irq(s);
        break;
    case SF32LB_USART_MISCR:
        value = s->miscr;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        value = 0;
        break;
    }

    return value;
}

static void sf32lb_usart_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned int size)
{
    SF32LBUSARTState *s = opaque;
    uint8_t ch;

    switch (addr) {
    case SF32LB_USART_CR1:
        s->cr1 = value;
        break;
    case SF32LB_USART_CR2:
        s->cr2 = value;
        break;
    case SF32LB_USART_CR3:
        s->cr3 = value;
        break;
    case SF32LB_USART_BRR:
        s->brr = value;
        break;
    case SF32LB_USART_RQR:
        if (value & SF32LB_USART_RQR_RXFRQ) {
            s->isr &= ~SF32LB_USART_ISR_RXNE;
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case SF32LB_USART_ICR:
        if (value & SF32LB_USART_ICR_TCCF) {
            s->isr &= ~SF32LB_USART_ISR_TC;
        }
        break;
    case SF32LB_USART_TDR:
        ch = value;
        if ((s->cr1 & SF32LB_USART_CR1_TE) &&
            qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        s->isr |= SF32LB_USART_ISR_TXE | SF32LB_USART_ISR_TC;
        break;
    case SF32LB_USART_MISCR:
        s->miscr = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    sf32lb_usart_update_irq(s);
}

static const MemoryRegionOps sf32lb_usart_ops = {
    .read = sf32lb_usart_read,
    .write = sf32lb_usart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sf32lb_usart_reset_hold(Object *obj, ResetType type)
{
    SF32LBUSARTState *s = SF32LB_USART(obj);

    s->cr1 = 0;
    s->cr2 = 0;
    s->cr3 = 0;
    s->brr = 0;
    s->miscr = 0;
    s->isr = SF32LB_USART_ISR_RESET;
    s->rdr = 0;

    sf32lb_usart_update_irq(s);
}

static void sf32lb_usart_realize(DeviceState *dev, Error **errp)
{
    SF32LBUSARTState *s = SF32LB_USART(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb_usart_ops, s,
                          TYPE_SF32LB_USART, SF32LB_USART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qemu_chr_fe_set_handlers(&s->chr, sf32lb_usart_can_receive,
                             sf32lb_usart_receive, NULL, NULL, s, NULL, true);
}

static const Property sf32lb_usart_properties[] = {
    DEFINE_PROP_CHR("chardev", SF32LBUSARTState, chr),
};

static const VMStateDescription vmstate_sf32lb_usart = {
    .name = TYPE_SF32LB_USART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cr1, SF32LBUSARTState),
        VMSTATE_UINT32(cr2, SF32LBUSARTState),
        VMSTATE_UINT32(cr3, SF32LBUSARTState),
        VMSTATE_UINT32(brr, SF32LBUSARTState),
        VMSTATE_UINT32(miscr, SF32LBUSARTState),
        VMSTATE_UINT32(isr, SF32LBUSARTState),
        VMSTATE_UINT8(rdr, SF32LBUSARTState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb_usart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb_usart_reset_hold;
    device_class_set_props(dc, sf32lb_usart_properties);
    dc->realize = sf32lb_usart_realize;
    dc->vmsd = &vmstate_sf32lb_usart;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo sf32lb_usart_types[] = {
    {
        .name = TYPE_SF32LB_USART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LBUSARTState),
        .class_init = sf32lb_usart_class_init,
    },
};

DEFINE_TYPES(sf32lb_usart_types);

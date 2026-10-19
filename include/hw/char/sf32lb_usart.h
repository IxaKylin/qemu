/*
 * SiFli SF32LB USART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_SF32LB_USART_H
#define HW_CHAR_SF32LB_USART_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB_USART "sifli.sf32lb-usart"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LBUSARTState, SF32LB_USART)

#define SF32LB_USART_SIZE 0x400

/* Register offsets. */
#define SF32LB_USART_CR1    0x00
#define SF32LB_USART_CR2    0x04
#define SF32LB_USART_CR3    0x08
#define SF32LB_USART_BRR    0x0c
#define SF32LB_USART_RQR    0x18
#define SF32LB_USART_ISR    0x1c
#define SF32LB_USART_ICR    0x20
#define SF32LB_USART_RDR    0x24
#define SF32LB_USART_TDR    0x28
#define SF32LB_USART_MISCR  0x2c

/* CR1 */
#define SF32LB_USART_CR1_UE     (1U << 0)
#define SF32LB_USART_CR1_RE     (1U << 2)
#define SF32LB_USART_CR1_TE     (1U << 3)
#define SF32LB_USART_CR1_RXNEIE (1U << 5)
#define SF32LB_USART_CR1_TCIE   (1U << 6)
#define SF32LB_USART_CR1_TXEIE  (1U << 7)

/* ISR */
#define SF32LB_USART_ISR_RXNE   (1U << 5)
#define SF32LB_USART_ISR_TC     (1U << 6)
#define SF32LB_USART_ISR_TXE    (1U << 7)

#define SF32LB_USART_ISR_RESET  0x020000c0

/* ICR */
#define SF32LB_USART_ICR_TCCF   (1U << 6)

/* RQR */
#define SF32LB_USART_RQR_RXFRQ  (1U << 3)

struct SF32LBUSARTState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    CharFrontend chr;

    uint32_t cr1;
    uint32_t cr2;
    uint32_t cr3;
    uint32_t brr;
    uint32_t miscr;
    uint32_t isr;
    uint8_t rdr;
};

#endif /* HW_CHAR_SF32LB_USART_H */

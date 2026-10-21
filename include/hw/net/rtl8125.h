/*
 * Realtek RTL8125B 2.5GbE PCIe endpoint
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_RTL8125_H
#define HW_NET_RTL8125_H

#include "hw/pci/pci_device.h"
#include "net/net.h"

#define TYPE_RTL8125_PCI "rtl8125-pci"
OBJECT_DECLARE_SIMPLE_TYPE(RTL8125State, RTL8125_PCI)

#define RTL8125_BAR_SIZE    0x10000
#define RTL8125_MMIO_BAR    2

/* Driver-visible frame buffer size and descriptor limit. */
#define RTL8125_FRAME_SIZE  1536
#define RTL8125_MAX_DESC    1024

/* Small register files backing the MAC and PHY OCP mailboxes. */
#define RTL8125_OCP_SLOTS   32

typedef struct RTL8125OcpReg {
    uint16_t addr;
    uint16_t val;
} RTL8125OcpReg;

struct RTL8125State {
    /*< private >*/
    PCIDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;
    uint8_t mac[6];

    /* Plain storage for the registers without side effects. */
    uint8_t regs[RTL8125_BAR_SIZE];

    /* MAC/PHY OCP mailbox register files and pending read state. */
    RTL8125OcpReg mac_ocp[RTL8125_OCP_SLOTS];
    RTL8125OcpReg phy_ocp[RTL8125_OCP_SLOTS];
    uint8_t mac_ocp_nr;
    uint8_t phy_ocp_nr;
    bool mac_ocp_read_pending;
    uint16_t mac_ocp_read_addr;
    bool phy_ocp_read_pending;
    bool phy_ocp_read_reported;
    uint16_t phy_ocp_read_addr;
    uint16_t phy_ocp_read_val;

    /* 32-bit RTL8125 interrupt registers. */
    uint32_t intr_status;
    uint32_t intr_mask;
    bool irq_level;

    /* Legacy descriptor rings in guest RAM. */
    uint64_t tx_desc_base;
    uint64_t rx_desc_base;
    uint32_t tx_idx;
    uint32_t rx_idx;
    bool tx_enabled;
    bool rx_enabled;
    bool link_up;

    /* Model-side counters (diagnostics only, not guest visible). */
    uint64_t tx_packets;
    uint64_t tx_errors;
    uint64_t rx_packets;
    uint64_t rx_drops;
};

#endif /* HW_NET_RTL8125_H */

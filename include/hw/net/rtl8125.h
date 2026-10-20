/*
 * Realtek RTL8125B 2.5GbE PCIe endpoint
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the RTL8125 Ethernet endpoint that the RK3588 pcie2x1l2 link is
 * wired to on the ROCK 5B family.  The register contract is taken from the
 * unmodified ZVisor Zephyr driver (eth_rtl8125.c) and the RTL8125B legacy
 * descriptor layout it programs:
 *
 *   - PCI configuration space with vendor/device 10ec:8125, class 0x020000,
 *     a 64 KiB 32-bit MMIO BAR2 and a one-vector MSI capability;
 *   - legacy TX/RX descriptor rings in guest RAM (16-byte descriptors with
 *     opts1/opts2/addr, OWN/FIRST/LAST/EOR bits), DMA through the PCI bus
 *     master address space;
 *   - 32-bit RTL8125 interrupt status/mask registers (W1C status) and MSI
 *     delivery through the PCI MSI capability;
 *   - PHY link state reported through PHY_STATUS plus the PHY/MAC OCP
 *     mailboxes used by the driver's autonegotiation and hardware-config
 *     sequences;
 *   - a QEMU net backend (NICState/NetClientInfo) so the endpoint carries
 *     real traffic (DHCPv4, ARP, ICMP, TCP/UDP).
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

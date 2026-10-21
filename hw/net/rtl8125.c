/*
 * Realtek RTL8125B 2.5GbE PCIe endpoint
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/net/rtl8125.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Register file offsets used by the Zephyr RTL8125 driver. */
#define RTL8125_REG_MAC0            0x0000
#define RTL8125_REG_MAR0            0x0008
#define RTL8125_REG_TX_DESC_LO      0x0020
#define RTL8125_REG_TX_DESC_HI      0x0024
#define RTL8125_REG_CHIP_CMD        0x0037
#define RTL8125_REG_INTR_MASK       0x0038
#define RTL8125_REG_INTR_STATUS     0x003c
#define RTL8125_REG_TX_CONFIG       0x0040
#define RTL8125_REG_RX_CONFIG       0x0044
#define RTL8125_REG_CFG9346         0x0050
#define RTL8125_REG_CONFIG1         0x0052
#define RTL8125_REG_PHY_STATUS      0x006c
#define RTL8125_REG_EPHYAR          0x0080
#define RTL8125_REG_TX_POLL         0x0090
#define RTL8125_REG_OCPDR           0x00b0
#define RTL8125_REG_RX_MAX_SIZE     0x00da
#define RTL8125_REG_CPLUS_CMD       0x00e0
#define RTL8125_REG_RX_DESC_LO      0x00e4
#define RTL8125_REG_RX_DESC_HI      0x00e8
#define RTL8125_REG_MISC            0x00f0
#define RTL8125_REG_BACKUP_MAC0     0x19e0
#define RTL8125_REG_BACKUP_MAC4     0x19e4

#define RTL8125_REG_OCPDR_PHY       (RTL8125_REG_OCPDR + 8)

/* CHIP_CMD bits. */
#define RTL8125_CMD_RESET           BIT(4)
#define RTL8125_CMD_TX_ENABLE       BIT(2)
#define RTL8125_CMD_RX_ENABLE       BIT(3)

/* Interrupt status/mask bits (32-bit RTL8125 legacy register layout). */
#define RTL8125_INT_RX_OK           BIT(0)
#define RTL8125_INT_RX_ERR          BIT(1)
#define RTL8125_INT_TX_OK           BIT(2)
#define RTL8125_INT_TX_ERR          BIT(3)
#define RTL8125_INT_RX_DESC_UNAVAIL BIT(4)
#define RTL8125_INT_LINK_CHANGE     BIT(5)
#define RTL8125_INT_RX_FIFO_OVER    BIT(6)
#define RTL8125_INT_TX_DESC_UNAVAIL BIT(7)

/* Legacy 16-byte descriptor bits. */
#define RTL8125_DESC_OWN            BIT(31)
#define RTL8125_DESC_EOR            BIT(30)
#define RTL8125_DESC_FIRST          BIT(29)
#define RTL8125_DESC_LAST           BIT(28)
#define RTL8125_DESC_LEN_MASK       0x3fffu

/* TX descriptor opts2 checksum-offload request bits. */
#define RTL8125_TX_IPV4_CHKSUM      BIT(29)
#define RTL8125_TX_TCP_CHKSUM       BIT(30)
#define RTL8125_TX_UDP_CHKSUM       BIT(31)

/* PHY_STATUS bits (PHYstatus register read by rtl_update_link). */
#define RTL8125_PHY_LINK            BIT(1)
#define RTL8125_PHY_2500_FULL       BIT(10)
#define RTL8125_PHY_FULL_DUPLEX     BIT(0)

/* PHY OCP registers used by rtl_phy_config_autoneg and the shell command. */
#define RTL8125_OCP_FLAG            BIT(31)
#define RTL8125_OCP_STD_PHY_BASE    0xa400
#define RTL8125_OCP_2500_ADV        0xa5d4
#define RTL8125_OCP_2500_LPA        0xa5d6
#define RTL8125_ADVERTISE_2500FULL  BIT(7)
#define RTL8125_LPA_2500FULL        BIT(5)

/*
 * RTL8125B-family revision field in TX_CONFIG; the driver rejects anything
 * else with -ENOTSUP before its work items are initialized.
 */
#define RTL8125_TX_CONFIG_RTL8125B  0x64000000

/*
 * Factory MAC address, as RTL8125B methods 4/5 keep it in the backup
 * registers.  Value 02:00:00:00:01:00: locally administered, unicast,
 * non-zero, so the driver accepts it without hitting its random-MAC path.
 */
static const uint8_t rtl8125_default_mac[6] = {
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
};

/*
 * qemu_macaddr_default_if_unset() assigns this prefix to a NIC whose MAC the
 * user did not choose.  Treat such an auto-generated address as "not
 * provided" so the endpoint keeps the modeled factory address by default.
 */
static const uint8_t rtl8125_qemu_mac_prefix[5] = {
    0x52, 0x54, 0x00, 0x12, 0x34,
};

static void rtl8125_update_irq(RTL8125State *s);

static bool rtl8125_dma_read(RTL8125State *s, uint64_t addr, void *buf,
                             size_t len)
{
    return pci_dma_read(PCI_DEVICE(s), addr, buf, len) == MEMTX_OK;
}

static bool rtl8125_dma_write(RTL8125State *s, uint64_t addr,
                              const void *buf, size_t len)
{
    return pci_dma_write(PCI_DEVICE(s), addr, buf, len) == MEMTX_OK;
}

static uint16_t rtl8125_ocp_lookup(const RTL8125OcpReg *tbl, unsigned int nr,
                                   uint16_t addr, uint16_t def)
{
    for (unsigned int i = 0; i < nr; i++) {
        if (tbl[i].addr == addr) {
            return tbl[i].val;
        }
    }

    return def;
}

static void rtl8125_ocp_store(RTL8125OcpReg *tbl, uint8_t *nr, uint16_t addr,
                              uint16_t val)
{
    for (unsigned int i = 0; i < *nr; i++) {
        if (tbl[i].addr == addr) {
            tbl[i].val = val;
            return;
        }
    }

    if (*nr < RTL8125_OCP_SLOTS) {
        tbl[*nr].addr = addr;
        tbl[*nr].val = val;
        (*nr)++;
    }
}

static uint16_t rtl8125_phy_status(const RTL8125State *s)
{
    if (!s->link_up) {
        return 0;
    }

    return RTL8125_PHY_LINK | RTL8125_PHY_2500_FULL |
           RTL8125_PHY_FULL_DUPLEX;
}

static void rtl8125_raise_irq(RTL8125State *s, uint32_t bits)
{
    s->intr_status |= bits;
    rtl8125_update_irq(s);
}

/*
 * The endpoint delivers its single MSI vector while any unmasked interrupt
 * status bit is pending.  Status is level-like inside the device (W1C to the
 * guest) but the PCI MSI message is edge-triggered, so only notify on a
 * 0 -> 1 transition of (status & mask).
 */
static void rtl8125_update_irq(RTL8125State *s)
{
    bool level = (s->intr_status & s->intr_mask) != 0;

    if (level == s->irq_level) {
        return;
    }

    s->irq_level = level;

    if (level && s->nic && msi_enabled(PCI_DEVICE(s))) {
        msi_notify(PCI_DEVICE(s), 0);
    }
}

/*
 * The Zephyr driver hands IPv4/TCP/UDP checksum work to the hardware through
 * the TX descriptor opts2 bits and expects the device to fill the checksum
 * fields in the packet buffer.
 */
static void rtl8125_tx_checksums(uint8_t *buf, size_t size, uint32_t opts2)
{
    int flags = 0;

    if (opts2 & RTL8125_TX_IPV4_CHKSUM) {
        flags |= CSUM_IP;
    }
    if (opts2 & RTL8125_TX_TCP_CHKSUM) {
        flags |= CSUM_TCP;
    }
    if (opts2 & RTL8125_TX_UDP_CHKSUM) {
        flags |= CSUM_UDP;
    }

    if (flags == 0) {
        return;
    }

    net_checksum_calculate(buf, size, flags);
}

static void rtl8125_tx_kick(RTL8125State *s)
{
    uint8_t frame[RTL8125_FRAME_SIZE];
    NetClientState *nc;
    bool completed = false;
    bool error = false;
    unsigned int count;

    if (!s->tx_enabled || !s->nic) {
        return;
    }

    nc = qemu_get_queue(s->nic);

    for (count = 0; count < RTL8125_MAX_DESC; count++) {
        uint8_t raw[16];
        uint8_t done[4];
        uint64_t daddr;
        uint64_t paddr;
        uint32_t opts1;
        uint32_t opts2;
        uint32_t len;
        uint32_t newopts;
        bool eor;

        daddr = s->tx_desc_base + (uint64_t)s->tx_idx * 16;
        if (!rtl8125_dma_read(s, daddr, raw, sizeof(raw))) {
            error = true;
            break;
        }

        opts1 = ldl_le_p(raw);
        opts2 = ldl_le_p(raw + 4);
        paddr = ldq_le_p(raw + 8);

        if (!(opts1 & RTL8125_DESC_OWN)) {
            /* Nothing else is queued: the ring is drained. */
            break;
        }

        eor = (opts1 & RTL8125_DESC_EOR) != 0;
        len = opts1 & RTL8125_DESC_LEN_MASK;

        if ((opts1 & (RTL8125_DESC_FIRST | RTL8125_DESC_LAST)) ==
                (RTL8125_DESC_FIRST | RTL8125_DESC_LAST) &&
            len > 0 && len <= sizeof(frame) &&
            rtl8125_dma_read(s, paddr, frame, len)) {
            rtl8125_tx_checksums(frame, len, opts2);
            s->tx_packets++;
            if (nc->peer && !nc->link_down) {
                qemu_send_packet(nc, frame, len);
            }
            completed = true;
        } else {
            if ((opts1 & (RTL8125_DESC_FIRST | RTL8125_DESC_LAST)) !=
                    (RTL8125_DESC_FIRST | RTL8125_DESC_LAST)) {
                qemu_log_mask(LOG_UNIMP,
                              "rtl8125: fragmented TX descriptor unsupported\n");
            }
            s->tx_errors++;
            error = true;
        }

        /* Return the descriptor to the host (clear OWN, keep EOR). */
        newopts = opts1 & ~(RTL8125_DESC_OWN | RTL8125_DESC_LEN_MASK |
                            RTL8125_DESC_FIRST | RTL8125_DESC_LAST);
        stl_le_p(done, newopts);
        if (!rtl8125_dma_write(s, daddr, done, sizeof(done))) {
            error = true;
            break;
        }

        s->tx_idx = eor ? 0 : (s->tx_idx + 1) % RTL8125_MAX_DESC;
    }

    if (completed) {
        s->intr_status |= RTL8125_INT_TX_OK;
    }
    if (error) {
        s->intr_status |= RTL8125_INT_TX_ERR;
    }
    if (completed || error) {
        rtl8125_update_irq(s);
    }
}

static ssize_t rtl8125_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    RTL8125State *s = qemu_get_nic_opaque(nc);
    uint8_t raw[16];
    uint64_t daddr;
    uint64_t paddr;
    uint32_t opts1;
    uint32_t newlen;
    bool eor;

    if (!s->nic) {
        return -1;
    }
    if (!s->link_up || !s->rx_enabled) {
        return -1;
    }

    /*
     * The driver subtracts the 4-byte Ethernet FCS from the descriptor length
     * and copies the remaining frame bytes, so the device reports
     * (frame length + 4) in opts1.  Frames that do not fit the modeled
     * RTL8125_FRAME_SIZE buffer are dropped.
     */
    if (size + 4 > RTL8125_FRAME_SIZE) {
        s->rx_drops++;
        return size;
    }

    daddr = s->rx_desc_base + (uint64_t)s->rx_idx * 16;
    if (!rtl8125_dma_read(s, daddr, raw, sizeof(raw))) {
        s->rx_drops++;
        return size;
    }

    opts1 = ldl_le_p(raw);
    paddr = ldq_le_p(raw + 8);
    eor = (opts1 & RTL8125_DESC_EOR) != 0;

    if (!(opts1 & RTL8125_DESC_OWN)) {
        /* Ring full: report RxDescUnavail and let the driver drain it. */
        s->rx_drops++;
        rtl8125_raise_irq(s, RTL8125_INT_RX_DESC_UNAVAIL);
        return size;
    }

    if (!rtl8125_dma_write(s, paddr, buf, size)) {
        s->rx_drops++;
        return size;
    }

    newlen = size + 4;
    stl_le_p(raw, newlen | (eor ? RTL8125_DESC_EOR : 0));
    stl_le_p(raw + 4, 0);
    if (!rtl8125_dma_write(s, daddr, raw, sizeof(raw))) {
        s->rx_drops++;
        return size;
    }

    s->rx_idx = eor ? 0 : (s->rx_idx + 1) % RTL8125_MAX_DESC;
    s->rx_packets++;
    rtl8125_raise_irq(s, RTL8125_INT_RX_OK);

    return size;
}

static bool rtl8125_can_receive(NetClientState *nc)
{
    RTL8125State *s = qemu_get_nic_opaque(nc);

    return s->nic && s->link_up && s->rx_enabled;
}

static void rtl8125_cleanup(NetClientState *nc)
{
    RTL8125State *s = qemu_get_nic_opaque(nc);

    s->nic = NULL;
}

static void rtl8125_set_link(RTL8125State *s, bool up)
{
    if (up == s->link_up) {
        return;
    }

    s->link_up = up;
    rtl8125_raise_irq(s, RTL8125_INT_LINK_CHANGE);
}

static void rtl8125_link_status_changed(NetClientState *nc)
{
    RTL8125State *s = qemu_get_nic_opaque(nc);

    rtl8125_set_link(s, nc->peer && !nc->link_down);
}

static uint32_t rtl8125_mac_low(const uint8_t mac[6])
{
    return (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
           ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
}

static uint16_t rtl8125_mac_high(const uint8_t mac[6])
{
    return (uint16_t)mac[4] | ((uint16_t)mac[5] << 8);
}

static void rtl8125_set_mac(RTL8125State *s)
{
    const uint8_t *conf_mac = s->conf.macaddr.a;
    bool zero = true;

    for (unsigned int i = 0; i < 6; i++) {
        if (conf_mac[i] != 0) {
            zero = false;
            break;
        }
    }

    if (!zero && memcmp(conf_mac, rtl8125_qemu_mac_prefix,
                        sizeof(rtl8125_qemu_mac_prefix)) != 0) {
        memcpy(s->mac, conf_mac, sizeof(s->mac));
    } else {
        memcpy(s->mac, rtl8125_default_mac, sizeof(s->mac));
    }
}

static void rtl8125_reset(DeviceState *dev)
{
    RTL8125State *s = RTL8125_PCI(dev);
    NetClientState *nc;

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->mac_ocp, 0, sizeof(s->mac_ocp));
    memset(s->phy_ocp, 0, sizeof(s->phy_ocp));
    s->mac_ocp_nr = 0;
    s->phy_ocp_nr = 0;
    s->mac_ocp_read_pending = false;
    s->mac_ocp_read_addr = 0;
    s->phy_ocp_read_pending = false;
    s->phy_ocp_read_reported = false;
    s->phy_ocp_read_addr = 0;
    s->phy_ocp_read_val = 0;
    s->intr_status = 0;
    s->intr_mask = 0;
    s->irq_level = false;
    s->tx_desc_base = 0;
    s->rx_desc_base = 0;
    s->tx_idx = 0;
    s->rx_idx = 0;
    s->tx_enabled = false;
    s->rx_enabled = false;
    s->tx_packets = 0;
    s->tx_errors = 0;
    s->rx_packets = 0;
    s->rx_drops = 0;

    /* TX_CONFIG carries the chip revision in read-only bits. */
    stl_le_p(&s->regs[RTL8125_REG_TX_CONFIG], RTL8125_TX_CONFIG_RTL8125B);

    /* Factory MAC address in the backup registers (methods 4/5). */
    stl_le_p(&s->regs[RTL8125_REG_BACKUP_MAC0], rtl8125_mac_low(s->mac));
    stw_le_p(&s->regs[RTL8125_REG_BACKUP_MAC4], rtl8125_mac_high(s->mac));

    nc = s->nic ? qemu_get_queue(s->nic) : NULL;
    s->link_up = nc && nc->peer && !nc->link_down;
}

static uint32_t rtl8125_mac_ocp_read(RTL8125State *s)
{
    uint16_t val;

    if (!s->mac_ocp_read_pending) {
        return 0;
    }

    s->mac_ocp_read_pending = false;
    val = rtl8125_ocp_lookup(s->mac_ocp, s->mac_ocp_nr,
                             s->mac_ocp_read_addr, 0);
    return val;
}

static void rtl8125_mac_ocp_write(RTL8125State *s, uint32_t value)
{
    uint16_t addr = (value >> 15) & 0xffff;

    if (value & RTL8125_OCP_FLAG) {
        rtl8125_ocp_store(s->mac_ocp, &s->mac_ocp_nr, addr,
                          value & 0xffff);
    } else {
        s->mac_ocp_read_pending = true;
        s->mac_ocp_read_addr = addr;
    }

    stl_le_p(&s->regs[RTL8125_REG_OCPDR], value & 0xffff);
}

static uint16_t rtl8125_phy_ocp_value(const RTL8125State *s, uint16_t addr)
{
    /* The modeled link partner advertises 2.5G full duplex capability. */
    if (addr == RTL8125_OCP_2500_LPA) {
        return RTL8125_LPA_2500FULL;
    }

    return rtl8125_ocp_lookup(s->phy_ocp, s->phy_ocp_nr, addr, 0);
}

static uint32_t rtl8125_phy_ocp_read(RTL8125State *s)
{
    uint32_t val;

    if (!s->phy_ocp_read_pending) {
        return 0;
    }

    val = s->phy_ocp_read_val;

    /*
     * The driver polls the flag until it is set and then reads the value
     * with a second access.  Report the completion flag first, then the
     * register value.
     */
    if (!s->phy_ocp_read_reported) {
        s->phy_ocp_read_reported = true;
        return val | RTL8125_OCP_FLAG;
    }

    s->phy_ocp_read_pending = false;
    s->phy_ocp_read_reported = false;
    return val;
}

static void rtl8125_phy_ocp_write(RTL8125State *s, uint32_t value)
{
    uint16_t addr = (value >> 15) & 0xffff;

    if (value & RTL8125_OCP_FLAG) {
        rtl8125_ocp_store(s->phy_ocp, &s->phy_ocp_nr, addr,
                          value & 0xffff);
        s->phy_ocp_read_pending = false;
        s->phy_ocp_read_reported = false;
    } else {
        s->phy_ocp_read_pending = true;
        s->phy_ocp_read_reported = false;
        s->phy_ocp_read_addr = addr;
        s->phy_ocp_read_val = rtl8125_phy_ocp_value(s, addr);
    }

    stl_le_p(&s->regs[RTL8125_REG_OCPDR_PHY], value & 0xffff);
}

static void rtl8125_chip_cmd_write(RTL8125State *s, uint32_t value)
{
    uint8_t cmd = value & 0xff;
    bool was_rx = s->rx_enabled;

    s->regs[RTL8125_REG_CHIP_CMD] = cmd;

    if (cmd & RTL8125_CMD_RESET) {
        /* The reset command is self-clearing; the driver polls for it. */
        s->regs[RTL8125_REG_CHIP_CMD] &= ~RTL8125_CMD_RESET;
        s->intr_status = 0;
        s->intr_mask = 0;
        s->irq_level = false;
        s->tx_enabled = false;
        s->rx_enabled = false;
        s->tx_idx = 0;
        s->rx_idx = 0;
        return;
    }

    s->tx_enabled = (cmd & RTL8125_CMD_TX_ENABLE) != 0;
    s->rx_enabled = (cmd & RTL8125_CMD_RX_ENABLE) != 0;

    if (s->rx_enabled && !was_rx) {
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        if (s->link_up) {
            rtl8125_raise_irq(s, RTL8125_INT_LINK_CHANGE);
        }
    }
}

static void rtl8125_desc_base_write(RTL8125State *s, bool tx)
{
    if (tx) {
        s->tx_desc_base = ((uint64_t)ldl_le_p(&s->regs[
                               RTL8125_REG_TX_DESC_HI]) << 32) |
                          ldl_le_p(&s->regs[RTL8125_REG_TX_DESC_LO]);
        s->tx_idx = 0;
    } else {
        s->rx_desc_base = ((uint64_t)ldl_le_p(&s->regs[
                               RTL8125_REG_RX_DESC_HI]) << 32) |
                          ldl_le_p(&s->regs[RTL8125_REG_RX_DESC_LO]);
        s->rx_idx = 0;
    }
}

static uint64_t rtl8125_mmio_read(void *opaque, hwaddr addr, unsigned int size)
{
    RTL8125State *s = opaque;
    uint64_t val = 0;

    if (addr + size > RTL8125_BAR_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rtl8125: read outside BAR at 0x%" HWADDR_PRIx
                      " size %u\n", addr, size);
        return 0;
    }

    if (addr == RTL8125_REG_INTR_STATUS && size == 4) {
        return s->intr_status;
    }

    if (addr == RTL8125_REG_INTR_MASK && size == 4) {
        return s->intr_mask;
    }

    if (addr == RTL8125_REG_PHY_STATUS && size == 2) {
        return rtl8125_phy_status(s);
    }

    if (addr == RTL8125_REG_OCPDR && size == 4) {
        return rtl8125_mac_ocp_read(s);
    }

    if (addr == RTL8125_REG_OCPDR_PHY && size == 4) {
        return rtl8125_phy_ocp_read(s);
    }

    if (addr == RTL8125_REG_EPHYAR && size == 4) {
        val = ldl_le_p(&s->regs[addr]);
        stl_le_p(&s->regs[addr], val & ~RTL8125_OCP_FLAG);
        return val & ~RTL8125_OCP_FLAG;
    }

    for (unsigned int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (8 * i);
    }

    return val;
}

static void rtl8125_mmio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    RTL8125State *s = opaque;

    if (addr + size > RTL8125_BAR_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rtl8125: write outside BAR at 0x%" HWADDR_PRIx
                      " size %u\n", addr, size);
        return;
    }

    if (addr == RTL8125_REG_INTR_STATUS && size == 4) {
        s->intr_status &= ~(uint32_t)value;
        rtl8125_update_irq(s);
        return;
    }

    if (addr == RTL8125_REG_INTR_MASK && size == 4) {
        s->intr_mask = value;
        rtl8125_update_irq(s);
        return;
    }

    if (addr == RTL8125_REG_CHIP_CMD) {
        rtl8125_chip_cmd_write(s, value);
        return;
    }

    if (addr == RTL8125_REG_OCPDR && size == 4) {
        rtl8125_mac_ocp_write(s, value);
        return;
    }

    if (addr == RTL8125_REG_OCPDR_PHY && size == 4) {
        rtl8125_phy_ocp_write(s, value);
        return;
    }

    if (addr == RTL8125_REG_EPHYAR && size == 4) {
        stl_le_p(&s->regs[addr], value);
        return;
    }

    if (addr == RTL8125_REG_TX_POLL && (value & BIT(0))) {
        rtl8125_tx_kick(s);
        return;
    }

    for (unsigned int i = 0; i < size; i++) {
        s->regs[addr + i] = (value >> (8 * i)) & 0xff;
    }

    if (addr <= RTL8125_REG_TX_DESC_LO &&
        addr + size > RTL8125_REG_TX_DESC_LO) {
        rtl8125_desc_base_write(s, true);
    }
    if (addr <= RTL8125_REG_TX_DESC_HI &&
        addr + size > RTL8125_REG_TX_DESC_HI) {
        rtl8125_desc_base_write(s, true);
    }
    if (addr <= RTL8125_REG_RX_DESC_LO &&
        addr + size > RTL8125_REG_RX_DESC_LO) {
        rtl8125_desc_base_write(s, false);
    }
    if (addr <= RTL8125_REG_RX_DESC_HI &&
        addr + size > RTL8125_REG_RX_DESC_HI) {
        rtl8125_desc_base_write(s, false);
    }
}

static const MemoryRegionOps rtl8125_mmio_ops = {
    .read = rtl8125_mmio_read,
    .write = rtl8125_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = false,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = false,
    },
};

static NetClientInfo net_rtl8125_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = rtl8125_can_receive,
    .receive = rtl8125_receive,
    .cleanup = rtl8125_cleanup,
    .link_status_changed = rtl8125_link_status_changed,
};

static void rtl8125_realize(PCIDevice *pci_dev, Error **errp)
{
    RTL8125State *s = RTL8125_PCI(pci_dev);
    DeviceState *dev = DEVICE(pci_dev);
    Error *local_err = NULL;

    memory_region_init_io(&s->mmio, OBJECT(s), &rtl8125_mmio_ops, s,
                          "rtl8125-mmio", RTL8125_BAR_SIZE);
    /*
     * A 32-bit memory BAR is deliberate.  Zephyr's generic PCIe enumerator
     * allocates 32-bit BARs from the controller's non-prefetchable MEM range
     * (DT range 0xf4200000+0xe00000).  The separate MEM64 range
     * (0xa00000000+0x40000000) is not forwarded by QEMU's root-port bridge
     * window logic, which the unmodified Zephyr enumerator only programs for
     * the non-prefetchable MEM range.  On real RK3588 hardware the RC iATU
     * forwards the MEM64 BAR directly, but for this model the 32-bit BAR lands
     * in the range that the unmodified guest does configure.
     */
    pci_register_bar(pci_dev, RTL8125_MMIO_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /*
     * One 64-bit MSI vector, matching the driver's single-vector ITS setup.
     * If QEMU cannot provide MSI, keep the endpoint enumerable: the driver
     * logs the failure after it has initialized its work items and boot
     * continues without a working interrupt path.
     */
    if (msi_init(pci_dev, 0x50, 1, true, false, &local_err) < 0) {
        if (local_err) {
            warn_report_err(local_err);
        }
        local_err = NULL;
    }

    rtl8125_set_mac(s);

    s->nic = qemu_new_nic(&net_rtl8125_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->mac);

    rtl8125_reset(dev);
}

static void rtl8125_exit(PCIDevice *pci_dev)
{
    RTL8125State *s = RTL8125_PCI(pci_dev);

    msi_uninit(pci_dev);
    if (s->nic) {
        qemu_del_nic(s->nic);
    }
}

static const VMStateDescription vmstate_rtl8125_ocp = {
    .name = "rtl8125-ocp-reg",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(addr, RTL8125OcpReg),
        VMSTATE_UINT16(val, RTL8125OcpReg),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rtl8125 = {
    .name = "rtl8125",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, RTL8125State),
        VMSTATE_BUFFER(mac, RTL8125State),
        VMSTATE_BUFFER(regs, RTL8125State),
        VMSTATE_STRUCT_ARRAY(mac_ocp, RTL8125State, RTL8125_OCP_SLOTS, 0,
                             vmstate_rtl8125_ocp, RTL8125OcpReg),
        VMSTATE_STRUCT_ARRAY(phy_ocp, RTL8125State, RTL8125_OCP_SLOTS, 0,
                             vmstate_rtl8125_ocp, RTL8125OcpReg),
        VMSTATE_UINT8(mac_ocp_nr, RTL8125State),
        VMSTATE_UINT8(phy_ocp_nr, RTL8125State),
        VMSTATE_BOOL(mac_ocp_read_pending, RTL8125State),
        VMSTATE_UINT16(mac_ocp_read_addr, RTL8125State),
        VMSTATE_BOOL(phy_ocp_read_pending, RTL8125State),
        VMSTATE_BOOL(phy_ocp_read_reported, RTL8125State),
        VMSTATE_UINT16(phy_ocp_read_addr, RTL8125State),
        VMSTATE_UINT16(phy_ocp_read_val, RTL8125State),
        VMSTATE_UINT32(intr_status, RTL8125State),
        VMSTATE_UINT32(intr_mask, RTL8125State),
        VMSTATE_BOOL(irq_level, RTL8125State),
        VMSTATE_UINT64(tx_desc_base, RTL8125State),
        VMSTATE_UINT64(rx_desc_base, RTL8125State),
        VMSTATE_UINT32(tx_idx, RTL8125State),
        VMSTATE_UINT32(rx_idx, RTL8125State),
        VMSTATE_BOOL(tx_enabled, RTL8125State),
        VMSTATE_BOOL(rx_enabled, RTL8125State),
        VMSTATE_BOOL(link_up, RTL8125State),
        VMSTATE_UINT64(tx_packets, RTL8125State),
        VMSTATE_UINT64(tx_errors, RTL8125State),
        VMSTATE_UINT64(rx_packets, RTL8125State),
        VMSTATE_UINT64(rx_drops, RTL8125State),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rtl8125_properties[] = {
    DEFINE_NIC_PROPERTIES(RTL8125State, conf),
};

static void rtl8125_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = rtl8125_realize;
    k->exit = rtl8125_exit;
    k->vendor_id = PCI_VENDOR_ID_REALTEK;
    k->device_id = 0x8125;
    k->revision = 0x05;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    device_class_set_legacy_reset(dc, rtl8125_reset);
    device_class_set_props(dc, rtl8125_properties);
    dc->desc = "Realtek RTL8125 2.5GbE PCIe endpoint";
    dc->vmsd = &vmstate_rtl8125;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo rtl8125_info = {
    .name          = TYPE_RTL8125_PCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(RTL8125State),
    .class_init    = rtl8125_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void rtl8125_register_types(void)
{
    type_register_static(&rtl8125_info);
}

type_init(rtl8125_register_types)

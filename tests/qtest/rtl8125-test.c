/*
 * QEMU RTL8125 PCIe endpoint tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"

#define RTL8125_VENDOR_ID           0x10ec
#define RTL8125_DEVICE_ID           0x8125

/*
 * aarch64 virt with highmem-ecam=off: PCIe ECAM is in the low memory map at
 * 0x3f000000 and RAM starts at 0x40000000.
 */
#define VIRT_ECAM_BASE              0x3f000000ULL
#define VIRT_RAM_BASE               0x40000000ULL
#define RTL8125_TEST_BAR            0x20000000ULL

/* PCI config space basics. */
#define PCI_CONFIG_COMMAND          0x04
#define PCI_CONFIG_BAR2             0x18
#define PCI_COMMAND_MEMORY          0x0002
#define PCI_COMMAND_MASTER          0x0004

/* RTL8125 register offsets (must match hw/net/rtl8125.c). */
#define RTL8125_REG_TX_DESC_LO      0x0020
#define RTL8125_REG_TX_DESC_HI      0x0024
#define RTL8125_REG_CHIP_CMD        0x0037
#define RTL8125_REG_INTR_MASK       0x0038
#define RTL8125_REG_INTR_STATUS     0x003c
#define RTL8125_REG_TX_CONFIG       0x0040
#define RTL8125_REG_PHY_STATUS      0x006c
#define RTL8125_REG_EPHYAR          0x0080
#define RTL8125_REG_TX_POLL         0x0090
#define RTL8125_REG_OCPDR           0x00b0
#define RTL8125_REG_RX_MAX_SIZE     0x00da
#define RTL8125_REG_CPLUS_CMD       0x00e0
#define RTL8125_REG_RX_DESC_LO      0x00e4
#define RTL8125_REG_RX_DESC_HI      0x00e8
#define RTL8125_REG_BACKUP_MAC0     0x19e0
#define RTL8125_REG_BACKUP_MAC4     0x19e4
#define RTL8125_REG_OCPDR_PHY       (RTL8125_REG_OCPDR + 8)

/* Register bits. */
#define RTL8125_CMD_RESET           BIT(4)
#define RTL8125_CMD_TX_ENABLE       BIT(2)
#define RTL8125_CMD_RX_ENABLE       BIT(3)

#define RTL8125_INT_RX_OK           BIT(0)
#define RTL8125_INT_TX_OK           BIT(2)
#define RTL8125_INT_LINK_CHANGE     BIT(5)

#define RTL8125_DESC_OWN            BIT(31)
#define RTL8125_DESC_EOR            BIT(30)
#define RTL8125_DESC_FIRST          BIT(29)
#define RTL8125_DESC_LAST           BIT(28)

#define RTL8125_OCP_FLAG            BIT(31)
#define RTL8125_OCP_2500_ADV        0xa5d4
#define RTL8125_OCP_2500_LPA        0xa5d6
#define RTL8125_ADVERTISE_2500FULL  BIT(7)
#define RTL8125_LPA_2500FULL        BIT(5)

#define RTL8125_TX_CONFIG_RTL8125B  0x64000000
#define RTL8125_PHY_STATUS_LINK     0x0403

#define RTL8125_MSI_DATA            0x5a5a

/* Guest RAM used for the descriptor rings, frames and the MSI target. */
#define TEST_TX_DESC   (VIRT_RAM_BASE + 0x00100000)
#define TEST_TX_BUF    (VIRT_RAM_BASE + 0x00110000)
#define TEST_RX_DESC   (VIRT_RAM_BASE + 0x00120000)
#define TEST_RX_BUF    (VIRT_RAM_BASE + 0x00130000)
#define TEST_MSI_ADDR  (VIRT_RAM_BASE + 0x00140000)

#define TEST_DEVICE_ARGS                                                     \
    "-machine virt,highmem-ecam=off,gic-version=3 -m 256M -display none "   \
    "-device rtl8125-pci,netdev=n0 -netdev user,id=n0"

static uint8_t cfg_readb(QTestState *qts, int devfn, unsigned int off)
{
    return qtest_readb(qts, VIRT_ECAM_BASE + ((uint64_t)devfn << 12) + off);
}

static uint16_t cfg_readw(QTestState *qts, int devfn, unsigned int off)
{
    return qtest_readw(qts, VIRT_ECAM_BASE + ((uint64_t)devfn << 12) + off);
}

static uint32_t cfg_readl(QTestState *qts, int devfn, unsigned int off)
{
    return qtest_readl(qts, VIRT_ECAM_BASE + ((uint64_t)devfn << 12) + off);
}

static void cfg_writew(QTestState *qts, int devfn, unsigned int off,
                       uint16_t val)
{
    qtest_writew(qts, VIRT_ECAM_BASE + ((uint64_t)devfn << 12) + off, val);
}

static void cfg_writel(QTestState *qts, int devfn, unsigned int off,
                       uint32_t val)
{
    qtest_writel(qts, VIRT_ECAM_BASE + ((uint64_t)devfn << 12) + off, val);
}

static uint32_t reg_readl(QTestState *qts, unsigned int off)
{
    return qtest_readl(qts, RTL8125_TEST_BAR + off);
}

static uint8_t reg_readb(QTestState *qts, unsigned int off)
{
    return qtest_readb(qts, RTL8125_TEST_BAR + off);
}

static uint16_t reg_readw(QTestState *qts, unsigned int off)
{
    return qtest_readw(qts, RTL8125_TEST_BAR + off);
}

static void reg_writel(QTestState *qts, unsigned int off, uint32_t val)
{
    qtest_writel(qts, RTL8125_TEST_BAR + off, val);
}

static void reg_writew(QTestState *qts, unsigned int off, uint16_t val)
{
    qtest_writew(qts, RTL8125_TEST_BAR + off, val);
}

static void reg_writeb(QTestState *qts, unsigned int off, uint8_t val)
{
    qtest_writeb(qts, RTL8125_TEST_BAR + off, val);
}

/* Program BAR2 and enable memory decode plus bus mastering. */
static int rtl8125_setup(QTestState *qts)
{
    uint32_t mask;
    int devfn = -1;

    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = cfg_readl(qts, dev << 3, 0x00);

        if ((id & 0xffff) == RTL8125_VENDOR_ID &&
            (id >> 16) == RTL8125_DEVICE_ID) {
            devfn = dev << 3;
            break;
        }
    }
    g_assert_cmpint(devfn, >=, 0);

    /* The BAR must probe as a 64 KiB 32-bit memory window. */
    cfg_writel(qts, devfn, PCI_CONFIG_BAR2, 0xffffffff);
    mask = cfg_readl(qts, devfn, PCI_CONFIG_BAR2);
    g_assert_cmphex(mask, ==, 0xffff0000);

    cfg_writel(qts, devfn, PCI_CONFIG_BAR2, RTL8125_TEST_BAR);
    cfg_writew(qts, devfn, PCI_CONFIG_COMMAND,
               PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    return devfn;
}

/*
 * The driver reads the 2.5G register pair through the PHY OCP mailbox: a
 * request without the completion flag starts a read, the first access returns
 * the flag, and the second access returns the 16-bit register value.
 */
static uint16_t phy_ocp_read(QTestState *qts, uint16_t addr)
{
    uint32_t val;

    reg_writel(qts, RTL8125_REG_OCPDR_PHY, (uint32_t)addr << 15);

    val = reg_readl(qts, RTL8125_REG_OCPDR_PHY);
    g_assert_cmphex(val & RTL8125_OCP_FLAG, ==, RTL8125_OCP_FLAG);

    return reg_readl(qts, RTL8125_REG_OCPDR_PHY) & 0xffff;
}

static void phy_ocp_write(QTestState *qts, uint16_t addr, uint16_t val)
{
    uint32_t reg = RTL8125_OCP_FLAG | ((uint32_t)addr << 15) | val;

    reg_writel(qts, RTL8125_REG_OCPDR_PHY, reg);
    /* The completion flag is clear once the write has been accepted. */
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_OCPDR_PHY) & RTL8125_OCP_FLAG,
                    ==, 0);
}

static void test_registers(void)
{
    QTestState *qts = qtest_init(TEST_DEVICE_ARGS);
    int devfn = rtl8125_setup(qts);
    uint8_t cap;
    uint32_t status;

    /* PCI identity and class. */
    g_assert_cmphex(cfg_readl(qts, devfn, 0x00), ==,
                    (uint32_t)RTL8125_DEVICE_ID << 16 | RTL8125_VENDOR_ID);
    g_assert_cmphex(cfg_readb(qts, devfn, 0x08), ==, 0x05);
    g_assert_cmphex(cfg_readl(qts, devfn, 0x08) >> 8, ==, 0x020000);

    /* One MSI capability at 0x50, 64-bit capable, initially disabled. */
    cap = cfg_readb(qts, devfn, 0x34);
    g_assert_cmphex(cap, ==, 0x50);
    g_assert_cmphex(cfg_readb(qts, devfn, cap), ==, 0x05);
    g_assert_cmphex(cfg_readb(qts, devfn, cap + 1), ==, 0x00);
    g_assert_cmphex(cfg_readw(qts, devfn, cap + 2), ==,
                    0x0080 /* 64-bit capable, one vector, masked off */);

    /* Reset-visible driver contract. */
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_TX_CONFIG), ==,
                    RTL8125_TX_CONFIG_RTL8125B);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_BACKUP_MAC0), ==, 0x00000002);
    g_assert_cmphex(reg_readw(qts, RTL8125_REG_BACKUP_MAC4), ==, 0x0001);

    /* With a net backend attached the PHY reports link up at 2.5G full. */
    g_assert_cmphex(reg_readw(qts, RTL8125_REG_PHY_STATUS), ==,
                    RTL8125_PHY_STATUS_LINK);

    /* CHIP_CMD reset is self-clearing (the driver polls for it). */
    reg_writeb(qts, RTL8125_REG_CHIP_CMD, RTL8125_CMD_RESET);
    g_assert_cmphex(reg_readb(qts, RTL8125_REG_CHIP_CMD) & RTL8125_CMD_RESET,
                    ==, 0);

    /* Interrupt mask is RW and status is write-1-to-clear. */
    reg_writel(qts, RTL8125_REG_INTR_MASK, 0x00000033);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_INTR_MASK), ==, 0x00000033);

    /* Enabling RX/TX latches the link change interrupt. */
    reg_writeb(qts, RTL8125_REG_CHIP_CMD,
               RTL8125_CMD_RX_ENABLE | RTL8125_CMD_TX_ENABLE);
    status = reg_readl(qts, RTL8125_REG_INTR_STATUS);
    g_assert_cmphex(status & RTL8125_INT_LINK_CHANGE, ==,
                    RTL8125_INT_LINK_CHANGE);
    reg_writel(qts, RTL8125_REG_INTR_STATUS, status);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_INTR_STATUS), ==, 0);

    /* PHY OCP mailbox: autonegotiation advertise and link-partner LPA. */
    phy_ocp_write(qts, RTL8125_OCP_2500_ADV, RTL8125_ADVERTISE_2500FULL);
    g_assert_cmphex(phy_ocp_read(qts, RTL8125_OCP_2500_ADV), ==,
                    RTL8125_ADVERTISE_2500FULL);
    g_assert_cmphex(phy_ocp_read(qts, RTL8125_OCP_2500_LPA), ==,
                    RTL8125_LPA_2500FULL);

    /* MAC OCP mailbox: write with the flag, read back by register address. */
    reg_writel(qts, RTL8125_REG_OCPDR,
               RTL8125_OCP_FLAG | (0xd40aU << 15) | 0x1234);
    reg_writel(qts, RTL8125_REG_OCPDR, 0xd40aU << 15);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_OCPDR) & 0xffff, ==, 0x1234);

    /* EPHY access register completes immediately (flag clears on read). */
    reg_writel(qts, RTL8125_REG_EPHYAR, RTL8125_OCP_FLAG | (0x06 << 16) | 0x1f);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_EPHYAR) & RTL8125_OCP_FLAG,
                    ==, 0);

    qtest_quit(qts);
}

static void test_no_backend_link_down(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,highmem-ecam=off,gic-version=3 -m 256M "
        "-display none -device rtl8125-pci");
    int devfn = rtl8125_setup(qts);

    /* Without a net backend the PHY must not report a carrier. */
    g_assert_cmphex(reg_readw(qts, RTL8125_REG_PHY_STATUS), ==, 0);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_BACKUP_MAC0), ==, 0x00000002);

    reg_writeb(qts, RTL8125_REG_CHIP_CMD,
               RTL8125_CMD_RX_ENABLE | RTL8125_CMD_TX_ENABLE);
    g_assert_cmphex(reg_readl(qts, RTL8125_REG_INTR_STATUS) &
                    RTL8125_INT_LINK_CHANGE, ==, 0);

    g_assert_cmpint(devfn, >=, 0);
    qtest_quit(qts);
}

static void desc_write(QTestState *qts, uint64_t addr, uint32_t opts1,
                       uint32_t opts2, uint64_t paddr)
{
    uint8_t raw[16];

    stl_le_p(raw, opts1);
    stl_le_p(raw + 4, opts2);
    stq_le_p(raw + 8, paddr);
    qtest_memwrite(qts, addr, raw, sizeof(raw));
}

static uint32_t desc_opts1(QTestState *qts, uint64_t addr)
{
    uint8_t raw[4];

    qtest_memread(qts, addr, raw, sizeof(raw));
    return ldl_le_p(raw);
}

static bool wait_desc_owned_by_guest(QTestState *qts, uint64_t addr)
{
    for (int i = 0; i < 500; i++) {
        if (!(desc_opts1(qts, addr) & RTL8125_DESC_OWN)) {
            return true;
        }
        g_usleep(10000);
    }

    return false;
}

/* One 42-byte ARP request from the guest 10.0.2.15 for the slirp gateway. */
static const uint8_t arp_request[42] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             /* dst: broadcast */
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,             /* src: endpoint MAC */
    0x08, 0x06,                                     /* ARP */
    0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01, /* IPv4 request */
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,             /* sender MAC */
    10, 0, 2, 15,                                   /* sender IP */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* target MAC */
    10, 0, 2, 2,                                    /* target IP: slirp GW */
};

static void test_rx_tx_msi(void)
{
    QTestState *qts = qtest_init(TEST_DEVICE_ARGS);
    int devfn = rtl8125_setup(qts);
    uint8_t cap = cfg_readb(qts, devfn, 0x34);
    uint8_t frame[128];
    uint32_t opts1;
    uint32_t len;
    uint32_t status;

    g_assert_cmphex(cfg_readb(qts, devfn, cap), ==, 0x05);

    /* Let the driver-style init enable the link and both data paths. */
    reg_writeb(qts, RTL8125_REG_CHIP_CMD,
               RTL8125_CMD_RX_ENABLE | RTL8125_CMD_TX_ENABLE);
    reg_writel(qts, RTL8125_REG_INTR_STATUS, 0xffffffff);
    reg_writel(qts, RTL8125_REG_INTR_MASK, RTL8125_INT_RX_OK);

    /* Program the one-vector MSI to land in plain guest RAM. */
    qtest_writel(qts, TEST_MSI_ADDR, 0);
    cfg_writel(qts, devfn, cap + 4, (uint32_t)TEST_MSI_ADDR);
    cfg_writel(qts, devfn, cap + 8, (uint32_t)(TEST_MSI_ADDR >> 32));
    cfg_writew(qts, devfn, cap + 12, RTL8125_MSI_DATA);
    cfg_writew(qts, devfn, cap + 2, 0x0001);
    g_assert_cmphex(cfg_readw(qts, devfn, cap + 2) & 0x0001, ==, 0x0001);

    /* Single-descriptor rings with the EOR marker on descriptor 0. */
    reg_writel(qts, RTL8125_REG_TX_DESC_HI, 0);
    reg_writel(qts, RTL8125_REG_TX_DESC_LO, (uint32_t)TEST_TX_DESC);
    reg_writel(qts, RTL8125_REG_RX_DESC_HI, 0);
    reg_writel(qts, RTL8125_REG_RX_DESC_LO, (uint32_t)TEST_RX_DESC);
    qtest_writel(qts, RTL8125_REG_RX_MAX_SIZE, 1537);

    desc_write(qts, TEST_RX_DESC,
               RTL8125_DESC_OWN | RTL8125_DESC_EOR | 1536, 0, TEST_RX_BUF);
    qtest_memwrite(qts, TEST_TX_BUF, arp_request, sizeof(arp_request));
    desc_write(qts, TEST_TX_DESC,
               RTL8125_DESC_OWN | RTL8125_DESC_FIRST | RTL8125_DESC_LAST |
               RTL8125_DESC_EOR | sizeof(arp_request), 0, TEST_TX_BUF);

    /* TX_POLL makes the device consume the descriptor and emit the frame. */
    reg_writew(qts, RTL8125_REG_TX_POLL, BIT(0));
    g_assert_true(wait_desc_owned_by_guest(qts, TEST_TX_DESC));
    status = reg_readl(qts, RTL8125_REG_INTR_STATUS);
    g_assert_cmphex(status & RTL8125_INT_TX_OK, ==, RTL8125_INT_TX_OK);

    /*
     * The user-mode net backend answers the ARP request, so the same frame
     * batch also fills the RX descriptor and raises the unmasked RX MSI.
     */
    g_assert_true(wait_desc_owned_by_guest(qts, TEST_RX_DESC));

    opts1 = desc_opts1(qts, TEST_RX_DESC);
    len = opts1 & 0x3fff;
    g_assert_cmphex(opts1 & RTL8125_DESC_OWN, ==, 0);
    g_assert_cmphex(opts1 & RTL8125_DESC_EOR, ==, RTL8125_DESC_EOR);
    g_assert_cmpuint(len, >=, 42 + 4);
    g_assert_cmpuint(len, <=, 1536);

    qtest_memread(qts, TEST_RX_BUF, frame, sizeof(frame));
    g_assert_cmphex(lduw_be_p(frame + 12), ==, 0x0806);        /* ARP */
    g_assert_cmphex(lduw_be_p(frame + 20), ==, 0x0002);        /* reply */
    g_assert_cmphex(ldl_be_p(frame + 28), ==, 0x0a000202);     /* 10.0.2.2 */
    g_assert_cmphex(ldl_be_p(frame + 38), ==, 0x0a00020f);     /* 10.0.2.15 */

    status = reg_readl(qts, RTL8125_REG_INTR_STATUS);
    g_assert_cmphex(status & RTL8125_INT_RX_OK, ==, RTL8125_INT_RX_OK);

    /* The single MSI vector wrote its data word to the programmed address. */
    g_assert_cmphex(qtest_readl(qts, TEST_MSI_ADDR), ==, RTL8125_MSI_DATA);

    /*
     * Recycle the ring the way the driver does and confirm the endpoint wraps
     * on the EOR marker and fills descriptor 0 again.
     */
    desc_write(qts, TEST_RX_DESC,
               RTL8125_DESC_OWN | RTL8125_DESC_EOR | 1536, 0, TEST_RX_BUF);
    desc_write(qts, TEST_TX_DESC,
               RTL8125_DESC_OWN | RTL8125_DESC_FIRST | RTL8125_DESC_LAST |
               RTL8125_DESC_EOR | sizeof(arp_request), 0, TEST_TX_BUF);
    reg_writew(qts, RTL8125_REG_TX_POLL, BIT(0));
    g_assert_true(wait_desc_owned_by_guest(qts, TEST_RX_DESC));
    g_assert_cmphex(desc_opts1(qts, TEST_RX_DESC) & RTL8125_DESC_OWN, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rtl8125/registers", test_registers);
    qtest_add_func("/rtl8125/no-backend-link-down", test_no_backend_link_down);
    qtest_add_func("/rtl8125/rx-tx-msi", test_rx_tx_msi);

    return g_test_run();
}

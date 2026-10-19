/*
 * QTest for the SiFli SF32LB52X board machine model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "libqtest.h"

#define SF32LB52X_MACHINE "sf32lb52-devkit-lcd"

/* Memory map */
#define SF32LB52X_SRAM_BASE        0x20000000ULL
#define SF32LB52X_SRAM_SIZE        0x00080000ULL
#define SF32LB52X_SHARED_SRAM_BASE 0x20400000ULL
#define SF32LB52X_SHARED_SRAM_SIZE 0x00010000ULL
#define SF32LB52X_XIP_BASE         0x12000000ULL
#define SF32LB52X_XIP_SIZE         0x01000000ULL
#define SF32LB52X_XIP_IMAGE_ADDR   0x12010000ULL

/* Peripherals */
#define SF32LB52X_RCC_BASE       0x50000000ULL
#define SF32LB52X_PINMUX_BASE    0x50003000ULL
#define SF32LB52X_HPSYS_CFG_BASE 0x5000b000ULL
#define SF32LB52X_GPIO_BASE      0x500a0000ULL
#define SF32LB52X_USART1_BASE    0x50084000ULL
#define SF32LB52X_AON_BASE       0x500c0000ULL
#define SF32LB52X_PMUC_BASE      0x500ca000ULL

/* RCC registers */
#define RCC_CSR    0x20
#define RCC_CFGR   0x24
#define RCC_USBCR  0x28
#define RCC_DLL1CR 0x2c

#define RCC_DLL1CR_EN    (1U << 0)
#define RCC_DLL1CR_READY (1U << 31)

/* AON registers */
#define AON_ACR 0x10

#define AON_ACR_HXT48_REQ (1U << 1)
#define AON_ACR_HXT48_RDY (1U << 31)

/* HPSYS_CFG registers */
#define HPSYS_CFG_CAU2_CR 0x94

/* PINMUX pad registers */
#define PINMUX_PA18_PAD 0x7c
#define PINMUX_PA19_PAD 0x80

/* PMUC registers */
#define PMUC_HXT_CR1 0x68

/* USART registers */
#define USART_CR1 0x00
#define USART_ISR 0x1c
#define USART_ICR 0x20
#define USART_RDR 0x24
#define USART_TDR 0x28

#define USART_CR1_UE (1U << 0)
#define USART_CR1_RE (1U << 2)
#define USART_CR1_TE (1U << 3)

#define USART_ISR_RXNE (1U << 5)
#define USART_ISR_TC   (1U << 6)
#define USART_ISR_TXE  (1U << 7)

#define USART_ISR_RESET 0x020000c0

#define USART_ICR_TCCF (1U << 6)

#define CONSOLE_TIMEOUT (5 * G_TIME_SPAN_SECOND)

static QTestState *sf32lb52x_qtest_start(void)
{
    return qtest_init("-machine " SF32LB52X_MACHINE " -serial null");
}

static QTestState *sf32lb52x_qtest_start_serial(int *sock_fd)
{
    QTestState *qts = qtest_init_with_serial("-machine " SF32LB52X_MACHINE,
                                             sock_fd);

    qemu_set_blocking(*sock_fd, false, &error_abort);
    return qts;
}

static uint32_t rcc_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_RCC_BASE + offset, value);
}

static uint32_t aon_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_AON_BASE + offset);
}

static void aon_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_AON_BASE + offset, value);
}

static uint32_t hpsys_cfg_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_HPSYS_CFG_BASE + offset);
}

static void hpsys_cfg_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_HPSYS_CFG_BASE + offset, value);
}

static uint32_t pinmux_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_PINMUX_BASE + offset);
}

static void pinmux_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_PINMUX_BASE + offset, value);
}

static uint32_t usart1_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_USART1_BASE + offset);
}

static void usart1_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_USART1_BASE + offset, value);
}

static uint8_t console_recv_byte(int sock_fd)
{
    gint64 deadline = g_get_monotonic_time() + CONSOLE_TIMEOUT;
    uint8_t byte;

    do {
        ssize_t ret = recv(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            return byte;
        }
        g_assert_cmpint(ret, ==, -1);
        g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR);
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_assert_not_reached();
}

static void console_send_byte(int sock_fd, uint8_t byte)
{
    gint64 deadline = g_get_monotonic_time() + CONSOLE_TIMEOUT;

    do {
        ssize_t ret = send(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            return;
        }
        g_assert_cmpint(ret, ==, -1);
        g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR);
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_assert_not_reached();
}

static uint32_t usart1_wait_isr(QTestState *qts, uint32_t mask)
{
    gint64 deadline = g_get_monotonic_time() + CONSOLE_TIMEOUT;
    uint32_t isr;

    do {
        isr = usart1_readl(qts, USART_ISR);
        if ((isr & mask) == mask) {
            return isr;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    return isr;
}

static void test_machine_constructs(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    qtest_quit(qts);
}

static void test_sram_read_write(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    qtest_writel(qts, SF32LB52X_SRAM_BASE, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SRAM_BASE), ==, 0xdeadbeef);

    qtest_writel(qts, SF32LB52X_SRAM_BASE + 0x1000, 0x5555aaaa);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SRAM_BASE + 0x1000), ==,
                    0x5555aaaa);

    qtest_writeb(qts, SF32LB52X_SRAM_BASE + 0x800, 0xa5);
    qtest_writew(qts, SF32LB52X_SRAM_BASE + 0x802, 0x5aa5);
    g_assert_cmphex(qtest_readb(qts, SF32LB52X_SRAM_BASE + 0x800), ==, 0xa5);
    g_assert_cmphex(qtest_readw(qts, SF32LB52X_SRAM_BASE + 0x802), ==, 0x5aa5);

    qtest_writel(qts, SF32LB52X_SRAM_BASE + SF32LB52X_SRAM_SIZE - 4,
                 0x12345678);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SRAM_BASE +
                                SF32LB52X_SRAM_SIZE - 4), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_shared_sram_read_write(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    qtest_writel(qts, SF32LB52X_SHARED_SRAM_BASE, 0xcafef00d);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SHARED_SRAM_BASE), ==,
                    0xcafef00d);

    qtest_writel(qts, SF32LB52X_SHARED_SRAM_BASE + 0x100, 0x0badf00d);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SHARED_SRAM_BASE + 0x100), ==,
                    0x0badf00d);

    qtest_writel(qts, SF32LB52X_SHARED_SRAM_BASE +
                 SF32LB52X_SHARED_SRAM_SIZE - 4, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SHARED_SRAM_BASE +
                                SF32LB52X_SHARED_SRAM_SIZE - 4), ==,
                    0x5a5a5a5a);

    qtest_quit(qts);
}

static void test_xip_flash_read_only(void)
{
    QTestState *qts = sf32lb52x_qtest_start();
    static const uint64_t windows[] = {
        SF32LB52X_XIP_IMAGE_ADDR,
        SF32LB52X_XIP_BASE,
        SF32LB52X_XIP_BASE + SF32LB52X_XIP_SIZE - 8,
    };
    uint8_t first[8], second[8];
    size_t i, j;

    for (i = 0; i < ARRAY_SIZE(windows); i++) {
        qtest_memread(qts, windows[i], first, sizeof(first));
        qtest_memread(qts, windows[i], second, sizeof(second));
        g_assert_cmpmem(first, sizeof(first), second, sizeof(second));

        /* No -kernel was given: the whole XIP window is uninitialized ROM. */
        for (j = 0; j < sizeof(first); j++) {
            g_assert_cmphex(first[j], ==, 0);
        }
    }

    qtest_quit(qts);
}

static void test_rcc_store(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    rcc_writel(qts, RCC_CSR, 0x12345678);
    rcc_writel(qts, RCC_CFGR, 0x0badf00d);
    rcc_writel(qts, RCC_USBCR, 0x00000007);

    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==, 0x12345678);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR), ==, 0x0badf00d);
    g_assert_cmphex(rcc_readl(qts, RCC_USBCR), ==, 0x00000007);

    qtest_quit(qts);
}

static void test_rcc_dll1_ready(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    g_assert_cmphex(rcc_readl(qts, RCC_DLL1CR) & RCC_DLL1CR_READY, ==, 0);

    rcc_writel(qts, RCC_DLL1CR, RCC_DLL1CR_EN);
    g_assert_cmphex(rcc_readl(qts, RCC_DLL1CR) & RCC_DLL1CR_READY, ==,
                    RCC_DLL1CR_READY);

    qtest_quit(qts);
}

static void test_aon_hxt48_ready(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    g_assert_cmphex(aon_readl(qts, AON_ACR) & AON_ACR_HXT48_RDY, ==, 0);

    aon_writel(qts, AON_ACR, AON_ACR_HXT48_REQ);
    g_assert_cmphex(aon_readl(qts, AON_ACR) & AON_ACR_HXT48_RDY, ==,
                    AON_ACR_HXT48_RDY);

    qtest_quit(qts);
}

static void test_hpsys_cfg_cau2_cr(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    hpsys_cfg_writel(qts, HPSYS_CFG_CAU2_CR, 0x12345678);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0x12345678);

    hpsys_cfg_writel(qts, HPSYS_CFG_CAU2_CR, 0x00000000);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0x00000000);

    qtest_quit(qts);
}

static void test_pinmux_pads(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    pinmux_writel(qts, PINMUX_PA18_PAD, 0x00001234);
    pinmux_writel(qts, PINMUX_PA19_PAD, 0x00005678);

    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0x00001234);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA19_PAD), ==, 0x00005678);

    pinmux_writel(qts, PINMUX_PA18_PAD, 0x0000abcd);

    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0x0000abcd);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA19_PAD), ==, 0x00005678);

    qtest_quit(qts);
}

static void test_usart1_console(void)
{
    static const uint8_t tx_byte = 0x41;
    QTestState *qts;
    uint8_t rdr[4];
    int sock_fd;

    qts = sf32lb52x_qtest_start_serial(&sock_fd);

    g_assert_cmphex(usart1_readl(qts, USART_ISR), ==, USART_ISR_RESET);

    /* Enable the transmitter, clear TC, and check a TDR write sets TC. */
    usart1_writel(qts, USART_CR1, USART_CR1_UE | USART_CR1_TE);
    usart1_writel(qts, USART_ICR, USART_ICR_TCCF);
    g_assert_cmphex(usart1_readl(qts, USART_ISR) & USART_ISR_TC, ==, 0);

    qtest_memwrite(qts, SF32LB52X_USART1_BASE + USART_TDR, &tx_byte,
                   sizeof(tx_byte));
    g_assert_cmphex(usart1_readl(qts, USART_ISR) & USART_ISR_TC, ==,
                    USART_ISR_TC);
    g_assert_cmphex(console_recv_byte(sock_fd), ==, 0x41);

    /* Receiving requires CR1.RE in addition to CR1.UE. */
    usart1_writel(qts, USART_CR1,
                  USART_CR1_UE | USART_CR1_RE | USART_CR1_TE);
    console_send_byte(sock_fd, 0x42);
    g_assert_cmphex(usart1_wait_isr(qts, USART_ISR_RXNE) & USART_ISR_RXNE,
                    ==, USART_ISR_RXNE);

    qtest_memread(qts, SF32LB52X_USART1_BASE + USART_RDR, rdr, sizeof(rdr));
    g_assert_cmphex(rdr[0], ==, 0x42);

    close(sock_fd);
    qtest_quit(qts);
}

static void test_system_reset(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    rcc_writel(qts, RCC_CSR, 0x12345678);
    aon_writel(qts, AON_ACR, AON_ACR_HXT48_REQ);
    hpsys_cfg_writel(qts, HPSYS_CFG_CAU2_CR, 0x12345678);
    pinmux_writel(qts, PINMUX_PA18_PAD, 0x00001234);
    qtest_writel(qts, SF32LB52X_PMUC_BASE + PMUC_HXT_CR1, 0x12345678);
    qtest_writel(qts, SF32LB52X_GPIO_BASE, 0x12345678);

    /* Make sure every bank is dirty before the reset. */
    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==, 0x12345678);
    g_assert_cmphex(aon_readl(qts, AON_ACR) & AON_ACR_HXT48_REQ, ==,
                    AON_ACR_HXT48_REQ);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0x12345678);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0x00001234);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_PMUC_BASE + PMUC_HXT_CR1), ==,
                    0x12345678);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_GPIO_BASE), ==, 0x12345678);

    qtest_system_reset(qts);

    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==, 0);
    g_assert_cmphex(aon_readl(qts, AON_ACR), ==, 0);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_PMUC_BASE + PMUC_HXT_CR1), ==,
                    0);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_GPIO_BASE), ==, 0);
    g_assert_cmphex(usart1_readl(qts, USART_ISR), ==, USART_ISR_RESET);

    qtest_quit(qts);
}

static void test_kernel_load(void)
{
    static const uint8_t image[16] = {
        0x00, 0x10, 0x00, 0x20,  /* initial SP: 0x20001000 */
        0x01, 0x01, 0x01, 0x12,  /* reset PC: 0x12010101 */
    };
    uint8_t loaded[sizeof(image)];
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sf32lb52x-kernel-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);

    qts = qtest_initf("-machine " SF32LB52X_MACHINE
                      " -kernel %s -serial null", path);
    qtest_memread(qts, SF32LB52X_XIP_IMAGE_ADDR, loaded, sizeof(loaded));
    g_assert_cmpmem(loaded, sizeof(loaded), image, sizeof(image));
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(path), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/machine/constructs",
                   test_machine_constructs);
    qtest_add_func("/sf32lb52x/sram/read-write", test_sram_read_write);
    qtest_add_func("/sf32lb52x/shared-sram/read-write",
                   test_shared_sram_read_write);
    qtest_add_func("/sf32lb52x/xip/read-only", test_xip_flash_read_only);
    qtest_add_func("/sf32lb52x/rcc/store", test_rcc_store);
    qtest_add_func("/sf32lb52x/rcc/dll1-ready", test_rcc_dll1_ready);
    qtest_add_func("/sf32lb52x/aon/hxt48-ready", test_aon_hxt48_ready);
    qtest_add_func("/sf32lb52x/hpsys-cfg/cau2-cr", test_hpsys_cfg_cau2_cr);
    qtest_add_func("/sf32lb52x/pinmux/pads", test_pinmux_pads);
    qtest_add_func("/sf32lb52x/usart1/console", test_usart1_console);
    qtest_add_func("/sf32lb52x/syscon/reset", test_system_reset);
    qtest_add_func("/sf32lb52x/kernel/load", test_kernel_load);

    return g_test_run();
}

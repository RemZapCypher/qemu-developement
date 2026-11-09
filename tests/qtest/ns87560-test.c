/*
 * QTest for the National Semiconductor PC87560 (NS87560) SuperIO chip
 * on the HP C3700 machine
 *
 * Copyright (c) 2026 Soumyajyotii Ssarkar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/* Elroy 0 register window as seen by the CPU (hw/hppa/hppa_hardware.h) */
#define ELROY0_HPA          0xfffffffffed30000ULL
#define ELROY_CFG_ADDR      0x40
#define ELROY_CFG_DATA      0x48

#define NS87560_SLOT        0x0e
#define CFG(fn, off)        ((NS87560_SLOT << 11) | ((fn) << 8) | (off))

/* Elroy registers are little-endian; the CPU is big-endian. */
static uint32_t cfg_readl(QTestState *qts, int fn, int off)
{
    qtest_writel(qts, ELROY0_HPA + ELROY_CFG_ADDR, bswap32(CFG(fn, off)));
    return bswap32(qtest_readl(qts, ELROY0_HPA + ELROY_CFG_DATA));
}

static void cfg_writel(QTestState *qts, int fn, int off, uint32_t val)
{
    qtest_writel(qts, ELROY0_HPA + ELROY_CFG_ADDR, bswap32(CFG(fn, off)));
    qtest_writel(qts, ELROY0_HPA + ELROY_CFG_DATA, bswap32(val));
}

static uint8_t cfg_readb(QTestState *qts, int fn, int off)
{
    return cfg_readl(qts, fn, off & ~3) >> (8 * (off & 3));
}

static void test_ids(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");
    int fn;

    for (fn = 0; fn < 3; fn++) {
        g_assert_cmphex(cfg_readl(qts, fn, 0x00) & 0xffff, ==, 0x100b);
        g_assert_cmphex((cfg_readl(qts, fn, 0x0c) >> 16) & 0xff, ==, 0x80);
    }
    /* fn0 IDE: device, class 0101, prog-if 8A, no interrupt pin */
    g_assert_cmphex(cfg_readl(qts, 0, 0x00) >> 16, ==, 0x0002);
    g_assert_cmphex(cfg_readl(qts, 0, 0x08) >> 8, ==, 0x01018a);
    g_assert_cmphex((cfg_readl(qts, 0, 0x3c) >> 8) & 0xff, ==, 0);
    /* fn1 legacy I/O bridge: device 000E, class 0680, no interrupt pin */
    g_assert_cmphex(cfg_readl(qts, 1, 0x00) >> 16, ==, 0x000e);
    g_assert_cmphex(cfg_readl(qts, 1, 0x08), ==, 0x06800001);
    g_assert_cmphex((cfg_readl(qts, 1, 0x3c) >> 8) & 0xff, ==, 0);
    /* fn2 OHCI: device 0012, class 0C0310, revision 02, pin D */
    g_assert_cmphex(cfg_readl(qts, 2, 0x00) >> 16, ==, 0x0012);
    g_assert_cmphex(cfg_readl(qts, 2, 0x08), ==, 0x0c031002);
    g_assert_cmphex((cfg_readl(qts, 2, 0x3c) >> 8) & 0xff, ==, 4);
    /* functions 3-7 are absent */
    for (fn = 3; fn < 8; fn++) {
        g_assert_cmphex(cfg_readl(qts, fn, 0x00) & 0xffff, ==, 0xffff);
    }
    qtest_quit(qts);
}

/* Function 1 reset values (PC87560UBD datasheet chapter 20) */
static void test_lio_reset(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");
    static const uint8_t ircr[] = { 0x34, 0x65, 0xfe, 0x00, 0x00, 0xc1,
                                    0x00, 0x00, 0x00 };
    int i;

    g_assert_cmphex(cfg_readl(qts, 1, 0x04) & 0xffff, ==, 0x000f);
    g_assert_cmphex(cfg_readl(qts, 1, 0x58), ==, 0xffbf0024);
    g_assert_cmphex(cfg_readb(qts, 1, 0x5e), ==, 0x10);
    g_assert_cmphex(cfg_readb(qts, 1, 0x63), ==, 0x67);
    g_assert_cmphex(cfg_readb(qts, 1, 0x67), ==, 0x00);
    for (i = 0; i < ARRAY_SIZE(ircr); i++) {
        g_assert_cmphex(cfg_readb(qts, 1, 0x69 + i), ==, ircr[i]);
    }
    g_assert_cmphex((cfg_readl(qts, 1, 0x70) >> 16), ==, 0x4c88);
    g_assert_cmphex(cfg_readb(qts, 1, 0x7e), ==, 0x01);
    g_assert_cmphex(cfg_readl(qts, 1, 0x84), ==, 0x00000060);
    g_assert_cmphex(cfg_readl(qts, 1, 0x88), ==, 0x00000801);
    g_assert_cmphex(cfg_readl(qts, 1, 0x8c), ==, 0xffffff01);
    g_assert_cmphex(cfg_readl(qts, 1, 0x90), ==, 0x000003f1);
    g_assert_cmphex(cfg_readl(qts, 1, 0x94), ==, 0x000003f9);
    g_assert_cmphex(cfg_readl(qts, 1, 0x98), ==, 0x000002f9);
    g_assert_cmphex(cfg_readl(qts, 1, 0x9c), ==, 0x00000379);
    g_assert_cmphex(cfg_readl(qts, 1, 0xa0), ==, 0);
    qtest_quit(qts);
}

/* The dword writes of Linux drivers/parisc/superio.c read back intact */
static void test_lio_linux_writes(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");

    cfg_writel(qts, 1, 0x64, 0x82000000);
    cfg_writel(qts, 1, 0x68, 0x07654300);
    cfg_writel(qts, 1, 0x6c, 0x00001000);
    cfg_writel(qts, 1, 0x70, 0x4c880000);
    g_assert_cmphex(cfg_readl(qts, 1, 0x64), ==, 0x82000000);
    g_assert_cmphex(cfg_readl(qts, 1, 0x68), ==, 0x07654300);
    g_assert_cmphex(cfg_readl(qts, 1, 0x6c), ==, 0x00001000);
    g_assert_cmphex(cfg_readl(qts, 1, 0x70), ==, 0x4c880000);
    qtest_quit(qts);
}

/* Write masks: reserved bits stay 0, read-only registers keep their value */
static void test_lio_masks(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");

    cfg_writel(qts, 1, 0x64, 0xffffffff);     /* ELCR1: bits 0, 2 reserved */
    g_assert_cmphex(cfg_readb(qts, 1, 0x67), ==, 0xfa);
    cfg_writel(qts, 1, 0x68, 0x000000ff);     /* ELCR2: bits 0, 5 reserved */
    g_assert_cmphex(cfg_readb(qts, 1, 0x68), ==, 0xde);
    cfg_writel(qts, 1, 0x70, 0x0000ff00);     /* IRCR9 high nibble reserved */
    g_assert_cmphex(cfg_readb(qts, 1, 0x71), ==, 0x0f);
    cfg_writel(qts, 1, 0x5c, 0x00ff0000);     /* parallel port ID is RO */
    g_assert_cmphex(cfg_readb(qts, 1, 0x5e), ==, 0x10);
    cfg_writel(qts, 1, 0x94, 0xffffffff);     /* bits 2:1 RO 0, bit 0 RO 1 */
    g_assert_cmphex(cfg_readl(qts, 1, 0x94), ==, 0xfffffff9);
    cfg_writel(qts, 1, 0xa0, 0xffffffff);     /* reserved */
    g_assert_cmphex(cfg_readl(qts, 1, 0xa0), ==, 0);
    cfg_writel(qts, 1, 0x70, 0x12490000);     /* equal arbiter fields */
    g_assert_cmphex(cfg_readl(qts, 1, 0x70) >> 16, ==, 0x4688);
    qtest_quit(qts);
}

static void test_superio_off(void)
{
    QTestState *qts = qtest_init("-machine C3700 -nodefaults");

    g_assert_cmphex(cfg_readl(qts, 0, 0x00) & 0xffff, ==, 0xffff);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("C3700")) {
        g_test_skip("C3700 machine not available");
        return g_test_run();
    }
    qtest_add_func("/ns87560/ids", test_ids);
    qtest_add_func("/ns87560/superio-off", test_superio_off);
    qtest_add_func("/ns87560/lio-reset", test_lio_reset);
    qtest_add_func("/ns87560/lio-linux-writes", test_lio_linux_writes);
    qtest_add_func("/ns87560/lio-masks", test_lio_masks);
    return g_test_run();
}

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
    return g_test_run();
}

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

/* Elroy 0 PCI I/O space: port N at ELROY0_IO + N */
#define ELROY0_IO           0xfffffffffee00000ULL
#define LIO_PATH            "/machine/ns87560/lio"
#define SRC_SP1             0
#define SRC_SP2             1

/* Elroy IOSAPIC registers (hw/pci-host/astro.c) */
#define IOSAPIC_REG_SELECT  0x800
#define IOSAPIC_REG_WINDOW  0x810
#define IOSAPIC_REG_EOI     0x840
#define NS87560_IOSAPIC_IN  (NS87560_SLOT & 7)   /* Elroy maps slot & 7 */

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

static void outb(QTestState *qts, uint16_t port, uint8_t val)
{
    qtest_writeb(qts, ELROY0_IO + port, val);
}

static uint8_t inb(QTestState *qts, uint16_t port)
{
    return qtest_readb(qts, ELROY0_IO + port);
}

/* Router -> 8259 -> CPUINT, acknowledge by POLL + specific EOI (Linux) */
static void test_irq_route(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");

    qtest_irq_intercept_out_named(qts, LIO_PATH, "intr");
    /* 8259 setup as in drivers/parisc/superio.c; IRQ0 masked */
    outb(qts, 0x20, 0x11); outb(qts, 0x21, 0x00);
    outb(qts, 0x21, 0x04); outb(qts, 0x21, 0x01);
    outb(qts, 0x21, 0x01);
    outb(qts, 0xa0, 0x11); outb(qts, 0xa1, 0x08);
    outb(qts, 0xa1, 0x02); outb(qts, 0xa1, 0x01);
    outb(qts, 0xa1, 0xff);
    /* IRQ3 level-triggered, SP1 -> IRQ3 */
    cfg_writel(qts, 1, 0x64, 0x08000000);
    cfg_writel(qts, 1, 0x68, 0x07654300);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    outb(qts, 0x20, 0x0c);                       /* OCW3: poll */
    g_assert_cmphex(inb(qts, 0x20), ==, 0x83);
    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 0);
    outb(qts, 0x20, 0x63);                       /* specific EOI IRQ3 */
    g_assert_false(qtest_get_irq(qts, 0));

    /* Rerouting an asserted source away lowers the 8259 input */
    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    cfg_writel(qts, 1, 0x68, 0x07654000);        /* SP1 not routed */
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/* ICW1.LTIM is ignored: trigger mode comes only from the ELCR */
static void test_irq_ltim_ignored(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");

    qtest_irq_intercept_out_named(qts, LIO_PATH, "intr");
    outb(qts, 0x20, 0x19); outb(qts, 0x21, 0x00);  /* ICW1 with LTIM */
    outb(qts, 0x21, 0x04); outb(qts, 0x21, 0x01);
    outb(qts, 0x21, 0x01);
    cfg_writel(qts, 1, 0x68, 0x07654300);         /* SP1 -> IRQ3, edge */
    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 1);
    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 0);
    /* Edge-triggered: the request stays latched after the input drops */
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void elroy_writel(QTestState *qts, int reg, uint32_t val)
{
    qtest_writel(qts, ELROY0_HPA + reg, bswap32(val));
}

/* Point Elroy input 6 at the CPU with vector @vec, enabled. */
static void iosapic_set_vector(QTestState *qts, int vec)
{
    elroy_writel(qts, IOSAPIC_REG_SELECT, 0x10 + 2 * NS87560_IOSAPIC_IN);
    elroy_writel(qts, IOSAPIC_REG_WINDOW, vec);
}

/* The CPU does not run under qtest, so EIRR only accumulates bits. */
static uint64_t cpu_eirr(QTestState *qts)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers");
    char *p = strstr(regs, "EIRR ");

    g_assert_nonnull(p);
    return g_ascii_strtoull(p + 5, NULL, 16);
}

#define EIRR_BIT(vec)   (1ull << (31 - (vec)))

/*
 * Two interrupts pending at once: the second must reach the CPU after
 * Linux services the first (poll, specific EOI, IOSAPIC EOI).
 */
static void test_irq_two_sources(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=on -nodefaults");

    /* 8259 setup exactly as drivers/parisc/superio.c */
    outb(qts, 0x20, 0x11); outb(qts, 0x21, 0x00);
    outb(qts, 0x21, 0x04); outb(qts, 0x21, 0x01);
    outb(qts, 0x21, 0xff); outb(qts, 0x20, 0xc2);
    outb(qts, 0xa0, 0x11); outb(qts, 0xa1, 0x00);
    outb(qts, 0xa1, 0x02); outb(qts, 0xa1, 0x01);
    outb(qts, 0x21, 0xff); outb(qts, 0x20, 0x68);
    outb(qts, 0x21, 0xe7);                      /* unmask IRQ3, IRQ4 */
    cfg_writel(qts, 1, 0x68, 0x07654300);       /* SP1 -> 3, SP2 -> 4 */
    iosapic_set_vector(qts, 5);
    g_assert_cmphex(cpu_eirr(qts) & EIRR_BIT(5), ==, 0);

    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 1);
    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP2, 1);
    g_assert_cmphex(cpu_eirr(qts) & EIRR_BIT(5), !=, 0);

    /* A different vector makes the second delivery visible. */
    iosapic_set_vector(qts, 6);
    outb(qts, 0x20, 0x0c);
    g_assert_cmphex(inb(qts, 0x20), ==, 0x83);
    qtest_set_irq_in(qts, LIO_PATH, "router-in", SRC_SP1, 0);
    g_assert_cmphex(cpu_eirr(qts) & EIRR_BIT(6), ==, 0);
    outb(qts, 0x20, 0x63);                      /* specific EOI IRQ3 */
    elroy_writel(qts, IOSAPIC_REG_EOI, 6);
    g_assert_cmphex(cpu_eirr(qts) & EIRR_BIT(6), !=, 0);

    outb(qts, 0x20, 0x0c);
    g_assert_cmphex(inb(qts, 0x20), ==, 0x84);
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
    qtest_add_func("/ns87560/irq-route", test_irq_route);
    qtest_add_func("/ns87560/irq-ltim-ignored", test_irq_ltim_ignored);
    qtest_add_func("/ns87560/irq-two-sources", test_irq_two_sources);
    return g_test_run();
}

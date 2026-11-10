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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");
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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");
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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

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
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

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

/* Linux superio_fixup_pci() switches both IDE channels to native mode */
static void test_ide_progif(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    cfg_writel(qts, 0, 0x08, 0xffffffff);
    g_assert_cmphex(cfg_readl(qts, 0, 0x08), ==, 0x01018f03);
    cfg_writel(qts, 0, 0x40, 0xffffffff);
    g_assert_cmphex(cfg_readb(qts, 0, 0x43) & 3, ==, 0);
    g_assert_cmphex((cfg_readl(qts, 0, 0x3c) >> 8) & 0xff, ==, 0);
    qtest_quit(qts);
}

/* SP1 base register relocates the UART; 5Ah bit 1 disables it */
#define IDE_PATH        "/machine/ns87560/ide"
#define IDE_ARGS        "-machine C3700,superio=ns87560 -nodefaults " \
                        "-drive if=ide,index=0,file=null-co://,format=raw"
#define IDE_CMD_BASE    0x1000
#define IDE_CTL_BASE    0x1010
#define IDE_BM_BASE     0x1020

/* Channel 1 to native mode at IDE_CMD_BASE / IDE_CTL_BASE / IDE_BM_BASE */
static void ide_go_native(QTestState *qts)
{
    cfg_writel(qts, 0, 0x10, IDE_CMD_BASE);
    cfg_writel(qts, 0, 0x14, IDE_CTL_BASE);
    cfg_writel(qts, 0, 0x20, IDE_BM_BASE);
    cfg_writel(qts, 0, 0x08, 0x01018f00);
}

/* BAR and NS register reset values, write masks (PC87560UBD ch. 5) */
static void test_ide_regs(void)
{
    QTestState *qts = qtest_init(IDE_ARGS);
    static const uint32_t bars[] = {
        0xfffffff9, 0xfffffffd, 0xfffffff9, 0xfffffffd, 0xfffffff1
    };
    int i;

    for (i = 0; i < 5; i++) {
        g_assert_cmphex(cfg_readl(qts, 0, 0x10 + 4 * i), ==, bars[i]);
    }
    g_assert_cmphex(cfg_readl(qts, 0, 0x04) & 0xffff, ==, 0x0005);
    g_assert_cmphex(cfg_readl(qts, 0, 0x40), ==, 0x00000000);
    g_assert_cmphex(cfg_readl(qts, 0, 0x44), ==, 0x00008585);
    g_assert_cmphex(cfg_readl(qts, 0, 0x54), ==, 0x0000eeb7);
    /* 44h/45h are timing registers; 46h/47h are reserved */
    cfg_writel(qts, 0, 0x44, 0xffffffff);
    g_assert_cmphex(cfg_readl(qts, 0, 0x44), ==, 0x0000ffff);
    /* 40h: bits 1,4-7; 41h: bits 0-5; 42h: all but bit 2; 43h read-only */
    cfg_writel(qts, 0, 0x40, 0xffffffff);
    g_assert_cmphex(cfg_readl(qts, 0, 0x40), ==, 0x00fb3ff2);
    qtest_quit(qts);
}

/* Legacy ports, 5Ah channel enable, native BARs (PC87560UBD ch. 5, 20) */
static void test_ide_decode(void)
{
    QTestState *qts = qtest_init(IDE_ARGS);
    uint32_t fer = cfg_readl(qts, 1, 0x58);

    g_assert_cmphex(inb(qts, 0x1f7), ==, 0x50);
    cfg_writel(qts, 1, 0x58, fer & ~(1u << 18)  /* 5Ah bit 2 */);
    g_assert_cmphex(inb(qts, 0x1f7), ==, 0xff);
    cfg_writel(qts, 1, 0x58, fer);
    g_assert_cmphex(inb(qts, 0x1f7), ==, 0x50);

    ide_go_native(qts);
    g_assert_cmphex(cfg_readb(qts, 0, 0x09), ==, 0x8f);
    g_assert_cmphex(inb(qts, 0x1f7), ==, 0xff);
    g_assert_cmphex(inb(qts, IDE_CMD_BASE + 7), ==, 0x50);
    g_assert_cmphex(inb(qts, IDE_CTL_BASE + 2), ==, 0x50);
    /* Native mode is gated by 5Ah too */
    cfg_writel(qts, 1, 0x58, fer & ~(1u << 18)  /* 5Ah bit 2 */);
    g_assert_cmphex(inb(qts, IDE_CMD_BASE + 7), ==, 0xff);
    qtest_quit(qts);
}

/*
 * Drive interrupt: sets BM status Interrupt, reaches the router unless
 * 41h masks it, and BM command bit 2 clears the status bit (erratum).
 */
static void test_ide_irq_erratum(void)
{
    QTestState *qts = qtest_init(IDE_ARGS);

    qtest_irq_intercept_out_named(qts, IDE_PATH, "isa-irq");
    ide_go_native(qts);
    outb(qts, IDE_CMD_BASE + 6, 0xa0);
    outb(qts, IDE_CMD_BASE + 7, 0xec);              /* IDENTIFY DEVICE */
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(inb(qts, IDE_BM_BASE + 2) & 0x04, ==, 0x04);

    cfg_writel(qts, 0, 0x40, 0x00000100);           /* 41h: mask ch1 */
    g_assert_false(qtest_get_irq(qts, 0));
    cfg_writel(qts, 0, 0x40, 0);
    g_assert_true(qtest_get_irq(qts, 0));

    outb(qts, IDE_BM_BASE, 0x04);
    g_assert_cmphex(inb(qts, IDE_BM_BASE + 2) & 0x04, ==, 0);
    g_assert_cmphex(inb(qts, IDE_BM_BASE), ==, 0);   /* bits 1/2 read 0 */

    inb(qts, IDE_CMD_BASE + 7);                       /* status read acks */
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/* Elroy 0 PCI memory: PCI address A at ELROY0_MEM_CPU + (A - ELROY0_MEM) */
#define ELROY0_MEM          0xf4000000u
#define ELROY0_MEM_CPU      0xfffffffff4000000ULL
#define OHCI_BAR            0xf4100000u
#define NUSB_BAR            0xf4101000u

/*
 * Astro maps the distributed LMMIO window only when firmware programs it;
 * qtest runs no firmware.  LMMIO_DIST_ROUTE 22: 4 MiB per rope.
 */
#define ASTRO_HPA_CPU       0xfffffffffed00000ULL
#define ASTRO_LMMIO_ROUTE   0x370

static void lmmio_enable(QTestState *qts)
{
    qtest_writeq(qts, ASTRO_HPA_CPU + ASTRO_LMMIO_ROUTE,
                 bswap64(22ULL << 58));
}

static uint32_t mem_readl(QTestState *qts, uint32_t pci_addr)
{
    return bswap32(qtest_readl(qts, ELROY0_MEM_CPU + pci_addr - ELROY0_MEM));
}

static void mem_writel(QTestState *qts, uint32_t pci_addr, uint32_t val)
{
    qtest_writel(qts, ELROY0_MEM_CPU + pci_addr - ELROY0_MEM, bswap32(val));
}

/* Function 2 configuration space (PC87560UBD tables 142, p.292-294) */
static void test_usb_config(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    g_assert_cmphex(cfg_readl(qts, 2, 0x04), ==, 0x02000000);
    g_assert_cmphex(cfg_readl(qts, 2, 0x3c), ==, 0x000004ff);
    cfg_writel(qts, 2, 0x04, 0xffff);
    g_assert_cmphex(cfg_readl(qts, 2, 0x04) & 0xffff, ==, 0x0147);
    cfg_writel(qts, 2, 0x10, 0xffffffff);
    cfg_writel(qts, 2, 0x14, 0xffffffff);
    g_assert_cmphex(cfg_readl(qts, 2, 0x10), ==, 0xfffff000);
    g_assert_cmphex(cfg_readl(qts, 2, 0x14), ==, 0xfffff000);
    /* no device-specific registers */
    cfg_writel(qts, 2, 0x40, 0xffffffff);
    g_assert_cmphex(cfg_readl(qts, 2, 0x40), ==, 0);
    qtest_quit(qts);
}

/*
 * OHCI BAR: HcRevision 110h, three root-hub ports with port 1 tied off,
 * Hce registers read 0; NUSBBAR reset values.
 */
static void test_usb_regs(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults "
                                 "-device usb-kbd,port=2");

    lmmio_enable(qts);
    cfg_writel(qts, 2, 0x10, OHCI_BAR);
    cfg_writel(qts, 2, 0x14, NUSB_BAR);
    cfg_writel(qts, 2, 0x04, 0x0006);
    g_assert_cmphex(mem_readl(qts, OHCI_BAR + 0x00), ==, 0x00000110);
    g_assert_cmphex(mem_readl(qts, OHCI_BAR + 0x48) & 0xff, ==, 3);
    g_assert_cmphex(mem_readl(qts, OHCI_BAR + 0x54) & 1, ==, 0);
    g_assert_cmphex(mem_readl(qts, OHCI_BAR + 0x58) & 1, ==, 1);
    mem_writel(qts, OHCI_BAR + 0x100, 0xffffffff);
    g_assert_cmphex(mem_readl(qts, OHCI_BAR + 0x100), ==, 0);

    g_assert_cmphex(mem_readl(qts, NUSB_BAR + 0x00), ==, 0x00000014);
    g_assert_cmphex(mem_readl(qts, NUSB_BAR + 0x34), ==, 0x000a0a0a);
    mem_writel(qts, NUSB_BAR + 0x00, 0xffffffff);
    g_assert_cmphex(mem_readl(qts, NUSB_BAR + 0x00), ==, 0xff0103ff);
    qtest_quit(qts);
}

static uint8_t fdc_cmd(QTestState *qts, const uint8_t *bytes, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        outb(qts, 0x3f5, bytes[i]);
    }
    return inb(qts, 0x3f4);                     /* MSR */
}

/* NSC commands of the PC87560 FDC (p.117): PART ID 73h, MODE */
static void test_fdc_commands(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");
    static const uint8_t partid[] = { 0x18 };
    static const uint8_t version[] = { 0x10 };
    static const uint8_t mode[] = { 0x01, 0x00, 0x00, 0xc8, 0x00 };

    g_assert_cmphex(fdc_cmd(qts, partid, 1) & 0xd0, ==, 0xd0);
    g_assert_cmphex(inb(qts, 0x3f5), ==, 0x73);
    fdc_cmd(qts, version, 1);
    g_assert_cmphex(inb(qts, 0x3f5), ==, 0x90);
    /* MODE takes four parameters and has no result phase */
    g_assert_cmphex(fdc_cmd(qts, mode, 5), ==, 0x80);
    qtest_quit(qts);
}

/* FDC configuration mode (p.95-96): 5Dh bit 0, then 55h / AAh at FDCBAR */
static void test_fdc_config_mode(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    outb(qts, 0x3f0, 0x55);                     /* 5Dh bit 0 clear: ignored */
    outb(qts, 0x3f0, 0x00);
    g_assert_cmphex(inb(qts, 0x3f1), !=, 0x10);

    cfg_writel(qts, 1, 0x5c, 0x00000100);       /* 5Dh bit 0 */
    outb(qts, 0x3f0, 0x55);
    outb(qts, 0x3f0, 0x00);
    g_assert_cmphex(inb(qts, 0x3f1), ==, 0x10); /* FDCDID */
    outb(qts, 0x3f0, 0x07);
    g_assert_cmphex(inb(qts, 0x3f1), ==, 0x05);
    outb(qts, 0x3f1, 0x12);
    g_assert_cmphex(inb(qts, 0x3f1), ==, 0x12);
    outb(qts, 0x3f0, 0x00);
    outb(qts, 0x3f1, 0xff);                     /* FDCDID is read-only */
    g_assert_cmphex(inb(qts, 0x3f1), ==, 0x10);
    outb(qts, 0x3f0, 0xaa);
    g_assert_cmphex(inb(qts, 0x3f1), !=, 0x10);
    qtest_quit(qts);
}

/* 8237 high pages 480h-48Fh, cleared by low page or base writes (p.160-166) */
static void test_dma_hipage(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");
    uint32_t fer;

    outb(qts, 0x483, 0x12);                     /* channel 1 */
    g_assert_cmphex(inb(qts, 0x483), ==, 0x12);
    outb(qts, 0x83, 0x05);                      /* its low page */
    g_assert_cmphex(inb(qts, 0x483), ==, 0);

    outb(qts, 0x48b, 0x34);                     /* channel 5 */
    g_assert_cmphex(inb(qts, 0x48b), ==, 0x34);
    outb(qts, 0xc4, 0x00);                      /* its base address */
    g_assert_cmphex(inb(qts, 0x48b), ==, 0);
    g_assert_cmphex(inb(qts, 0x484), ==, 0);    /* unused offset */

    fer = cfg_readl(qts, 1, 0x58);
    cfg_writel(qts, 1, 0x58, fer & ~(1u << 27));    /* 5Ah bit 11: 8237 #1 */
    outb(qts, 0x483, 0x56);
    g_assert_cmphex(inb(qts, 0x483), ==, 0xff);
    qtest_quit(qts);
}

/* 8254 at 40h, port 61h gate/OUT2, 5Ah bit 10 decode (p.392, ch. 20) */
static void test_pit(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");
    uint32_t fer;
    uint8_t a, b;

    outb(qts, 0x61, 0x01);                      /* gate counter 2 */
    outb(qts, 0x43, 0xb0);                      /* ch2, lo/hi, mode 0 */
    outb(qts, 0x42, 0x34);
    outb(qts, 0x42, 0x12);
    outb(qts, 0x43, 0x80);                      /* latch ch2 */
    g_assert_cmphex(inb(qts, 0x42), ==, 0x34);
    g_assert_cmphex(inb(qts, 0x42), ==, 0x12);

    a = inb(qts, 0x61);
    b = inb(qts, 0x61);
    g_assert_cmphex(a & 0x2f, ==, 0x01);         /* gate on, OUT2 low */
    g_assert_cmphex((a ^ b) & 0x10, ==, 0x10);   /* refresh toggles */
    qtest_clock_step(qts, 10 * 1000 * 1000);     /* > 0x1234 ticks */
    g_assert_cmphex(inb(qts, 0x61) & 0x20, ==, 0x20);

    fer = cfg_readl(qts, 1, 0x58);
    cfg_writel(qts, 1, 0x58, fer & ~(1u << 26)); /* 5Ah bit 10 */
    outb(qts, 0x43, 0x80);
    g_assert_cmphex(inb(qts, 0x42), ==, 0xff);
    qtest_quit(qts);
}

/* Port 92h sticky bits, APM B2h/B3h storage (p.389-390, p.394-395) */
static void test_fixed_ports(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    outb(qts, 0x92, 0xff);
    g_assert_cmphex(inb(qts, 0x92), ==, 0x2b);
    outb(qts, 0x92, 0x00);
    g_assert_cmphex(inb(qts, 0x92), ==, 0x28);
    outb(qts, 0xb2, 0x5a);
    outb(qts, 0xb3, 0xa5);
    g_assert_cmphex(inb(qts, 0xb2), ==, 0x5a);
    g_assert_cmphex(inb(qts, 0xb3), ==, 0xa5);
    qtest_quit(qts);
}

#define SP1     0x3f8

/* SCC register banks (PC87560UBD p.338-350) */
static void test_uart_banks(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    outb(qts, SP1 + 3, 0xe4);                   /* bank 3 */
    g_assert_cmphex(inb(qts, SP1 + 0), ==, 0x20);   /* module ID */
    g_assert_cmphex(inb(qts, SP1 + 1), ==, 0x00);   /* shadow LCR */
    g_assert_cmphex(inb(qts, SP1 + 3), ==, 0xe4);   /* BSR */

    /* Bank 2 divisor: no fallback, visible through bank 1 */
    outb(qts, SP1 + 3, 0xe0);
    outb(qts, SP1 + 0, 0x0c);
    outb(qts, SP1 + 1, 0x00);
    g_assert_cmphex(inb(qts, SP1 + 0), ==, 0x0c);
    outb(qts, SP1 + 3, 0x83);                   /* bank 1 = DLAB */
    g_assert_cmphex(inb(qts, SP1 + 0), ==, 0x0c);
    outb(qts, SP1 + 3, 0x03);                   /* bank 0, 8N1 */
    outb(qts, SP1 + 3, 0xe4);
    g_assert_cmphex(inb(qts, SP1 + 1), ==, 0x03);

    /* Extended mode: IER bits 7:4; a bank 1 divisor access falls back */
    outb(qts, SP1 + 3, 0xe0);
    outb(qts, SP1 + 2, 0x01);                   /* EXCR1.EXT_SL */
    outb(qts, SP1 + 3, 0x03);
    outb(qts, SP1 + 1, 0xf0);
    g_assert_cmphex(inb(qts, SP1 + 1), ==, 0xf0);
    outb(qts, SP1 + 3, 0x83);
    inb(qts, SP1 + 0);
    outb(qts, SP1 + 3, 0xe0);
    g_assert_cmphex(inb(qts, SP1 + 2), ==, 0x00);
    outb(qts, SP1 + 3, 0x03);
    g_assert_cmphex(inb(qts, SP1 + 1), ==, 0x00);

    /* Divisor lock: bank 1 offsets 0/1 become scratch storage */
    outb(qts, SP1 + 3, 0xe0);
    outb(qts, SP1 + 4, 0x80);                   /* EXCR2.LOCK */
    outb(qts, SP1 + 3, 0x83);
    outb(qts, SP1 + 0, 0x55);
    g_assert_cmphex(inb(qts, SP1 + 0), ==, 0x55);
    outb(qts, SP1 + 3, 0xe0);
    g_assert_cmphex(inb(qts, SP1 + 0), ==, 0x0c);
    qtest_quit(qts);
}

/* MCR.OUT2 gates the UART interrupt in non-extended mode (p.338) */
static void test_uart_out2(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    outb(qts, 0x20, 0x0a);                      /* OCW3: read IRR */
    outb(qts, SP1 + 1, 0x02);                   /* IER: THRE */
    g_assert_cmphex(inb(qts, 0x20) & 0x10, ==, 0);
    outb(qts, SP1 + 4, 0x08);                   /* MCR.OUT2 */
    g_assert_cmphex(inb(qts, 0x20) & 0x10, ==, 0x10);
    qtest_quit(qts);
}

#define ECP     (0x378 + 0x400)

/* ECP registers at PPBAR+400h, enabled by 5Fh (p.328-335) */
static void test_pp_ecp(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults "
                                 "-parallel null");

    g_assert_cmphex(inb(qts, ECP + 2), ==, 0xff);   /* 5Fh = 0: compatible */
    cfg_writel(qts, 1, 0x5c, 0x04000000);            /* 5Fh = 100b: ECP */
    g_assert_cmphex(inb(qts, ECP + 2), ==, 0x15);   /* ECR reset */

    outb(qts, ECP + 2, 0xe0);                        /* configuration mode */
    g_assert_cmphex(inb(qts, ECP + 0), ==, 0x14);   /* CNFGA */

    outb(qts, ECP + 2, 0xc0);                        /* test FIFO mode */
    outb(qts, ECP + 0, 0x11);
    outb(qts, ECP + 0, 0x22);
    g_assert_cmphex(inb(qts, ECP + 2) & 0x03, ==, 0);
    g_assert_cmphex(inb(qts, ECP + 0), ==, 0x11);
    g_assert_cmphex(inb(qts, ECP + 0), ==, 0x22);
    g_assert_cmphex(inb(qts, ECP + 2) & 0x03, ==, 0x01);    /* empty */

    /* Forward FIFO mode: serviceIntr is set once software clears it */
    outb(qts, ECP + 2, 0x40);
    g_assert_cmphex(inb(qts, ECP + 2) & 0x04, ==, 0x04);

    cfg_writel(qts, 1, 0x5c, 0);
    g_assert_cmphex(inb(qts, ECP + 2), ==, 0xff);
    qtest_quit(qts);
}

static void test_uart_relocate(void)
{
    QTestState *qts = qtest_init("-machine C3700,superio=ns87560 -nodefaults");

    outb(qts, 0x3ff, 0x5a);                     /* scratch register */
    g_assert_cmphex(inb(qts, 0x3ff), ==, 0x5a);
    cfg_writel(qts, 1, 0x94, 0x3e8);
    outb(qts, 0x3ef, 0xa5);
    g_assert_cmphex(inb(qts, 0x3ef), ==, 0xa5);
    g_assert_cmphex(inb(qts, 0x3ff), ==, 0xff);   /* old base unclaimed */
    cfg_writel(qts, 1, 0x58, 0xffbd0024);          /* 5Ah: SP1 off */
    g_assert_cmphex(inb(qts, 0x3ef), ==, 0xff);
    qtest_quit(qts);
}

/* Linux: outb(1, ACPIBAR + 1Fh) must read back bit 0 ("USB regulator") */
static void check_acpi_usbreg(const char *args)
{
    QTestState *qts = qtest_init(args);
    uint16_t base = cfg_readl(qts, 1, 0x88) & ~0x1f;

    g_assert_cmphex(base, ==, 0x800);
    outb(qts, base + 0x1f, 0x01);
    g_assert_cmphex(inb(qts, base + 0x1f), ==, 0x01);
    qtest_quit(qts);
}

static void test_acpi_usbreg(void)
{
    check_acpi_usbreg("-machine C3700,superio=ns87560 -nodefaults");
}

/* Same with default devices: firmware-placed BARs must not collide */
static void test_acpi_usbreg_defaults(void)
{
    check_acpi_usbreg("-machine C3700,superio=ns87560");
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
    qtest_add_func("/ns87560/uart-relocate", test_uart_relocate);
    qtest_add_func("/ns87560/ide-progif", test_ide_progif);
    qtest_add_func("/ns87560/ide-regs", test_ide_regs);
    qtest_add_func("/ns87560/ide-decode", test_ide_decode);
    qtest_add_func("/ns87560/ide-irq-erratum", test_ide_irq_erratum);
    qtest_add_func("/ns87560/usb-config", test_usb_config);
    qtest_add_func("/ns87560/usb-regs", test_usb_regs);
    qtest_add_func("/ns87560/fdc-commands", test_fdc_commands);
    qtest_add_func("/ns87560/fdc-config-mode", test_fdc_config_mode);
    qtest_add_func("/ns87560/dma-hipage", test_dma_hipage);
    qtest_add_func("/ns87560/pit", test_pit);
    qtest_add_func("/ns87560/fixed-ports", test_fixed_ports);
    qtest_add_func("/ns87560/uart-banks", test_uart_banks);
    qtest_add_func("/ns87560/uart-out2", test_uart_out2);
    qtest_add_func("/ns87560/pp-ecp", test_pp_ecp);
    qtest_add_func("/ns87560/acpi-usbreg", test_acpi_usbreg);
    qtest_add_func("/ns87560/acpi-usbreg-defaults", test_acpi_usbreg_defaults);
    return g_test_run();
}

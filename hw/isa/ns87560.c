/*
 * National Semiconductor PC87560 (NS87560) PCI System I/O
 *
 * The chip is one PCI device with three functions: a PC87415-compatible
 * IDE controller (function 0), the legacy I/O bridge (function 1) and an
 * OHCI USB host controller (function 2).  This file implements the
 * container that creates and wires the three functions, and function 1.
 *
 * Reference: National Semiconductor, "PC87560UBD PCI System I/O",
 * October 1998.
 *
 * Copyright (c) 2026 Soumyajyotii Ssarkar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "chardev/char-fe.h"
#include "hw/char/parallel-isa.h"
#include "hw/char/serial-isa.h"
#include "hw/block/fdc.h"
#include "hw/intc/i8259.h"
#include "hw/isa/i8259_internal.h"
#include "hw/isa/isa.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/i8254.h"
#include "hw/timer/i8254_internal.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/range.h"
#include "trace.h"

#define NS87560_LIO_REVISION    0x01
#define NS87560_LIO_CMD_RESET   0x000f  /* I/O, memory, master, special cycles */
#define NS87560_LIO_CMD_WMASK   0x014c  /* bits 2, 3, 6 and 8 are writable */
#define NS87560_ACPIBAR_RESET   0x00000801  /* see ns87560_lio_regs[] */

#define CFG_FER                 0x5a
#define FER_FDC                 BIT(0)
#define FER_SP1                 BIT(1)
#define FER_IDE1                BIT(2)
#define FER_SP2                 BIT(3)
#define FER_IDE2                BIT(4)
#define FER_PP                  BIT(5)
#define FER_P92                 BIT(7)
#define FER_PIC                 BIT(9)
#define FER_PIT                 BIT(10)
#define FER_P61                 BIT(14)
#define FER_APM                 BIT(15)
#define FER_DMA1                BIT(11)
#define FER_DMA2                BIT(12)
#define CFG_SIOCFG              0x5c
#define SIOCFG_UIR_BANKS        BIT(6)
#define CFG_SFE                 0x5d
#define SFE_FDC_CFG             BIT(0)
#define CFG_DMART1              0x63
#define CFG_FDCBAR              0x90
#define NS87560_FDC_DID         0x10
#define NS87560_FDC_PARTID      0x73
#define CFG_ACPIBAR             0x88
#define CFG_SP1BAR              0x94
#define CFG_SP2BAR              0x98
#define CFG_PPBAR               0x9c
#define CFG_ELCR                0x67
#define CFG_IRCR1               0x69
#define CFG_IRCR9               0x71
#define CFG_RSVD7E              0x7e
#define CFG_ARB                 0x72
#define NS87560_ARB_ROTATE      0x4688

/* 8259 inputs the router may drive: 1, 3-7, 9-12, 14, 15 (p.268) */
#define NS87560_IRQ_ROUTABLE    0xdefa
/* ELCR: reserved bits 0, 2 (master) and IRQ8, IRQ13 (slave) */
#define NS87560_ELCR_MASK_MASTER    0xfa
#define NS87560_ELCR_MASK_SLAVE     0xde

/*
 * Function 1 configuration registers 40h-FFh: reset value, writable bits
 * and, for registers whose side effects are not modelled, the bits whose
 * setting is reported once with LOG_UNIMP.  Offsets not listed are
 * reserved: they read as 0 and ignore writes.
 * Source: PC87560UBD datasheet (1998), chapter 20, p.240-295.
 */
typedef struct NS87560LIOReg {
    uint8_t off;
    uint8_t size;
    uint32_t reset;
    uint32_t wmask;
    uint32_t unimp;
    const char *name;
} NS87560LIOReg;

static const NS87560LIOReg ns87560_lio_regs[] = {
    { 0x40, 4, 0x00000000, 0xffffffff, 0x00000001, "FX region 0" },
    { 0x44, 4, 0x00000000, 0xffffffff, 0x00000001, "FX region 1" },
    { 0x48, 4, 0x00000000, 0xffffffff, 0x00000001, "FX region 2" },
    { 0x4c, 4, 0x00000000, 0xffffff83, 0x00000003, "distributed DMA" },
    { 0x50, 4, 0x00000000, 0xffffffc3, 0x00000001, "FIR scatter/gather DMA" },
    { 0x58, 2, 0x0024,     0xb03f,     0x0013,     "subtractive decode/flash" },
    { 0x5a, 2, 0xffbf,     0xffbf,     0,          "function enable" },
    { 0x5c, 1, 0x00,       0xff,       0,          "system I/O config" },
    { 0x5d, 1, 0x00,       0x03,       0,          "special function enable" },
    { 0x5e, 1, 0x10,       0x00,       0,          "parallel port ID" },
    { 0x5f, 1, 0x00,       0x07,       0,          "parallel port mode" },
    { 0x61, 1, 0x00,       0x0f,       0,          "DMA routing 4" },
    { 0x62, 1, 0x00,       0xff,       0xff,       "ROM chip selects" },
    { 0x63, 1, 0x67,       0x77,       0,          "DMA routing 1" },
    { 0x64, 1, 0x00,       0xff,       0,          "DMA routing 2" },
    { 0x65, 1, 0x00,       0xff,       0,          "DMA routing 3" },
    { 0x66, 1, 0x00,       0x7f,       0,          "DMA channel control" },
    { 0x67, 1, 0x00,       0xfa,       0,          "ELCR 1" },
    { 0x68, 1, 0x00,       0xde,       0,          "ELCR 2" },
    { 0x69, 1, 0x34,       0xff,       0,          "IRCR1" },
    { 0x6a, 1, 0x65,       0xff,       0,          "IRCR2" },
    { 0x6b, 1, 0xfe,       0xff,       0,          "IRCR3" },
    { 0x6c, 1, 0x00,       0xff,       0,          "IRCR4" },
    { 0x6d, 1, 0x00,       0xff,       0,          "IRCR5" },
    { 0x6e, 1, 0xc1,       0xff,       0,          "IRCR6" },
    { 0x6f, 1, 0x00,       0xff,       0,          "IRCR7" },
    { 0x70, 1, 0x00,       0xff,       0,          "IRCR8" },
    { 0x71, 1, 0x00,       0x0f,       0,          "IRCR9" },
    { 0x72, 2, 0x4c88,     0x7fff,     0,          "arbiter control" },
    { 0x74, 2, 0x0000,     0xfffe,     0,          "FX region 2 mask" },
    { 0x76, 2, 0x0000,     0xffff,     0,          "FX timing" },
    { 0x78, 1, 0x00,       0x0f,       0,          "FX bus control" },
    { 0x79, 1, 0x00,       0xff,       0x70,       "native IRQ pins (GPIO4-6)" },
    { 0x7a, 1, 0x00,       0xff,       0,          "GPIO direction" },
    { 0x7b, 1, 0x00,       0xff,       0x80,       "serial IRQ" },
    { 0x7c, 2, 0x0000,     0xef7f,     0,          "serial IRQ enable" },
    { 0x7e, 1, 0x01,       0x0f,       0,          "reserved 7Eh" },
    { 0x7f, 1, 0x00,       0x3f,       0x3f,       "audio chip selects" },
    { 0x80, 4, 0x00000000, 0x0f1ff01f, 0x00000001, "FX memory range" },
    { 0x84, 4, 0x00000060, 0xfffffff8, 0,          "KBCBAR" },
    /*
     * ACPIBAR resets to 0 on the chip; firmware programs it.  The model
     * starts at 800h: SeaBIOS assigns PCI I/O BARs from 1000h upwards, and
     * Linux writes ACPIBAR + 1Fh without programming the base.
     */
    { 0x88, 4, NS87560_ACPIBAR_RESET, 0xffffffe1, 0, "ACPIBAR" },
    { 0x8c, 4, 0xffffff01, 0xffffff00, 0,          "PMBAR" },
    { 0x90, 4, 0x000003f1, 0xfffffff8, 0,          "FDCBAR" },
    { 0x94, 4, 0x000003f9, 0xfffffff8, 0,          "SP1BAR" },
    { 0x98, 4, 0x000002f9, 0xfffffff8, 0,          "SP2BAR" },
    { 0x9c, 4, 0x00000379, 0xfffffff8, 0,          "PPBAR" },
    { 0xff, 1, 0x00,       0x0f,       0,          "test register" },
};

static uint32_t lio_cfg(NS87560LIOState *s, const NS87560LIOReg *r)
{
    uint8_t *c = s->dev.config + r->off;

    return r->size == 4 ? pci_get_long(c) : r->size == 2 ? pci_get_word(c) : *c;
}

static void lio_cfg_set(uint8_t *c, const NS87560LIOReg *r, uint32_t v)
{
    if (r->size == 4) {
        pci_set_long(c + r->off, v);
    } else if (r->size == 2) {
        pci_set_word(c + r->off, v);
    } else {
        c[r->off] = v;
    }
}

static void ns87560_lio_init_masks(NS87560LIOState *s)
{
    uint8_t *w = s->dev.wmask;
    int i;

    memset(w + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    pci_set_word(w + PCI_COMMAND, NS87560_LIO_CMD_WMASK);
    w[PCI_CLASS_PROG] = 0xff;
    w[PCI_INTERRUPT_LINE] = 0;
    for (i = 0; i < ARRAY_SIZE(ns87560_lio_regs); i++) {
        lio_cfg_set(w, &ns87560_lio_regs[i], ns87560_lio_regs[i].wmask);
    }
}

static void ns87560_lio_config_defaults(NS87560LIOState *s)
{
    uint8_t *c = s->dev.config;
    int i;

    pci_set_word(c + PCI_COMMAND, NS87560_LIO_CMD_RESET);
    c[PCI_CLASS_PROG] = 0;
    c[PCI_CACHE_LINE_SIZE] = 0;
    c[PCI_LATENCY_TIMER] = 0;
    memset(c + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    for (i = 0; i < ARRAY_SIZE(ns87560_lio_regs); i++) {
        lio_cfg_set(c, &ns87560_lio_regs[i], ns87560_lio_regs[i].reset);
    }
    s->unimp_logged = 0;
}

/* Report each unmodelled side-effect register once. */
static void ns87560_lio_check_unimp(NS87560LIOState *s, uint32_t addr, int len)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(ns87560_lio_regs); i++) {
        const NS87560LIOReg *r = &ns87560_lio_regs[i];
        uint32_t v;

        if (!ranges_overlap(addr, len, r->off, r->size)) {
            continue;
        }
        v = lio_cfg(s, r);
        if ((v & r->unimp) && !(s->unimp_logged & BIT_ULL(i))) {
            s->unimp_logged |= BIT_ULL(i);
            qemu_log_mask(LOG_UNIMP, "ns87560: %s (config 0x%02x = 0x%x) "
                          "has no effect in this model\n", r->name, r->off, v);
        }
        if (r->off == CFG_RSVD7E && v != 0x01) {
            qemu_log_mask(LOG_GUEST_ERROR, "ns87560: reserved config 0x7e "
                          "written with 0x%02x (must stay 0x01)\n", v);
        }
    }
}

/* ---- Interrupt router (IRCR1-9, p.268-276) ----------------------------- */

/* Source n is routed by nibble n % 2 of IRCR at 69h + n / 2; value = IRQ. */
static int ns87560_route_of(NS87560LIOState *s, int src)
{
    int irq;

    if (src == NS87560_SRC_RSVD) {
        return 0;
    }
    irq = (s->dev.config[CFG_IRCR1 + src / 2] >> (4 * (src & 1))) & 0xf;
    return (NS87560_IRQ_ROUTABLE & BIT(irq)) ? irq : 0;
}

/*
 * Rebuild every 8259 input from the source levels and the routing, so a
 * source moved away from an asserted input lowers it at once.
 */
static void ns87560_router_recompute(NS87560LIOState *s, bool force)
{
    uint16_t new = 0, changed;
    int src, irq;

    for (src = 0; src < NS87560_SRC_COUNT; src++) {
        if (s->src_level & BIT(src)) {
            irq = ns87560_route_of(s, src);
            if (irq) {
                new |= BIT(irq);
            }
        }
    }
    changed = force ? NS87560_IRQ_ROUTABLE : new ^ s->pic_level;
    s->pic_level = new;
    for (irq = 0; irq < ISA_NUM_IRQS; irq++) {
        if (changed & BIT(irq)) {
            trace_ns87560_router_pic(irq, !!(new & BIT(irq)));
            qemu_set_irq(s->isa_irqs[irq], !!(new & BIT(irq)));
        }
    }
}

static void ns87560_router_set_irq(void *opaque, int src, int level)
{
    NS87560LIOState *s = opaque;

    s->src_level = deposit32(s->src_level, src, 1, !!level);
    trace_ns87560_router_source(src, !!level);
    ns87560_router_recompute(s, false);
}

/*
 * Request lines of the ISA peripherals, installed in the ISA bus input
 * table at their labels; IRCR1/IRCR2 decide where they really go.
 */
#define SCC_MCR_OUT2    0x08
#define SCC_MCR_LOOP    0x10
#define EXCR1_EXT_SL    BIT(0)
#define EXCR1_LOOP      BIT(4)
#define EXCR1_ETDLBK    BIT(5)
#define EXCR1_BTEST     BIT(7)
#define EXCR2_PRESL(x)  (((x) >> 4) & 3)
#define EXCR2_LOCK      BIT(7)

/*
 * In non-extended mode MCR.OUT2 is the UART's interrupt enable; it is
 * forced on in loopback and in extended mode (p.338).  QEMU's 16550 core
 * does not gate its line, so the gate is applied here.
 */
static void ns87560_periph_update(NS87560LIOState *s)
{
    static const int src[] = {
        NS87560_SRC_SP1, NS87560_SRC_SP2, NS87560_SRC_PP, NS87560_SRC_FDC,
    };
    int n;

    for (n = 0; n < ARRAY_SIZE(src); n++) {
        bool level = s->periph_level & BIT(n);

        if (n < 2) {
            NS87560SCCState *sc = &s->scc[n];

            /* sc->ss is unset while the serial core realizes */
            level &= sc->ss && ((sc->ss->mcr & (SCC_MCR_OUT2 | SCC_MCR_LOOP)) ||
                                (sc->excr1 & EXCR1_EXT_SL));
        }
        ns87560_router_set_irq(s, src[n], level);
    }
}

static void ns87560_periph_irq(void *opaque, int n, int level)
{
    NS87560LIOState *s = opaque;

    s->periph_level = deposit32(s->periph_level, n, 1, !!level);
    ns87560_periph_update(s);
}

/* Trigger mode comes from config 67h/68h; the chip has no I/O 4D0h/4D1h. */
static void ns87560_lio_update_elcr(NS87560LIOState *s)
{
    I8259CommonState *m = I8259_COMMON(s->pic[0]);
    I8259CommonState *sl = I8259_COMMON(s->pic[1]);

    m->elcr = s->dev.config[CFG_ELCR] & m->elcr_mask;
    sl->elcr = s->dev.config[CFG_ELCR + 1] & sl->elcr_mask;
    trace_ns87560_elcr(m->elcr, sl->elcr);
    ns87560_router_recompute(s, true);
}

/*
 * ICW1.LTIM is ignored by the chip (p.198): trigger mode is set only by
 * the ELCR.  Filter it out before the command reaches QEMU's 8259.
 */
static uint64_t ns87560_pic_cmd_read(void *opaque, hwaddr addr, unsigned size)
{
    I8259CommonState *p = opaque;
    uint64_t v;

    memory_region_dispatch_read(&p->base_io, 0, &v, MO_8,
                                MEMTXATTRS_UNSPECIFIED);
    return v;
}

static void ns87560_pic_cmd_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    I8259CommonState *p = opaque;

    if (val & 0x10) {           /* ICW1 */
        val &= ~0x08;
    }
    memory_region_dispatch_write(&p->base_io, 0, val, MO_8,
                                 MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps ns87560_pic_cmd_ops = {
    .read = ns87560_pic_cmd_read,
    .write = ns87560_pic_cmd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* 8259 master INT = the chip's CPUINT pin */
static void ns87560_lio_i8259_irq(void *opaque, int n, int level)
{
    NS87560LIOState *s = opaque;

    trace_ns87560_cascade(level);
    qemu_set_irq(s->cascade_out, level);
}

/* ---- Decode enables and relocation (5Ah, 84h-9Fh) ---------------------- */

static hwaddr lio_win_base(NS87560LIOState *s, int off)
{
    return pci_get_long(s->dev.config + off) & ~7u;
}

static void ns87560_lio_decode_update(NS87560LIOState *s)
{
    uint16_t fer = pci_get_word(s->dev.config + CFG_FER);
    uint32_t acpibar = pci_get_long(s->dev.config + CFG_ACPIBAR);
    hwaddr fdc = lio_win_base(s, CFG_FDCBAR);
    int i;

    memory_region_transaction_begin();
    isa_fdc_set_iobase(s->fdc, fdc);
    isa_fdc_set_enabled(s->fdc, fer & FER_FDC);
    memory_region_set_address(&s->fdccfg_idx_io, fdc);
    memory_region_set_enabled(&s->fdccfg_idx_io, fer & FER_FDC);
    memory_region_set_address(&s->fdccfg_data_io, fdc + 1);
    memory_region_set_enabled(&s->fdccfg_data_io,
                              (fer & FER_FDC) && s->fdccfg_mode);
    for (i = 0; i < 2; i++) {
        bool en = fer & (i ? FER_DMA2 : FER_DMA1);

        memory_region_set_enabled(&s->dma[i]->channel_io, en);
        memory_region_set_enabled(&s->dma[i]->cont_io, en);
        portio_list_set_enabled(&s->dma[i]->portio_page, en);
    }
    memory_region_set_enabled(&s->dma_hipage_io, fer & (FER_DMA1 | FER_DMA2));
    memory_region_set_enabled(&s->pit->ioports, fer & FER_PIT);
    memory_region_set_enabled(&s->port61_io, fer & FER_P61);
    memory_region_set_enabled(&s->port92_io, fer & FER_P92);
    memory_region_set_enabled(&s->apm_io, fer & FER_APM);
    for (i = 0; i < 2; i++) {
        memory_region_set_address(&s->scc[i].io,
                                  lio_win_base(s, i ? CFG_SP2BAR : CFG_SP1BAR));
        memory_region_set_enabled(&s->scc[i].io, fer & (i ? FER_SP2 : FER_SP1));
    }
    if (s->parallel) {
        isa_parallel_set_iobase(s->parallel, lio_win_base(s, CFG_PPBAR));
        isa_parallel_set_enabled(s->parallel, fer & FER_PP);
    }
    for (i = 0; i < 2; i++) {
        memory_region_set_enabled(&I8259_COMMON(s->pic[i])->base_io,
                                  fer & FER_PIC);
        memory_region_set_enabled(&s->pic_cmd_io[i], fer & FER_PIC);
    }
    memory_region_set_address(&s->acpi_io, acpibar & ~0x1fu);
    memory_region_set_enabled(&s->acpi_io, acpibar & 1);
    if (s->ide_notify) {
        s->ide_notify(s->ide);
    }
    memory_region_transaction_commit();
    trace_ns87560_decode(fer, lio_win_base(s, CFG_SP1BAR),
                         lio_win_base(s, CFG_SP2BAR),
                         lio_win_base(s, CFG_PPBAR), acpibar & ~0x1fu);
}

/* ---- ACPI block (ACPIBAR, p.215 table 130) ------------------------------ */

static const uint8_t acpi_wmask[0x20] = {
    [0x02] = 0xff, [0x03] = 0xff, [0x04] = 0xff, [0x05] = 0xff,
    [0x0e] = 0xff, [0x0f] = 0xff, [0x11] = 0xff, [0x12] = 0xff,
    [0x1f] = 0x01,              /* USB regulator control */
};
static const uint8_t acpi_w1c[0x20] = {
    [0x00] = 0xff, [0x01] = 0xff, [0x0c] = 0xff, [0x0d] = 0xff,
    [0x10] = 0xff,
};

/*
 * SCC register banks (p.338-350).  The Bank Select Register at offset 3
 * is the LCR for banks 0/1; values 11xxxxx0b select banks 2-7.  Banks
 * 0/1 are the 16550, served by QEMU's serial core.  Bank 2 holds the
 * extended controls, bank 3 the module ID and shadows, banks 4-7 the
 * SCC2 infrared unit (storage only).
 */
#define NS87560_SCC_DID 0x20

static uint64_t scc_core_read(NS87560SCCState *sc, int off)
{
    return serial_io_ops.read(sc->ss, off, 1);
}

static void scc_core_write(NS87560SCCState *sc, int off, uint8_t val)
{
    serial_io_ops.write(sc->ss, off, val, 1);
}

static bool scc_locked(NS87560SCCState *sc)
{
    return (sc->excr2 & EXCR2_LOCK) && !(sc->excr1 & EXCR1_EXT_SL);
}

/* BSR value to bank (p.342-344); -1 for the reserved codes */
static int scc_bank_of(uint8_t v)
{
    if (!(v & 0x80)) {
        return 0;
    }
    if ((v & 0xc0) == 0x80 || (v & 0x03)) {
        return 1;
    }
    if (v >= 0xe0 && v <= 0xf4) {
        return 2 + (v - 0xe0) / 4;
    }
    return -1;
}

/* 24 MHz / prescaler / 16; prescaler 13 is QEMU's 115200 (115384 on chip) */
static void scc_set_baudbase(NS87560SCCState *sc)
{
    static const uint32_t baud[4] = { 115200, 923077, 0, 1500000 };
    uint32_t b = baud[EXCR2_PRESL(sc->excr2)];

    if (!b) {
        qemu_log_mask(LOG_GUEST_ERROR, "ns87560: SCC%d reserved prescaler\n",
                      sc->index + 1);
        return;
    }
    sc->ss->baudbase = b;
}

/* A bank 1 divisor access in extended mode falls back to 16550 mode. */
static void scc_fallback(NS87560SCCState *sc)
{
    bool was_ext = sc->excr1 & EXCR1_EXT_SL;

    sc->excr2 &= ~0x30;
    scc_set_baudbase(sc);
    sc->excr1 &= ~(EXCR1_ETDLBK | EXCR1_BTEST | (was_ext ? EXCR1_EXT_SL : 0));
    if (sc->index == 1 && was_ext) {
        sc->ir[0] &= ~0x0c;                     /* IR_SL back to UART */
    }
    sc->ier_ext = 0;
    ns87560_periph_update(sc->lio);
}

/* (bank, offset) of the infrared registers, -1 when reserved */
static int scc_ir_index(int bank, int off)
{
    static const int8_t map[4][8] = {
        [0] = { -1, -1, 0, -1, -1, -1, -1, -1 },
        [1] = { -1, -1, -1, -1, 1, -1, -1, -1 },
        [2] = { 2, -1, 3, -1, -1, -1, -1, -1 },
        [3] = { 4, 5, 6, -1, 7, -1, 8, 9 },
    };
    return map[bank - 4][off];
}

static uint64_t ns87560_scc_read(void *opaque, hwaddr off, unsigned size)
{
    NS87560SCCState *sc = opaque;
    uint8_t lsr = sc->ss->lsr;
    int idx;

    if (off == 3) {
        return sc->bsr;
    }
    switch (sc->bank) {
    case 0:
        if (off == 2 && (sc->excr1 & EXCR1_EXT_SL)) {
            /* Extended mode EIR: event bits, not a priority code */
            return (lsr & 0x01 ? 1 : 0) | (lsr & 0x20 ? 2 : 0) |
                   (lsr & 0x1e ? 4 : 0) | (sc->ss->msr & 0x0f ? 8 : 0) |
                   (lsr & 0x40 ? 0x20 : 0);
        }
        if (off == 1) {
            return scc_core_read(sc, 1) | sc->ier_ext;
        }
        return scc_core_read(sc, off);
    case 1:
        if (off <= 1) {
            if (scc_locked(sc)) {
                return sc->scratch[off];
            }
            scc_fallback(sc);
        }
        return scc_core_read(sc, off);
    case 2:
        switch (off) {
        case 0:
            return sc->ss->divider & 0xff;
        case 1:
            return sc->ss->divider >> 8;
        case 2:
            return sc->excr1;
        case 4:
            return sc->excr2;
        case 6:
            return fifo8_num_used(&sc->ss->xmit_fifo) & 0x1f;
        case 7:
            return fifo8_num_used(&sc->ss->recv_fifo) & 0x1f;
        }
        return 0;
    case 3:
        switch (off) {
        case 0:
            return NS87560_SCC_DID;
        case 1:
            return sc->ss->lcr;
        case 2:
            return sc->ss->fcr;
        }
        return 0;
    default:
        if (sc->index == 0) {
            return 0;
        }
        idx = scc_ir_index(sc->bank, off);
        return idx < 0 ? 0 : idx == 7 ? sc->ir[7] & 0x70 : sc->ir[idx];
    }
}

static void ns87560_scc_write_bsr(NS87560SCCState *sc, uint8_t val)
{
    NS87560LIOState *s = sc->lio;
    int b;

    sc->bsr = val;
    if (sc->index == 1 && !(s->dev.config[CFG_SIOCFG] & SIOCFG_UIR_BANKS)) {
        b = val >> 7;                           /* plain 16550 LCR */
    } else {
        b = scc_bank_of(val);
        if (b < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "ns87560: SCC%d reserved bank "
                          "code 0x%02x\n", sc->index + 1, val);
            b = 1;
        }
    }
    sc->bank = b;
    if (b <= 1) {
        scc_core_write(sc, 3, val);
    }
    trace_ns87560_scc_bank(sc->index, b);
}

static void ns87560_scc_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    NS87560SCCState *sc = opaque;
    uint8_t old, lcr;
    int idx;

    if (off == 3) {
        ns87560_scc_write_bsr(sc, val);
        return;
    }
    switch (sc->bank) {
    case 0:
        if (off == 1) {
            sc->ier_ext = (sc->excr1 & EXCR1_EXT_SL) ? (val & 0xf0) : 0;
            if (sc->ier_ext) {
                qemu_log_mask(LOG_UNIMP, "ns87560: SCC extended-mode "
                              "interrupt sources 0x%02x\n", sc->ier_ext);
            }
            val &= 0x0f;
        }
        scc_core_write(sc, off, val);
        if (off == 4) {
            ns87560_periph_update(sc->lio);
        }
        break;
    case 1:
        if (off <= 1) {
            if (scc_locked(sc)) {
                sc->scratch[off] = val;
                return;
            }
            scc_fallback(sc);
        }
        scc_core_write(sc, off, val);
        break;
    case 2:
        switch (off) {
        case 0:
        case 1:
            /* Baud generator divisor: set it without a fallback */
            lcr = sc->ss->lcr;
            scc_core_write(sc, 3, lcr | 0x80);
            scc_core_write(sc, off, val);
            scc_core_write(sc, 3, lcr);
            break;
        case 2:
            old = sc->excr1;
            sc->excr1 = val & ~BIT(6);
            if ((val ^ old) & EXCR1_LOOP) {
                scc_core_write(sc, 4, (val & EXCR1_LOOP) ?
                               sc->ss->mcr | SCC_MCR_LOOP :
                               sc->ss->mcr & ~SCC_MCR_LOOP);
            }
            if (((val ^ old) & EXCR1_EXT_SL) && (sc->ss->fcr & 0x01)) {
                scc_core_write(sc, 2, sc->ss->fcr | 0x06);  /* flush FIFOs */
            }
            if (val & 0x0e) {
                qemu_log_mask(LOG_UNIMP, "ns87560: SCC DMA controls 0x%02"
                              PRIx64 "\n", val);
            }
            ns87560_periph_update(sc->lio);
            break;
        case 4:
            sc->excr2 = val & 0xb0;
            scc_set_baudbase(sc);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "ns87560: SCC%d bank 2 offset %d "
                          "is read-only\n", sc->index + 1, (int)off);
        }
        break;
    case 3:
        qemu_log_mask(LOG_GUEST_ERROR, "ns87560: SCC%d bank 3 is read-only\n",
                      sc->index + 1);
        break;
    default:
        idx = sc->index ? scc_ir_index(sc->bank, off) : -1;
        if (idx < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "ns87560: SCC%d bank %d offset %d "
                          "is reserved\n", sc->index + 1, sc->bank, (int)off);
            return;
        }
        sc->ir[idx] = val;
        if (idx == 0 && (val & 0x0c)) {
            qemu_log_mask(LOG_UNIMP, "ns87560: SCC2 infrared mode\n");
        }
    }
}

static const MemoryRegionOps ns87560_scc_ops = {
    .read = ns87560_scc_read,
    .write = ns87560_scc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void ns87560_scc_reset(NS87560SCCState *sc)
{
    sc->bsr = 0;
    sc->bank = 0;
    sc->excr1 = 0;
    sc->excr2 = 0;
    sc->ier_ext = 0;
    memset(sc->scratch, 0, sizeof(sc->scratch));
    memset(sc->ir, 0, sizeof(sc->ir));
    sc->ir[1] = 0x02;
    sc->ir[4] = 0x29;
    sc->ir[5] = 0x69;
    scc_set_baudbase(sc);
    /* The chip's MCR resets to 0; QEMU's core reset sets OUT2. */
    scc_core_write(sc, 4, 0);
}

/*
 * Port 61h (p.392): bit 0 gates PIT counter 2, bit 1 is speaker data,
 * bits 2/3 disable the NMI sources; bit 4 toggles on each read
 * (refresh), bit 5 is counter 2's output.  No NMI sources: bits 6/7 read 0.
 */
static uint64_t ns87560_port61_read(void *opaque, hwaddr addr, unsigned size)
{
    NS87560LIOState *s = opaque;
    PITChannelInfo ch;

    pit_get_channel_info(s->pit, 2, &ch);
    s->port61 ^= BIT(4);
    return (s->port61 & 0x1e) | (ch.gate & 1) | (ch.out << 5);
}

static void ns87560_port61_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    NS87560LIOState *s = opaque;

    s->port61 = (s->port61 & BIT(4)) | (val & 0x0f);
    pit_set_gate(s->pit, 2, val & 1);
}

static const MemoryRegionOps ns87560_port61_ops = {
    .read = ns87560_port61_read,
    .write = ns87560_port61_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/*
 * Port 92h (p.389-390): bit 0 is the fast CPU init (no effect on
 * PA-RISC), bit 1 A20 gate; bits 3 and 5 can only be set until reset.
 */
static uint64_t ns87560_port92_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((NS87560LIOState *)opaque)->port92;
}

static void ns87560_port92_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    NS87560LIOState *s = opaque;

    s->port92 = (val & 0x2b) | (s->port92 & 0x28);
}

static const MemoryRegionOps ns87560_port92_ops = {
    .read = ns87560_port92_read,
    .write = ns87560_port92_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/* APM control/status B2h-B3h (p.394-395): storage, there is no SMI. */
static uint64_t ns87560_apm_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((NS87560LIOState *)opaque)->apm[addr];
}

static void ns87560_apm_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    ((NS87560LIOState *)opaque)->apm[addr] = val;
}

static const MemoryRegionOps ns87560_apm_ops = {
    .read = ns87560_apm_read,
    .write = ns87560_apm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/*
 * 8237 high page registers 480h-48Fh, at the same offsets as the low
 * pages 80h-8Fh.  Unused offsets read 0.  Each half follows its 8237's
 * enable in 5Ah.
 */
static const int8_t hipage_chan[16] = {
    -1, 2, 3, 1, -1, -1, -1, 0,
    -1, 6, 7, 5, -1, -1, -1, -1
};

static bool ns87560_hipage_decoded(NS87560LIOState *s, hwaddr addr)
{
    return pci_get_word(s->dev.config + CFG_FER) &
           (addr < 8 ? FER_DMA1 : FER_DMA2);
}

static uint64_t ns87560_hipage_read(void *opaque, hwaddr addr, unsigned size)
{
    NS87560LIOState *s = opaque;
    int ch = hipage_chan[addr];

    if (!ns87560_hipage_decoded(s, addr)) {
        return 0xff;
    }
    return ch < 0 ? 0 : s->dma[ch >> 2]->regs[ch & 3].pageh;
}

static void ns87560_hipage_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    NS87560LIOState *s = opaque;
    int ch = hipage_chan[addr];

    if (ns87560_hipage_decoded(s, addr) && ch >= 0) {
        s->dma[ch >> 2]->regs[ch & 3].pageh = val;
    }
}

static const MemoryRegionOps ns87560_hipage_ops = {
    .read = ns87560_hipage_read,
    .write = ns87560_hipage_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/*
 * FDC configuration mode (p.95-96): with 5Dh bit 0 set, writing 55h to
 * FDCBAR+0 enters it, AAh leaves it.  Inside, +0 is the index and +1 the
 * data of ten registers.  Outside, +0 is SRA, which reads FFh in PC-AT
 * mode; +1 is the FDC core's SRB.
 */
static const uint8_t fdccfg_wmask[10] = {
    [1] = 0x45, [2] = 0x01, [3] = 0x33, [7] = 0xff, [8] = 0xff, [9] = 0xff
};
static const uint8_t fdccfg_reset[10] = {
    [0] = NS87560_FDC_DID, [7] = 0x05, [8] = 0x05, [9] = 0x05
};

static void ns87560_fdccfg_set_mode(NS87560LIOState *s, bool on)
{
    s->fdccfg_mode = on;
    memory_region_set_enabled(&s->fdccfg_data_io,
                              on && (pci_get_word(s->dev.config + CFG_FER) &
                                     FER_FDC));
    trace_ns87560_fdc_cfg_mode(on);
}

static uint64_t ns87560_fdccfg_idx_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    NS87560LIOState *s = opaque;

    if (s->fdccfg[1] & 0x40) {
        qemu_log_mask(LOG_UNIMP, "ns87560: FDC PS/2 mode status A\n");
    }
    return 0xff;
}

static void ns87560_fdccfg_idx_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size)
{
    NS87560LIOState *s = opaque;

    if (!s->fdccfg_mode) {
        if (val == 0x55 && (s->dev.config[CFG_SFE] & SFE_FDC_CFG)) {
            s->fdccfg_index = 0;
            ns87560_fdccfg_set_mode(s, true);
        }
    } else if (val == 0xaa) {
        ns87560_fdccfg_set_mode(s, false);
    } else {
        s->fdccfg_index = val;
    }
}

static uint64_t ns87560_fdccfg_data_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    NS87560LIOState *s = opaque;

    return s->fdccfg_index < ARRAY_SIZE(s->fdccfg) ?
           s->fdccfg[s->fdccfg_index] : 0;
}

static void ns87560_fdccfg_data_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    NS87560LIOState *s = opaque;
    int i = s->fdccfg_index;

    if (i >= ARRAY_SIZE(s->fdccfg) || !fdccfg_wmask[i]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ns87560: FDC config register 0x%x is read-only\n", i);
        return;
    }
    s->fdccfg[i] = (s->fdccfg[i] & ~fdccfg_wmask[i]) | (val & fdccfg_wmask[i]);
    if (i == 1 && (val & 0x40)) {
        qemu_log_mask(LOG_UNIMP, "ns87560: FDC PS/2 mode\n");
    }
    trace_ns87560_fdc_cfg(i, val);
}

static const MemoryRegionOps ns87560_fdccfg_idx_ops = {
    .read = ns87560_fdccfg_idx_read,
    .write = ns87560_fdccfg_idx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static const MemoryRegionOps ns87560_fdccfg_data_ops = {
    .read = ns87560_fdccfg_data_read,
    .write = ns87560_fdccfg_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void ns87560_fdccfg_reset(NS87560LIOState *s)
{
    memcpy(s->fdccfg, fdccfg_reset, sizeof(s->fdccfg));
    s->fdccfg_index = 0;
    ns87560_fdccfg_set_mode(s, false);
}

/* 63h bits 6:4: 1xx routes the FDC to channel xx, 0xx leaves it unrouted */
static int ns87560_fdc_dma_channel(uint8_t dmart1)
{
    return (dmart1 & 0x40) ? (dmart1 >> 4) & 3 : -1;
}

bool ns87560_lio_ide_channel_enabled(NS87560LIOState *s, int channel)
{
    return pci_get_word(s->dev.config + CFG_FER) &
           (channel ? FER_IDE2 : FER_IDE1);
}

void ns87560_lio_set_decode_notifier(NS87560LIOState *s,
                                     NS87560DecodeNotify fn, PCIDevice *ide)
{
    s->ide_notify = fn;
    s->ide = ide;
}

static uint64_t ns87560_acpi_read(void *opaque, hwaddr addr, unsigned size)
{
    NS87560LIOState *s = opaque;

    trace_ns87560_acpi_read(addr, s->acpi_regs[addr]);
    return s->acpi_regs[addr];
}

static void ns87560_acpi_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    NS87560LIOState *s = opaque;
    uint8_t *r = &s->acpi_regs[addr];

    *r = (*r & ~acpi_wmask[addr]) | (val & acpi_wmask[addr]);
    *r &= ~(val & acpi_w1c[addr]);
    if ((addr == 0x05 && (val & 0x20)) || (addr == 0x12 && val)) {
        qemu_log_mask(LOG_UNIMP, "ns87560: ACPI register 0x%02" HWADDR_PRIx
                      " = 0x%02" PRIx64 " has no effect\n", addr, val);
    }
    trace_ns87560_acpi_write(addr, val);
}

static const MemoryRegionOps ns87560_acpi_ops = {
    .read = ns87560_acpi_read,
    .write = ns87560_acpi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void ns87560_lio_config_write(PCIDevice *d, uint32_t addr,
                                     uint32_t val, int len)
{
    NS87560LIOState *s = NS87560_LIO(d);

    trace_ns87560_lio_config_write(addr, val, len);
    pci_default_write_config(d, addr, val, len);

    if (ranges_overlap(addr, len, CFG_ARB, 2)) {
        /* Two equal priority fields select rotate mode (p.277). */
        uint16_t v = pci_get_word(d->config + CFG_ARB) & 0x7fff;
        int i, j;

        for (i = 0; i < 5; i++) {
            for (j = i + 1; j < 5; j++) {
                if (((v >> (3 * i)) & 7) == ((v >> (3 * j)) & 7)) {
                    pci_set_word(d->config + CFG_ARB, NS87560_ARB_ROTATE);
                    i = j = 5;
                }
            }
        }
    }
    if (ranges_overlap(addr, len, CFG_FER, 2) ||
        ranges_overlap(addr, len, 0x84, 0x1c)) {
        ns87560_lio_decode_update(s);
    }
    if (range_covers_byte(addr, len, CFG_SFE) &&
        !(d->config[CFG_SFE] & SFE_FDC_CFG) && s->fdccfg_mode) {
        ns87560_fdccfg_set_mode(s, false);
    }
    /* isa-fdc binds its DMA channel at realize; it cannot move later. */
    if (range_covers_byte(addr, len, CFG_DMART1) &&
        ns87560_fdc_dma_channel(d->config[CFG_DMART1]) != s->fdc_dma) {
        qemu_log_mask(LOG_UNIMP, "ns87560: FDC DMA routing change (63h = "
                      "0x%02x) has no effect\n", d->config[CFG_DMART1]);
    }
    if (ranges_overlap(addr, len, CFG_ELCR, 2)) {
        ns87560_lio_update_elcr(s);
    }
    if (ranges_overlap(addr, len, CFG_IRCR1, CFG_IRCR9 - CFG_IRCR1 + 1)) {
        ns87560_router_recompute(s, false);
    }
    ns87560_lio_check_unimp(s, addr, len);
}

/* ---- Function 1: legacy I/O bridge ------------------------------------ */

static void ns87560_lio_instance_init(Object *obj)
{
    NS87560LIOState *s = NS87560_LIO(obj);

    qdev_init_gpio_in_named(DEVICE(obj), ns87560_router_set_irq,
                            NS87560_LIO_GPIO_ROUTER, NS87560_SRC_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cascade_out,
                             NS87560_LIO_GPIO_INTR, 1);
    qemu_init_irq(&s->i8259_irq, ns87560_lio_i8259_irq, s, 0);
    qdev_init_gpio_in_named(DEVICE(obj), ns87560_periph_irq, "periph-irq", 4);

    /* Created here so that the container can alias their chardevs. */
    s->serial[0] = ISA_DEVICE(object_new(TYPE_ISA_SERIAL));
    object_property_add_child(obj, "serial[0]", OBJECT(s->serial[0]));
    object_unref(OBJECT(s->serial[0]));
    s->serial[1] = ISA_DEVICE(object_new(TYPE_ISA_SERIAL));
    object_property_add_child(obj, "serial[1]", OBJECT(s->serial[1]));
    object_unref(OBJECT(s->serial[1]));
    s->parallel = ISA_DEVICE(object_new(TYPE_ISA_PARALLEL));
    object_property_add_child(obj, "parallel", OBJECT(s->parallel));
    object_unref(OBJECT(s->parallel));
}

static void ns87560_lio_realize(PCIDevice *d, Error **errp)
{
    NS87560LIOState *s = NS87560_LIO(d);
    MemoryRegion *io = pci_address_space_io(d);
    int i;

    ns87560_lio_init_masks(s);
    ns87560_lio_config_defaults(s);

    /* QEMU supports one ISA bus per machine; this fails on the B160L. */
    s->isa_bus = isa_bus_new(DEVICE(d), pci_address_space(d), io, errp);
    if (!s->isa_bus) {
        return;
    }

    s->isa_irqs = i8259_init(s->isa_bus, &s->i8259_irq);
    s->pic[0] = ISA_DEVICE(object_resolve_path_component(OBJECT(isa_pic),
                                                         "primary"));
    s->pic[1] = ISA_DEVICE(object_resolve_path_component(OBJECT(isa_pic),
                                                         "secondary"));
    for (i = 0; i < 2; i++) {
        I8259CommonState *p = I8259_COMMON(s->pic[i]);

        memory_region_set_enabled(&p->elcr_io, false);
        /* QEMU's F8h would keep IRQ1 edge-triggered; Linux needs level. */
        p->elcr_mask = i ? NS87560_ELCR_MASK_SLAVE : NS87560_ELCR_MASK_MASTER;
        memory_region_init_io(&s->pic_cmd_io[i], OBJECT(d), &ns87560_pic_cmd_ops,
                              p, i ? "ns87560-pic2-cmd" : "ns87560-pic1-cmd", 1);
        memory_region_add_subregion_overlap(io, i ? 0xa0 : 0x20,
                                            &s->pic_cmd_io[i], 1);
    }

    /*
     * Fixed 8259 inputs go straight to the PICs.  The peripherals' "irq"
     * properties are labels (their reset routes) that select the router.
     */
    s->bus_irqs[0] = s->isa_irqs[0];
    s->bus_irqs[8] = s->isa_irqs[8];
    s->bus_irqs[13] = s->isa_irqs[13];
    s->bus_irqs[4] = qdev_get_gpio_in_named(DEVICE(d), "periph-irq", 0);
    s->bus_irqs[3] = qdev_get_gpio_in_named(DEVICE(d), "periph-irq", 1);
    s->bus_irqs[5] = qdev_get_gpio_in_named(DEVICE(d), "periph-irq", 2);
    s->bus_irqs[6] = qdev_get_gpio_in_named(DEVICE(d), "periph-irq", 3);
    isa_bus_register_input_irqs(s->isa_bus, s->bus_irqs);

    for (i = 0; i < 2; i++) {
        qdev_prop_set_uint32(DEVICE(s->serial[i]), "iobase",
                             lio_win_base(s, i ? CFG_SP2BAR : CFG_SP1BAR));
        qdev_prop_set_uint32(DEVICE(s->serial[i]), "irq", i ? 3 : 4);
        if (!qdev_realize(DEVICE(s->serial[i]), BUS(s->isa_bus), errp)) {
            return;
        }
        /* The bank wrapper decodes SPnBAR and forwards to the 16550 core */
        isa_serial_set_enabled(s->serial[i], false);
        s->scc[i].ss = SERIAL(object_resolve_path_component(
                                  OBJECT(s->serial[i]), "serial"));
        s->scc[i].lio = s;
        s->scc[i].index = i;
        memory_region_init_io(&s->scc[i].io, OBJECT(d), &ns87560_scc_ops,
                              &s->scc[i], i ? "ns87560-scc2" : "ns87560-scc1",
                              8);
        memory_region_set_enabled(&s->scc[i].io, false);
        memory_region_add_subregion(io, 0, &s->scc[i].io);
        ns87560_scc_reset(&s->scc[i]);
    }
    /* isa-parallel needs a backend; without one the port is absent. */
    if (!qemu_chr_fe_backend_connected(&ISA_PARALLEL(s->parallel)->state.chr)) {
        object_unparent(OBJECT(s->parallel));
        s->parallel = NULL;
    } else {
        qdev_prop_set_uint32(DEVICE(s->parallel), "iobase",
                             lio_win_base(s, CFG_PPBAR));
        qdev_prop_set_uint32(DEVICE(s->parallel), "irq", 5);
        if (!qdev_realize(DEVICE(s->parallel), BUS(s->isa_bus), errp)) {
            return;
        }
    }

    /* 8254; its output 0 goes straight to 8259 input 0 (not routable). */
    s->pit = PIT_COMMON(i8254_pit_init(s->isa_bus, 0x40, 0, NULL));
    memory_region_init_io(&s->port61_io, OBJECT(d), &ns87560_port61_ops, s,
                          "ns87560-port61", 1);
    memory_region_add_subregion(io, 0x61, &s->port61_io);
    memory_region_init_io(&s->port92_io, OBJECT(d), &ns87560_port92_ops, s,
                          "ns87560-port92", 1);
    memory_region_add_subregion(io, 0x92, &s->port92_io);
    memory_region_init_io(&s->apm_io, OBJECT(d), &ns87560_apm_ops, s,
                          "ns87560-apm", 2);
    memory_region_add_subregion(io, 0xb2, &s->apm_io);

    /* 8237 pair; fn1 owns the high page registers (480h-48Fh). */
    for (i = 0; i < 2; i++) {
        ISADevice *dma = isa_new(TYPE_I8257);

        object_property_add_child(OBJECT(d), "dma[*]", OBJECT(dma));
        qdev_prop_set_int32(DEVICE(dma), "base", i ? 0xc0 : 0x00);
        qdev_prop_set_int32(DEVICE(dma), "page-base", i ? 0x88 : 0x80);
        qdev_prop_set_int32(DEVICE(dma), "pageh-base", -1);
        qdev_prop_set_int32(DEVICE(dma), "dshift", i);
        qdev_prop_set_bit(DEVICE(dma), "pageh-autoclear", true);
        if (!isa_realize_and_unref(dma, s->isa_bus, errp)) {
            return;
        }
        s->dma[i] = I8257(dma);
    }
    isa_bus_dma(s->isa_bus, ISADMA(s->dma[0]), ISADMA(s->dma[1]));
    memory_region_init_io(&s->dma_hipage_io, OBJECT(d), &ns87560_hipage_ops,
                          s, "ns87560-dma-hipage", 16);
    memory_region_add_subregion(io, 0x480, &s->dma_hipage_io);

    /* FDC: the DMA channel comes from the reset value of 63h (2). */
    s->fdc_dma = ns87560_fdc_dma_channel(s->dev.config[CFG_DMART1]);
    s->fdc = isa_new(TYPE_ISA_FDC);
    object_property_add_child(OBJECT(d), "fdc", OBJECT(s->fdc));
    qdev_prop_set_uint32(DEVICE(s->fdc), "iobase", lio_win_base(s, CFG_FDCBAR));
    qdev_prop_set_uint32(DEVICE(s->fdc), "irq", 6);
    qdev_prop_set_uint32(DEVICE(s->fdc), "dma", s->fdc_dma);
    qdev_prop_set_uint8(DEVICE(s->fdc), "part-id", NS87560_FDC_PARTID);
    qdev_prop_set_bit(DEVICE(s->fdc), "mode-cmd", true);
    if (!isa_realize_and_unref(s->fdc, s->isa_bus, errp)) {
        return;
    }
    memory_region_init_io(&s->fdccfg_idx_io, OBJECT(d), &ns87560_fdccfg_idx_ops,
                          s, "ns87560-fdc-cfg-idx", 1);
    memory_region_set_enabled(&s->fdccfg_idx_io, false);
    memory_region_add_subregion(io, 0, &s->fdccfg_idx_io);
    memory_region_init_io(&s->fdccfg_data_io, OBJECT(d),
                          &ns87560_fdccfg_data_ops, s, "ns87560-fdc-cfg-data", 1);
    memory_region_set_enabled(&s->fdccfg_data_io, false);
    memory_region_add_subregion_overlap(io, 0, &s->fdccfg_data_io, 1);
    ns87560_fdccfg_reset(s);

    memory_region_init_io(&s->acpi_io, OBJECT(d), &ns87560_acpi_ops, s,
                          "ns87560-acpi", sizeof(s->acpi_regs));
    memory_region_set_enabled(&s->acpi_io, false);
    memory_region_add_subregion(io, 0, &s->acpi_io);

    ns87560_lio_update_elcr(s);
    ns87560_lio_decode_update(s);
}

static void ns87560_lio_reset_hold(Object *obj, ResetType type)
{
    NS87560LIOState *s = NS87560_LIO(obj);

    ns87560_lio_config_defaults(s);
    memset(s->acpi_regs, 0, sizeof(s->acpi_regs));
    ns87560_fdccfg_reset(s);
    ns87560_scc_reset(&s->scc[0]);
    ns87560_scc_reset(&s->scc[1]);
    ns87560_periph_update(s);
    s->port61 = 0;
    s->port92 = 0;
    memset(s->apm, 0, sizeof(s->apm));
    ns87560_lio_update_elcr(s);
    ns87560_lio_decode_update(s);
}

static int ns87560_lio_post_load(void *opaque, int version_id)
{
    NS87560LIOState *s = opaque;
    int src, irq;

    /*
     * The PICs migrate their own state; only rebuild the bookkeeping, do
     * not drive their inputs again.
     */
    s->pic_level = 0;
    for (src = 0; src < NS87560_SRC_COUNT; src++) {
        irq = ns87560_route_of(s, src);
        if ((s->src_level & BIT(src)) && irq) {
            s->pic_level |= BIT(irq);
        }
    }
    I8259_COMMON(s->pic[0])->elcr = s->dev.config[CFG_ELCR] &
                                  I8259_COMMON(s->pic[0])->elcr_mask;
    I8259_COMMON(s->pic[1])->elcr = s->dev.config[CFG_ELCR + 1] &
                                  I8259_COMMON(s->pic[1])->elcr_mask;
    scc_set_baudbase(&s->scc[0]);
    scc_set_baudbase(&s->scc[1]);
    ns87560_lio_decode_update(s);
    return 0;
}

/*
 * The PCI bus reset clears the writable Command bits after the device's
 * hold phase; restore the documented reset value afterwards.
 */
static void ns87560_lio_reset_exit(Object *obj, ResetType type)
{
    NS87560LIOState *s = NS87560_LIO(obj);

    pci_set_word(s->dev.config + PCI_COMMAND, NS87560_LIO_CMD_RESET);
    memory_region_set_enabled(&s->dev.bus_master_enable_region, true);
}

static const VMStateDescription vmstate_ns87560_scc = {
    .name = "ns87560-scc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(bsr, NS87560SCCState),
        VMSTATE_UINT8(bank, NS87560SCCState),
        VMSTATE_UINT8(excr1, NS87560SCCState),
        VMSTATE_UINT8(excr2, NS87560SCCState),
        VMSTATE_UINT8(ier_ext, NS87560SCCState),
        VMSTATE_UINT8_ARRAY(scratch, NS87560SCCState, 2),
        VMSTATE_UINT8_ARRAY(ir, NS87560SCCState, 10),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ns87560_lio = {
    .name = "ns87560-lio",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ns87560_lio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, NS87560LIOState),
        VMSTATE_UINT32(src_level, NS87560LIOState),
        VMSTATE_UINT8_ARRAY(acpi_regs, NS87560LIOState, 0x20),
        VMSTATE_UINT8_ARRAY(fdccfg, NS87560LIOState, 10),
        VMSTATE_UINT8(fdccfg_index, NS87560LIOState),
        VMSTATE_BOOL(fdccfg_mode, NS87560LIOState),
        VMSTATE_UINT8(port61, NS87560LIOState),
        VMSTATE_UINT8(port92, NS87560LIOState),
        VMSTATE_UINT8_ARRAY(apm, NS87560LIOState, 2),
        VMSTATE_UINT8(periph_level, NS87560LIOState),
        VMSTATE_STRUCT_ARRAY(scc, NS87560LIOState, 2, 1, vmstate_ns87560_scc,
                             NS87560SCCState),
        VMSTATE_END_OF_LIST()
    }
};

static void ns87560_lio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize = ns87560_lio_realize;
    k->config_write = ns87560_lio_config_write;
    k->vendor_id = PCI_VENDOR_ID_NS;
    k->device_id = PCI_DEVICE_ID_NS_87560_LIO;
    k->revision = NS87560_LIO_REVISION;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
    rc->phases.hold = ns87560_lio_reset_hold;
    rc->phases.exit = ns87560_lio_reset_exit;
    dc->vmsd = &vmstate_ns87560_lio;
    dc->desc = "NS87560 Legacy I/O (PCI function 1)";
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    /* Part of the ns87560 container, which wires it */
    dc->user_creatable = false;
}

/* ---- Container ---------------------------------------------------------- */

static void ns87560_usb_irq(void *opaque, int n, int level)
{
    NS87560State *s = opaque;

    /* Datasheet path (IRCR8) and the C3700 board wire USB_INT -> INTD# */
    qemu_set_irq(s->usb_split[0], level);
    qemu_set_irq(s->usb_split[1], level);
}

static void ns87560_instance_init(Object *obj)
{
    NS87560State *s = NS87560(obj);

    s->lio = NS87560_LIO(object_new(TYPE_NS87560_LIO));
    object_property_add_child(obj, "lio", OBJECT(s->lio));
    object_unref(OBJECT(s->lio));
    object_property_add_alias(obj, "chardev-sp1", OBJECT(s->lio->serial[0]),
                              "chardev");
    object_property_add_alias(obj, "chardev-sp2", OBJECT(s->lio->serial[1]),
                              "chardev");
    object_property_add_alias(obj, "chardev-pp", OBJECT(s->lio->parallel),
                              "chardev");
    qdev_init_gpio_in_named(DEVICE(obj), ns87560_usb_irq, "usb-irq", 1);
}

static void ns87560_realize(DeviceState *dev, Error **errp)
{
    NS87560State *s = NS87560(dev);
    DeviceState *lio = DEVICE(s->lio);
    int n;

    if (!s->bus) {
        error_setg(errp, "ns87560: 'bus' link property is not set");
        return;
    }
    if (s->slot < 0 || s->slot >= PCI_SLOT_MAX) {
        error_setg(errp, "ns87560: invalid slot %d", s->slot);
        return;
    }

    /* Function 1 first: it owns the interrupt router the others feed. */
    qdev_prop_set_int32(lio, "addr", PCI_DEVFN(s->slot, NS87560_FN_LIO));
    qdev_prop_set_bit(lio, "multifunction", true);
    if (!qdev_realize(lio, BUS(s->bus), errp)) {
        return;
    }
    if (!s->lio->parallel) {
        /* The alias must not outlive the removed port. */
        object_property_del(OBJECT(dev), "chardev-pp");
    }

    s->ide = pci_new_multifunction(PCI_DEVFN(s->slot, NS87560_FN_IDE),
                                   TYPE_NS87415_IDE);
    object_property_add_child(OBJECT(dev), "ide", OBJECT(s->ide));
    object_property_set_link(OBJECT(s->ide), "lio", OBJECT(s->lio),
                             &error_abort);
    if (!pci_realize_and_unref(s->ide, s->bus, errp)) {
        return;
    }
    for (n = 0; n < 2; n++) {
        qdev_connect_gpio_out_named(DEVICE(s->ide), NS87415_GPIO_IRQ, n,
            qdev_get_gpio_in_named(lio, NS87560_LIO_GPIO_ROUTER,
                                   NS87560_SRC_IDE1 + n));
    }

    s->usb = pci_new_multifunction(PCI_DEVFN(s->slot, NS87560_FN_USB),
                                   TYPE_NS87560_OHCI);
    object_property_add_child(OBJECT(dev), "usb", OBJECT(s->usb));
    if (!pci_realize_and_unref(s->usb, s->bus, errp)) {
        return;
    }
    s->usb_split[0] = qdev_get_gpio_in_named(lio, NS87560_LIO_GPIO_ROUTER,
                                             NS87560_SRC_USB);
    s->usb_split[1] = qdev_get_gpio_in_named(lio, NS87560_LIO_GPIO_ROUTER,
                                             NS87560_SRC_INTD);
    qdev_connect_gpio_out_named(DEVICE(s->usb), NS87560_OHCI_GPIO_IRQ, 0,
                                qdev_get_gpio_in_named(dev, "usb-irq", 0));

    /* The 8259 master output reaches the host through function 2's INTD. */
    qdev_connect_gpio_out_named(lio, NS87560_LIO_GPIO_INTR, 0,
        qdev_get_gpio_in_named(DEVICE(s->usb), NS87560_OHCI_GPIO_CASCADE, 0));
}

static const Property ns87560_properties[] = {
    DEFINE_PROP_LINK("bus", NS87560State, bus, TYPE_PCI_BUS, PCIBus *),
    DEFINE_PROP_INT32("slot", NS87560State, slot, 0x0e),
};

static void ns87560_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ns87560_realize;
    device_class_set_props(dc, ns87560_properties);
    dc->desc = "National Semiconductor PC87560 PCI System I/O";
    /* Needs machine wiring: PCI bus link, slot and firmware routing */
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo ns87560_types[] = {
    {
        .name = TYPE_NS87560,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(NS87560State),
        .instance_init = ns87560_instance_init,
        .class_init = ns87560_class_init,
    }, {
        .name = TYPE_NS87560_LIO,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(NS87560LIOState),
        .instance_init = ns87560_lio_instance_init,
        .class_init = ns87560_lio_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(ns87560_types)

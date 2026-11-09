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
#include "hw/intc/i8259.h"
#include "hw/isa/i8259_internal.h"
#include "hw/isa/isa.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/range.h"
#include "trace.h"

#define NS87560_LIO_REVISION    0x01
#define NS87560_LIO_CMD_RESET   0x000f  /* I/O, memory, master, special cycles */
#define NS87560_LIO_CMD_WMASK   0x014c  /* bits 2, 3, 6 and 8 are writable */
#define NS87560_ACPIBAR_RESET   0x00000801  /* see ns87560_lio_regs[] */

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

    /* Fixed 8259 inputs; routed peripherals are connected later. */
    s->bus_irqs[0] = s->isa_irqs[0];
    s->bus_irqs[8] = s->isa_irqs[8];
    s->bus_irqs[13] = s->isa_irqs[13];
    isa_bus_register_input_irqs(s->isa_bus, s->bus_irqs);

    ns87560_lio_update_elcr(s);
}

static void ns87560_lio_reset_hold(Object *obj, ResetType type)
{
    NS87560LIOState *s = NS87560_LIO(obj);

    ns87560_lio_config_defaults(s);
    ns87560_lio_update_elcr(s);
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

static const VMStateDescription vmstate_ns87560_lio = {
    .name = "ns87560-lio",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ns87560_lio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, NS87560LIOState),
        VMSTATE_UINT32(src_level, NS87560LIOState),
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

    s->ide = pci_new_multifunction(PCI_DEVFN(s->slot, NS87560_FN_IDE),
                                   TYPE_NS87415_IDE);
    object_property_add_child(OBJECT(dev), "ide", OBJECT(s->ide));
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

/*
 * National Semiconductor PC87415-compatible IDE controller,
 * function 0 of the PC87560 (NS87560) PCI System I/O
 *
 * Two-channel bus-master ATA controller.  Each channel decodes either its
 * legacy ports or BAR0-1 / BAR2-3 (prog-if bits 0/2), gated by Command.IO
 * and by the channel enables in function 1 register 5Ah.  BAR4 is an
 * SFF-8038i bus-master block with two differences: writing 1 to command
 * bits 1/2 clears status bits 1/2 (PC87415 erratum, used by Linux), and
 * the hardware clears Start at the end of a transfer.  Channel interrupts
 * never use a PCI pin: they go through the 41h masks to the chip's
 * interrupt router.
 *
 * References: National Semiconductor, "PC87415 PCI-IDE DMA Master Mode
 * Interface Controller", March 1996; "PC87560UBD PCI System I/O",
 * October 1998, chapter 5.
 *
 * Copyright (c) 2026 Soumyajyotii Ssarkar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/range.h"
#include "qapi/error.h"
#include "hw/ide/pci.h"
#include "hw/core/irq.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "ide-internal.h"
#include "trace.h"

#define NS87415_REVISION        0x03
#define NS87415_PROGIF          0x8a    /* both channels legacy, switchable */
#define NS87415_PROGIF_NATIVE(ch) ((ch) ? 0x04 : 0x01)
#define NS87415_COMMAND_RESET   0x0005  /* I/O + bus master */
#define NS87415_COMMAND_WMASK   0x0145

#define NS87415_CTRL1           0x40
#define   CTRL1_DRST            0x02    /* drive reset, both channels */
#define NS87415_CTRL2           0x41
#define   CTRL2_INT_MASK(ch)    (1 << (ch))
#define   CTRL2_BAR23_DIS       0x04
#define NS87415_CTRL3           0x42
#define NS87415_CMDTIM          0x54
#define NS87415_SECSIZE         0x55
#define NS87415_SHADOW(ch, drv) (0x58 + (ch) * 4 + (drv))

static const uint8_t ns87415_timing_regs[] = {
    0x44, 0x45, 0x48, 0x49, 0x4c, 0x4d, 0x50, 0x51
};

/* PC87415 erratum: command bits 1/2 clear the same status bits */
#define BM_CMD_ERRATUM          (BM_STATUS_ERROR | BM_STATUS_INT)

OBJECT_DECLARE_SIMPLE_TYPE(NS87415State, NS87415_IDE)

struct NS87415State {
    PCIIDEState parent_obj;

    NS87560LIOState *lio;
    IDEDMAOps dma_ops;          /* bmdma ops plus the Start auto-clear */
    uint8_t irq_level;          /* bit ch: channel level before the mask */
};

static void ns87415_update_decode(NS87415State *s)
{
    PCIDevice *pd = PCI_DEVICE(s);
    PCIIDEState *d = PCI_IDE(s);
    bool io_on = pci_get_word(pd->config + PCI_COMMAND) & PCI_COMMAND_IO;
    int ch;

    memory_region_transaction_begin();
    for (ch = 0; ch < 2; ch++) {
        bool chan_on = ns87560_lio_ide_channel_enabled(s->lio, ch);
        bool native = pd->config[PCI_CLASS_PROG] & NS87415_PROGIF_NATIVE(ch);
        bool legacy_on = !native && io_on && chan_on;
        bool native_on = native && chan_on &&
            !(ch && (pd->config[NS87415_CTRL2] & CTRL2_BAR23_DIS));

        portio_list_set_enabled(&d->bus[ch].portio_list, legacy_on);
        portio_list_set_enabled(&d->bus[ch].portio2_list, legacy_on);
        memory_region_set_enabled(&d->data_bar[ch], native_on);
        memory_region_set_enabled(&d->cmd_bar[ch], native_on);
        trace_ns87415_decode(ch, native, chan_on, io_on, legacy_on, native_on);
    }
    memory_region_transaction_commit();
}

static void ns87415_decode_notify(PCIDevice *ide)
{
    ns87415_update_decode(NS87415_IDE(ide));
}

static void ns87415_update_irq(NS87415State *s, int ch)
{
    bool level = s->irq_level & BIT(ch);
    bool masked = PCI_DEVICE(s)->config[NS87415_CTRL2] & CTRL2_INT_MASK(ch);

    trace_ns87415_irq(ch, level, masked);
    qemu_set_irq(PCI_IDE(s)->isa_irq[ch], level && !masked);
}

/*
 * Hardware clears Start at the end of a transfer, but only once the
 * engine is idle: clearing it while DMAING is still set would turn the
 * guest's later stop write into a no-op and leave DMAING stuck.
 */
static void ns87415_bm_autoclear(BMDMAState *bm)
{
    if ((bm->cmd & BM_CMD_START) && !(bm->status & BM_STATUS_DMAING)) {
        bm->cmd &= ~BM_CMD_START;
    }
}

static void ns87415_dma_cmd_done(const IDEDMA *dma)
{
    ns87415_bm_autoclear(container_of(dma, BMDMAState, dma));
}

static void ns87415_set_irq(void *opaque, int ch, int level)
{
    NS87415State *s = opaque;

    s->irq_level = deposit32(s->irq_level, ch, 1, !!level);
    if (level) {
        ns87415_bm_autoclear(&PCI_IDE(s)->bmdma[ch]);
    }
    ns87415_update_irq(s, ch);
}

/* BAR4 command/status bytes of one channel (the PRD address is separate) */
static uint64_t ns87415_bmdma_read(void *opaque, hwaddr addr, unsigned size)
{
    BMDMAState *bm = opaque;

    switch (addr) {
    case 0:
        return bm->cmd;
    case 2:
        return bm->status;
    default:
        return 0;
    }
}

static void ns87415_bmdma_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    BMDMAState *bm = opaque;

    trace_ns87415_bmdma_write(bm == &bm->pci_dev->bmdma[1], addr, val);
    switch (addr) {
    case 0:
        /* Clear first, so "clear + start" in one write leaves fresh status */
        bm->status &= ~(val & BM_CMD_ERRATUM);
        bmdma_cmd_writeb(bm, val);
        break;
    case 2:
        bmdma_status_writeb(bm, val);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ns87415: write 0x%02" PRIx64
                      " to reserved bus master offset %" HWADDR_PRId "\n",
                      val, addr);
    }
}

static const MemoryRegionOps ns87415_bmdma_ops = {
    .read = ns87415_bmdma_read,
    .write = ns87415_bmdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* Device Control writes are shadowed per drive in 58h/59h/5Ch/5Dh. */
static void ns87415_ctrl_write(IDEBus *bus, uint32_t addr, uint32_t val)
{
    PCIDevice *pd = PCI_DEVICE(BUS(bus)->parent);

    pd->config[NS87415_SHADOW(bus->bus_id, bus->unit)] = val;
    ide_ctrl_write(bus, addr, val);
}

static void ns87415_portio_ctrl_write(void *opaque, uint32_t addr,
                                      uint32_t val)
{
    ns87415_ctrl_write(opaque, addr, val);
}

static const MemoryRegionPortio ns87415_portio2_list[] = {
    { 0, 1, 1, .read = ide_status_read, .write = ns87415_portio_ctrl_write },
    PORTIO_END_OF_LIST(),
};

/* BAR1/BAR3: Alternate Status / Device Control at offset 2 */
static uint64_t ns87415_ctl_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr != 2 || size != 1) {
        return MAKE_64BIT_MASK(0, size * 8);
    }
    return ide_status_read(opaque, addr + 2);
}

static void ns87415_ctl_bar_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    if (addr == 2 && size == 1) {
        ns87415_ctrl_write(opaque, addr + 2, val);
    }
}

static const MemoryRegionOps ns87415_ctl_bar_ops = {
    .read = ns87415_ctl_bar_read,
    .write = ns87415_ctl_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * 40h bit 1 drives DRST#.  ide_bus_reset() also resets the bus master
 * registers, which DRST# does not: keep the PRD pointer and bits 5/6.
 */
static void ns87415_drive_reset(NS87415State *s)
{
    PCIIDEState *d = PCI_IDE(s);
    int ch;

    trace_ns87415_drive_reset();
    for (ch = 0; ch < 2; ch++) {
        BMDMAState *bm = &d->bmdma[ch];
        uint32_t prd = bm->addr;
        uint8_t cap = bm->status & 0x60;

        ide_bus_reset(&d->bus[ch], IDE_RESET_HARDWARE);
        bm->addr = prd;
        bm->status |= cap;
    }
    s->irq_level = 0;
    ns87415_update_irq(s, 0);
    ns87415_update_irq(s, 1);
}

static void ns87415_config_write(PCIDevice *pd, uint32_t addr, uint32_t val,
                                 int len)
{
    NS87415State *s = NS87415_IDE(pd);
    uint8_t old_ctrl1 = pd->config[NS87415_CTRL1];

    pci_default_write_config(pd, addr, val, len);

    if (~old_ctrl1 & pd->config[NS87415_CTRL1] & CTRL1_DRST) {
        ns87415_drive_reset(s);
    }
    if (range_covers_byte(addr, len, NS87415_CTRL2)) {
        ns87415_update_irq(s, 0);
        ns87415_update_irq(s, 1);
    }
    if (range_covers_byte(addr, len, PCI_CLASS_PROG) ||
        ranges_overlap(addr, len, PCI_COMMAND, 2) ||
        range_covers_byte(addr, len, NS87415_CTRL2)) {
        ns87415_update_decode(s);
    }
}

static void ns87415_reset_hold(Object *obj, ResetType type)
{
    NS87415State *s = NS87415_IDE(obj);
    PCIIDEState *d = PCI_IDE(obj);
    uint8_t *c = PCI_DEVICE(obj)->config;
    int i;

    for (i = 0; i < 2; i++) {
        ide_bus_reset(&d->bus[i], IDE_RESET_HARDWARE);
        qemu_set_irq(d->isa_irq[i], 0);
    }
    s->irq_level = 0;

    c[PCI_CLASS_PROG] = NS87415_PROGIF;
    c[PCI_LATENCY_TIMER] = 0;
    memset(c + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    for (i = 0; i < ARRAY_SIZE(ns87415_timing_regs); i++) {
        c[ns87415_timing_regs[i]] = 0x85;
    }
    c[NS87415_CMDTIM] = 0xb7;
    c[NS87415_SECSIZE] = 0xee;
}

/*
 * The PCI bus reset clears Command and the BARs after the device's hold
 * phase, so the datasheet values are applied here.  All-ones BARs read
 * back as FFFFFFF9h/FDh/F9h/FDh/F1h and decode nothing until programmed.
 */
static void ns87415_reset_exit(Object *obj, ResetType type)
{
    PCIDevice *pd = PCI_DEVICE(obj);
    int bar;

    for (bar = 0; bar < 5; bar++) {
        pci_default_write_config(pd, PCI_BASE_ADDRESS_0 + 4 * bar,
                                 0xffffffff, 4);
    }
    pci_default_write_config(pd, PCI_COMMAND, NS87415_COMMAND_RESET, 2);
    ns87415_update_decode(NS87415_IDE(obj));
}

static void ns87415_realize(PCIDevice *dev, Error **errp)
{
    NS87415State *s = NS87415_IDE(dev);
    PCIIDEState *d = PCI_IDE(dev);
    DeviceState *ds = DEVICE(dev);
    uint8_t *c = dev->config;
    int i;

    if (!s->lio) {
        error_setg(errp, "ns87415-ide: 'lio' link property is not set");
        return;
    }

    c[PCI_CLASS_PROG] = NS87415_PROGIF;
    /* Interrupts go through the chip's router, never to a PCI pin. */
    c[PCI_INTERRUPT_PIN] = 0;
    dev->wmask[PCI_CLASS_PROG] = 0x05;
    pci_set_word(dev->wmask + PCI_COMMAND, NS87415_COMMAND_WMASK);
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;
    dev->wmask[PCI_INTERRUPT_LINE] = 0;
    /* 43h, reserved bytes and the shadows stay read-only */
    memset(dev->wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    dev->wmask[NS87415_CTRL1] = 0xf2;
    dev->wmask[NS87415_CTRL2] = 0x3f;
    dev->wmask[NS87415_CTRL3] = 0xfb;
    for (i = 0; i < ARRAY_SIZE(ns87415_timing_regs); i++) {
        dev->wmask[ns87415_timing_regs[i]] = 0xff;
    }
    dev->wmask[NS87415_CMDTIM] = 0xff;
    dev->wmask[NS87415_SECSIZE] = 0xff;

    memory_region_init(&d->bmdma_bar, OBJECT(d), "ns87415-bmdma-container",
                       16);
    qdev_init_gpio_in(ds, ns87415_set_irq, 2);
    for (i = 0; i < 2; i++) {
        BMDMAState *bm = &d->bmdma[i];

        memory_region_init_io(&d->data_bar[i], OBJECT(d), &pci_ide_data_le_ops,
                              &d->bus[i], i ? "ns87415-ide1-data" :
                                              "ns87415-ide0-data", 8);
        pci_register_bar(dev, 2 * i, PCI_BASE_ADDRESS_SPACE_IO,
                         &d->data_bar[i]);
        memory_region_init_io(&d->cmd_bar[i], OBJECT(d), &ns87415_ctl_bar_ops,
                              &d->bus[i], i ? "ns87415-ide1-cmd" :
                                              "ns87415-ide0-cmd", 4);
        pci_register_bar(dev, 2 * i + 1, PCI_BASE_ADDRESS_SPACE_IO,
                         &d->cmd_bar[i]);

        memory_region_init_io(&bm->extra_io, OBJECT(d), &ns87415_bmdma_ops,
                              bm, "ns87415-bmdma", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8, &bm->extra_io);
        memory_region_init_io(&bm->addr_ioport, OBJECT(d),
                              &bmdma_addr_ioport_ops, bm, "bmdma", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8 + 4,
                                    &bm->addr_ioport);

        ide_bus_init(&d->bus[i], sizeof(d->bus[i]), ds, i, MAX_IDE_DEVS);
        ide_bus_init_output_irq(&d->bus[i], qdev_get_gpio_in(ds, i));
        bmdma_init(&d->bus[i], bm, d);
        ide_bus_register_restart_cb(&d->bus[i]);

        /* Legacy ports: created once, switched by ns87415_update_decode() */
        portio_list_init(&d->bus[i].portio_list, OBJECT(d), ide_portio_list,
                         &d->bus[i], "ide");
        portio_list_add(&d->bus[i].portio_list, pci_address_space_io(dev),
                        i ? 0x170 : 0x1f0);
        portio_list_init(&d->bus[i].portio2_list, OBJECT(d),
                         ns87415_portio2_list, &d->bus[i], "ide");
        portio_list_add(&d->bus[i].portio2_list, pci_address_space_io(dev),
                        i ? 0x376 : 0x3f6);
    }
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &d->bmdma_bar);

    s->dma_ops = *d->bmdma[0].dma.ops;
    s->dma_ops.cmd_done = ns87415_dma_cmd_done;
    d->bmdma[0].dma.ops = d->bmdma[1].dma.ops = &s->dma_ops;

    ns87560_lio_set_decode_notifier(s->lio, ns87415_decode_notify, dev);
    ns87415_update_decode(s);
}

static void ns87415_exitfn(PCIDevice *dev)
{
    NS87415State *s = NS87415_IDE(dev);
    PCIIDEState *d = PCI_IDE(dev);
    int i;

    ns87560_lio_set_decode_notifier(s->lio, NULL, NULL);
    for (i = 0; i < 2; i++) {
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].extra_io);
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].addr_ioport);
        portio_list_del(&d->bus[i].portio_list);
        portio_list_destroy(&d->bus[i].portio_list);
        portio_list_del(&d->bus[i].portio2_list);
        portio_list_destroy(&d->bus[i].portio2_list);
    }
}

static int ns87415_post_load(void *opaque, int version_id)
{
    ns87415_update_decode(opaque);
    return 0;
}

static const VMStateDescription vmstate_ns87415 = {
    .name = "ns87415-ide",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ns87415_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, NS87415State, 0, vmstate_ide_pci,
                       PCIIDEState),
        VMSTATE_UINT8(irq_level, NS87415State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ns87415_properties[] = {
    DEFINE_PROP_LINK("lio", NS87415State, lio, TYPE_NS87560_LIO,
                     NS87560LIOState *),
};

static void ns87415_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ns87415_reset_hold;
    rc->phases.exit = ns87415_reset_exit;
    k->realize = ns87415_realize;
    k->exit = ns87415_exitfn;
    k->config_write = ns87415_config_write;
    k->vendor_id = PCI_VENDOR_ID_NS;
    k->device_id = PCI_DEVICE_ID_NS_87415;
    k->revision = NS87415_REVISION;
    k->class_id = PCI_CLASS_STORAGE_IDE;
    dc->vmsd = &vmstate_ns87415;
    dc->desc = "NS87415 IDE (PC87560 PCI function 0)";
    device_class_set_props(dc, ns87415_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    /* Reason: function 0 of the ns87560 chip, needs the 'lio' link */
    dc->user_creatable = false;
}

static const TypeInfo ns87415_types[] = {
    {
        .name = TYPE_NS87415_IDE,
        .parent = TYPE_PCI_IDE,
        .instance_size = sizeof(NS87415State),
        .class_init = ns87415_class_init,
    },
};

DEFINE_TYPES(ns87415_types)

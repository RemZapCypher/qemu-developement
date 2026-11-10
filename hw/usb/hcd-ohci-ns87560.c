/*
 * OHCI USB host controller, function 2 of the National Semiconductor
 * PC87560 (NS87560) PCI System I/O
 *
 * An OpenHCI 1.0a controller with a 4 KiB OHCI BAR (operational registers
 * plus the Hce legacy-support registers at 100h-10Ch) and a 4 KiB National
 * vendor BAR (NUSBBAR).  The root hub has three ports; OHCI port 1 has no
 * transceiver.  The OHCI interrupt goes to the chip's interrupt router,
 * not to this function's INTx: INTD carries the chip's 8259 output
 * (CPUINT) instead, which is what HP firmware and Linux expect.
 *
 * Reference: National Semiconductor, "PC87560UBD PCI System I/O",
 * October 1998, chapter 19.
 *
 * Copyright (c) 2026 Soumyajyotii Ssarkar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hcd-ohci.h"
#include "trace.h"

/* Real C3700 hardware reports revision 02h (the 1998 datasheet says 03h). */
#define NS87560_OHCI_REVISION   0x02
#define NS87560_OHCI_PROGIF     0x10
#define NS87560_OHCI_CMD_WMASK  (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | \
                                 PCI_COMMAND_MASTER | PCI_COMMAND_PARITY | \
                                 PCI_COMMAND_SERR)
#define NS87560_OHCI_BAR_SIZE   0x1000
#define NS87560_OHCI_NUM_PORTS  3

#define HCREVISION_VAL          0x00000110  /* OHCI 1.0, legacy support */
#define HCE_BASE                0x100
#define HCE_SIZE                0x10
#define HCECONTROL_EE           BIT(0)

/* NUSBBAR registers */
#define NSC_CONTROL             0x00
#define   NSC_CONTROL_RESET     0x00000014
#define   NSC_CONTROL_BE        BIT(0)
#define   NSC_CONTROL_CCE       BIT(1)
#define   NSC_CONTROL_WMASK     0xff0103ff
#define NSC_CMD_STATUS          0x04
#define   NSC_CMD_STATUS_W1C    0x003f0fff
#define   NSC_CMD_STATUS_UE_PCI BIT(17)
#define NSC_PORT_RX_STATUS      0x08
#define NSC_ROOT_HUB_STATUS     0x34
#define   NSC_ROOT_HUB_STATUS_VAL 0x000a0a0a

OBJECT_DECLARE_SIMPLE_TYPE(NS87560OHCIState, NS87560_OHCI)

struct NS87560OHCIState {
    PCIDevice dev;

    OHCIState ohci;             /* .irq goes to the chip's router */
    MemoryRegion bar0;          /* 4 KiB: core at 0, Hce and reserved space */
    MemoryRegion hcrev;         /* HcRevision, overlays the core */
    MemoryRegion nusb;
    uint32_t nsc_control;
    uint32_t nsc_cmd_status;
};

/* The QEMU core reports HcRevision 10h; this controller sets Legacy (bit 8). */
static uint64_t ns87560_ohci_hcrev_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    return HCREVISION_VAL;
}

static void ns87560_ohci_hcrev_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "ns87560-ohci: write to HcRevision\n");
}

static const MemoryRegionOps ns87560_ohci_hcrev_ops = {
    .read = ns87560_ohci_hcrev_read,
    .write = ns87560_ohci_hcrev_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * BAR0 above the core's 256 bytes.  The Hce registers read 0: legacy
 * keyboard emulation is off, and nothing on hppa uses it.
 */
static uint64_t ns87560_ohci_bar0_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    if (addr < HCE_BASE || addr >= HCE_BASE + HCE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ns87560-ohci: read of reserved offset 0x%" HWADDR_PRIx
                      "\n", addr);
    }
    return 0;
}

static void ns87560_ohci_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    if (addr < HCE_BASE || addr >= HCE_BASE + HCE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ns87560-ohci: write to reserved offset 0x%" HWADDR_PRIx
                      "\n", addr);
    } else if (addr == HCE_BASE && (val & HCECONTROL_EE)) {
        qemu_log_mask(LOG_UNIMP,
                      "ns87560-ohci: HceControl.EmulationEnable\n");
    }
}

static const MemoryRegionOps ns87560_ohci_bar0_ops = {
    .read = ns87560_ohci_bar0_read,
    .write = ns87560_ohci_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint32_t ns87560_ohci_nusb_reg(NS87560OHCIState *s, hwaddr reg)
{
    switch (reg) {
    case NSC_CONTROL:
        return s->nsc_control;
    case NSC_CMD_STATUS:
        return s->nsc_cmd_status;
    case NSC_PORT_RX_STATUS:
        return 0;
    case NSC_ROOT_HUB_STATUS:
        return NSC_ROOT_HUB_STATUS_VAL;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ns87560-ohci: reserved NUSBBAR offset 0x%" HWADDR_PRIx
                      "\n", reg);
        return 0;
    }
}

static uint64_t ns87560_ohci_nusb_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    uint32_t val = extract32(ns87560_ohci_nusb_reg(opaque, addr & ~3),
                             (addr & 3) * 8, size * 8);

    trace_ns87560_ohci_nusb_read(addr, size, val);
    return val;
}

static void ns87560_ohci_nusb_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NS87560OHCIState *s = opaque;
    int shift = (addr & 3) * 8;
    uint32_t lanes = MAKE_64BIT_MASK(shift, size * 8);
    uint32_t v = val << shift;

    trace_ns87560_ohci_nusb_write(addr, size, val);
    switch (addr & ~3) {
    case NSC_CONTROL:
        s->nsc_control ^= (s->nsc_control ^ v) & lanes & NSC_CONTROL_WMASK;
        if (s->nsc_control & (NSC_CONTROL_BE | NSC_CONTROL_CCE)) {
            qemu_log_mask(LOG_UNIMP, "ns87560-ohci: NscControl BigEndian/"
                          "ConsistencyCheck (0x%08x)\n", s->nsc_control);
        }
        break;
    case NSC_CMD_STATUS:
        s->nsc_cmd_status &= ~(v & lanes & NSC_CMD_STATUS_W1C);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ns87560-ohci: write to NUSBBAR offset 0x%" HWADDR_PRIx
                      "\n", addr);
    }
}

static const MemoryRegionOps ns87560_ohci_nusb_ops = {
    .read = ns87560_ohci_nusb_read,
    .write = ns87560_ohci_nusb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * The chip's 8259 output (CPUINT) is wired on the C3700 to the line that
 * firmware lists for this function's INTD, so it is driven here.
 */
static void ns87560_ohci_cascade(void *opaque, int n, int level)
{
    pci_set_irq(PCI_DEVICE(opaque), level);
}

/* A DMA failure is a PCI master abort on this chip. */
static void ns87560_ohci_die(OHCIState *ohci)
{
    NS87560OHCIState *s = container_of(ohci, NS87560OHCIState, ohci);

    ohci_sysbus_die(ohci);
    s->nsc_cmd_status |= NSC_CMD_STATUS_UE_PCI;
    pci_word_test_and_set_mask(s->dev.config + PCI_STATUS,
                               PCI_STATUS_REC_MASTER_ABORT);
}

static void ns87560_ohci_instance_init(Object *obj)
{
    NS87560OHCIState *s = NS87560_OHCI(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->ohci.irq,
                             NS87560_OHCI_GPIO_IRQ, 1);
    qdev_init_gpio_in_named(DEVICE(obj), ns87560_ohci_cascade,
                            NS87560_OHCI_GPIO_CASCADE, 1);
}

static void ns87560_ohci_realize(PCIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    NS87560OHCIState *s = NS87560_OHCI(dev);

    dev->config[PCI_CLASS_PROG] = NS87560_OHCI_PROGIF;
    dev->config[PCI_INTERRUPT_PIN] = 4;     /* INTD */
    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_word(dev->wmask + PCI_COMMAND, NS87560_OHCI_CMD_WMASK);
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;
    /* No device-specific registers */
    memset(dev->wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);

    usb_ohci_init(&s->ohci, DEVICE(dev), NS87560_OHCI_NUM_PORTS, 0, NULL, 0,
                  pci_get_address_space(dev), ns87560_ohci_die, errp);
    if (*errp) {
        return;
    }
    /* OHCI port 1 has no transceiver: nothing can ever connect to it. */
    usb_unregister_port(&s->ohci.bus, &s->ohci.rhport[0].port);

    memory_region_init_io(&s->bar0, OBJECT(s), &ns87560_ohci_bar0_ops, s,
                          "ns87560-ohci-bar0", NS87560_OHCI_BAR_SIZE);
    memory_region_add_subregion(&s->bar0, 0, &s->ohci.mem);
    memory_region_init_io(&s->hcrev, OBJECT(s), &ns87560_ohci_hcrev_ops, s,
                          "ns87560-ohci-hcrev", 4);
    memory_region_add_subregion_overlap(&s->bar0, 0, &s->hcrev, 1);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    memory_region_init_io(&s->nusb, OBJECT(s), &ns87560_ohci_nusb_ops, s,
                          "ns87560-ohci-nusb", NS87560_OHCI_BAR_SIZE);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->nusb);
}

static void ns87560_ohci_exit(PCIDevice *dev)
{
    OHCIState *ohci = &NS87560_OHCI(dev)->ohci;

    ohci_bus_stop(ohci);
    if (ohci->async_td) {
        usb_cancel_packet(&ohci->usb_packet);
        ohci->async_td = 0;
    }
    ohci_stop_endpoints(ohci);
    usb_bus_release(&ohci->bus);
    timer_free(ohci->eof_timer);
}

static void ns87560_ohci_reset_hold(Object *obj, ResetType type)
{
    NS87560OHCIState *s = NS87560_OHCI(obj);

    ohci_hard_reset(&s->ohci);
    /* The core reset clears the status but does not lower the line. */
    qemu_irq_lower(s->ohci.irq);
    s->nsc_control = NSC_CONTROL_RESET;
    s->nsc_cmd_status = 0;
}

/* After the PCI bus reset, which clears Interrupt Line */
static void ns87560_ohci_reset_exit(Object *obj, ResetType type)
{
    PCI_DEVICE(obj)->config[PCI_INTERRUPT_LINE] = 0xff;
}

static const VMStateDescription vmstate_ns87560_ohci = {
    .name = "ns87560-ohci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, NS87560OHCIState),
        VMSTATE_STRUCT(ohci, NS87560OHCIState, 1, vmstate_ohci_state,
                       OHCIState),
        VMSTATE_UINT32(nsc_control, NS87560OHCIState),
        VMSTATE_UINT32(nsc_cmd_status, NS87560OHCIState),
        VMSTATE_END_OF_LIST()
    }
};

static void ns87560_ohci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ns87560_ohci_reset_hold;
    rc->phases.exit = ns87560_ohci_reset_exit;
    k->realize = ns87560_ohci_realize;
    k->exit = ns87560_ohci_exit;
    k->vendor_id = PCI_VENDOR_ID_NS;
    k->device_id = PCI_DEVICE_ID_NS_87560_USB;
    k->revision = NS87560_OHCI_REVISION;
    k->class_id = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_ns87560_ohci;
    dc->desc = "NS87560 OHCI USB (PCI function 2)";
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    /* Part of the ns87560 container, which wires it */
    dc->user_creatable = false;
}

static const TypeInfo ns87560_ohci_types[] = {
    {
        .name = TYPE_NS87560_OHCI,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(NS87560OHCIState),
        .instance_init = ns87560_ohci_instance_init,
        .class_init = ns87560_ohci_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(ns87560_ohci_types)

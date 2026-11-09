/*
 * OHCI USB host controller, function 2 of the National Semiconductor
 * PC87560 (NS87560) PCI System I/O
 *
 * Reference: National Semiconductor, "PC87560UBD PCI System I/O",
 * October 1998, chapter 19.
 *
 * Copyright (c) 2026 Soumyajyotii Ssarkar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* Real C3700 hardware reports revision 02h (the 1998 datasheet says 03h). */
#define NS87560_OHCI_REVISION   0x02
#define NS87560_OHCI_PROGIF     0x10

OBJECT_DECLARE_SIMPLE_TYPE(NS87560OHCIState, NS87560_OHCI)

struct NS87560OHCIState {
    PCIDevice dev;

    qemu_irq usb_irq;           /* OHCI interrupt, to the chip's router */
};

/*
 * The chip's 8259 output (CPUINT) is wired on the C3700 to the line that
 * firmware lists for this function's INTD, so it is driven here.
 */
static void ns87560_ohci_cascade(void *opaque, int n, int level)
{
    pci_set_irq(PCI_DEVICE(opaque), level);
}

static void ns87560_ohci_instance_init(Object *obj)
{
    NS87560OHCIState *s = NS87560_OHCI(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->usb_irq,
                             NS87560_OHCI_GPIO_IRQ, 1);
    qdev_init_gpio_in_named(DEVICE(obj), ns87560_ohci_cascade,
                            NS87560_OHCI_GPIO_CASCADE, 1);
}

static void ns87560_ohci_realize(PCIDevice *dev, Error **errp)
{
    dev->config[PCI_CLASS_PROG] = NS87560_OHCI_PROGIF;
    dev->config[PCI_INTERRUPT_PIN] = 4;     /* INTD */
}

static const VMStateDescription vmstate_ns87560_ohci = {
    .name = "ns87560-ohci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, NS87560OHCIState),
        VMSTATE_END_OF_LIST()
    }
};

static void ns87560_ohci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ns87560_ohci_realize;
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

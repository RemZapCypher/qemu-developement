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
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define NS87560_LIO_REVISION    0x01

/* ---- Function 1: legacy I/O bridge ------------------------------------ */

static void ns87560_router_set_irq(void *opaque, int n, int level)
{
    NS87560LIOState *s = opaque;

    if (level) {
        s->src_level |= BIT(n);
    } else {
        s->src_level &= ~BIT(n);
    }
}

static void ns87560_lio_instance_init(Object *obj)
{
    NS87560LIOState *s = NS87560_LIO(obj);

    qdev_init_gpio_in_named(DEVICE(obj), ns87560_router_set_irq,
                            NS87560_LIO_GPIO_ROUTER, NS87560_SRC_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cascade_out,
                             NS87560_LIO_GPIO_INTR, 1);
}

static void ns87560_lio_realize(PCIDevice *d, Error **errp)
{
}

static const VMStateDescription vmstate_ns87560_lio = {
    .name = "ns87560-lio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, NS87560LIOState),
        VMSTATE_UINT32(src_level, NS87560LIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void ns87560_lio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ns87560_lio_realize;
    k->vendor_id = PCI_VENDOR_ID_NS;
    k->device_id = PCI_DEVICE_ID_NS_87560_LIO;
    k->revision = NS87560_LIO_REVISION;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
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

static void ns87560_class_init(ObjectClass *klass, void *data)
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

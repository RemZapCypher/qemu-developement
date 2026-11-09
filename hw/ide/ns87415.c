/*
 * National Semiconductor PC87415-compatible IDE controller,
 * function 0 of the PC87560 (NS87560) PCI System I/O
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
#include "hw/ide/pci.h"
#include "hw/isa/ns87560.h"
#include "hw/pci/pci_ids.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define NS87415_REVISION    0x03
#define NS87415_PROGIF      0x8a    /* both channels legacy, switchable, BM */

static void ns87415_realize(PCIDevice *dev, Error **errp)
{
    uint8_t *c = dev->config;

    c[PCI_CLASS_PROG] = NS87415_PROGIF;
    /* Channel 1 and 2 legacy/native mode bits (p.89) */
    dev->wmask[PCI_CLASS_PROG] = 0x05;
    /*
     * NS registers 40h-5Dh are not modelled yet: keep them read-only 0.
     * In particular 43h bits 0-1 must read 0, or Linux waits forever.
     */
    memset(dev->wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    /* Interrupts go through the chip's router, never to a PCI pin. */
    c[PCI_INTERRUPT_PIN] = 0;
}

static void ns87415_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ns87415_realize;
    k->vendor_id = PCI_VENDOR_ID_NS;
    k->device_id = PCI_DEVICE_ID_NS_87415;
    k->revision = NS87415_REVISION;
    k->class_id = PCI_CLASS_STORAGE_IDE;
    dc->vmsd = &vmstate_ide_pci;
    dc->desc = "NS87415 IDE (PC87560 PCI function 0)";
    /* Part of the ns87560 container, which wires it */
    dc->user_creatable = false;
}

static const TypeInfo ns87415_types[] = {
    {
        .name = TYPE_NS87415_IDE,
        .parent = TYPE_PCI_IDE,
        .class_init = ns87415_class_init,
    },
};

DEFINE_TYPES(ns87415_types)

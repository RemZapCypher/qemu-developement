/*
 * National Semiconductor PC87560 (NS87560) PCI System I/O
 *
 * Copyright (c) 2026 Soumyajyotii Ssarkar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ISA_NS87560_H
#define HW_ISA_NS87560_H

#include "hw/core/irq.h"
#include "hw/isa/isa.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"

#define TYPE_NS87560        "ns87560"
#define TYPE_NS87560_LIO    "ns87560-lio"
#define TYPE_NS87415_IDE    "ns87415-ide"
#define TYPE_NS87560_OHCI   "ns87560-ohci"

OBJECT_DECLARE_SIMPLE_TYPE(NS87560State, NS87560)
OBJECT_DECLARE_SIMPLE_TYPE(NS87560LIOState, NS87560_LIO)

/* PCI function numbers within the chip's slot */
#define NS87560_FN_IDE   0
#define NS87560_FN_LIO   1
#define NS87560_FN_USB   2

/* GPIO names */
#define NS87560_LIO_GPIO_ROUTER    "router-in"
#define NS87560_LIO_GPIO_INTR      "intr"
#define NS87560_OHCI_GPIO_IRQ      "usb-irq"
#define NS87560_OHCI_GPIO_CASCADE  "cascade"
#define NS87415_GPIO_IRQ           "isa-irq"

/*
 * Interrupt router sources.  Source n is routed by the nibble n % 2 of
 * the IRCR register at config offset 0x69 + n / 2.
 */
typedef enum NS87560IrqSource {
    NS87560_SRC_SP1 = 0,
    NS87560_SRC_SP2,
    NS87560_SRC_PP,
    NS87560_SRC_FDC,
    NS87560_SRC_IDE1,
    NS87560_SRC_IDE2,
    NS87560_SRC_INTA,
    NS87560_SRC_INTB,
    NS87560_SRC_INTC,
    NS87560_SRC_INTD,
    NS87560_SRC_KBD,
    NS87560_SRC_MOUSE,
    NS87560_SRC_FX0,
    NS87560_SRC_FX1,
    NS87560_SRC_FX2,
    NS87560_SRC_USB,
    NS87560_SRC_SCI,
    NS87560_SRC_RSVD,
    NS87560_SRC_COUNT
} NS87560IrqSource;

struct NS87560LIOState {
    PCIDevice dev;

    ISABus *isa_bus;
    qemu_irq *isa_irqs;         /* 8259 inputs 0-15 */
    ISADevice *pic[2];          /* master, slave */
    IRQState i8259_irq;         /* target of the master's INT output */
    qemu_irq cascade_out;       /* 8259 master INT (CPUINT) */
    qemu_irq bus_irqs[ISA_NUM_IRQS];
    MemoryRegion pic_cmd_io[2]; /* ICW1.LTIM filter at 20h / A0h */
    uint32_t src_level;         /* bit n = level of NS87560IrqSource n */
    uint16_t pic_level;         /* derived: level driven to each 8259 input */
    uint64_t unimp_logged;      /* config registers already reported */
};

struct NS87560State {
    DeviceState parent_obj;

    PCIBus *bus;
    int32_t slot;

    PCIDevice *ide;
    NS87560LIOState *lio;
    PCIDevice *usb;

    qemu_irq usb_split[2];
};

#endif

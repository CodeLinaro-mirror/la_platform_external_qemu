// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nuvoton NPCM7xx PCI Mailbox Module
 *
 * Copyright 2021 Google LLC
 */
#ifndef NPCM7XX_PCI_MBOX_H
#define NPCM7XX_PCI_MBOX_H

#include "chardev/char-fe.h"
#include "system/memory.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define NPCM7XX_PCI_MBOX_RAM_SIZE 0x4000

#define NPCM7XX_PCI_VENDOR_ID   0x1050
#define NPCM7XX_PCI_DEVICE_ID   0x0750
#define NPCM7XX_PCI_REVISION    0
#define NPCM7XX_PCI_CLASS_CODE  0xff

/*
 * Maximum amount of control registers in PCI Mailbox module. Do not increase
 * this value without bumping vm version.
 */
#define NPCM7XX_PCI_MBOX_NR_REGS 3

/**
 * struct NPCM7xxPciMboxState - PCI Mailbox Device
 * @parent: System bus device.
 * @ram: the mailbox RAM memory space
 * @iomem: Memory region through which registers are accessed.
 * @content: The content of the PCI mailbox, initialized to 0.
 * @regs: The MMIO registers.
 */
typedef struct NPCM7xxPCIMBoxState {
    SysBusDevice parent;

    MemoryRegion ram;
    MemoryRegion iomem;

    qemu_irq irq;
    uint8_t content[NPCM7XX_PCI_MBOX_RAM_SIZE];
    uint32_t regs[NPCM7XX_PCI_MBOX_NR_REGS];
} NPCM7xxPCIMBoxState;

#define TYPE_NPCM7XX_PCI_MBOX "npcm7xx-pci-mbox"
#define NPCM7XX_PCI_MBOX(obj) \
    OBJECT_CHECK(NPCM7xxPCIMBoxState, (obj), TYPE_NPCM7XX_PCI_MBOX)

#endif /* NPCM7XX_PCI_MBOX_H */

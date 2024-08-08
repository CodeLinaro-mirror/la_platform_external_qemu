/*
 * This is a generic chardev backend based PCI Mailbox memory mmio module.
 * Inspried by the Nuvoton PCI Mailbox (npc7xx_pci_mbox.c).
 *
 * Copyright 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PCI_MBOX_H
#define PCI_MBOX_H

#include "chardev/char-fe.h"
#include "system/memory.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define PCI_MBOX_RAM_SIZE 0x4000

typedef enum PCIMBoxHostState {
  PCI_MBOX_STATE_IDLE,
  PCI_MBOX_STATE_OFFSET,
  PCI_MBOX_STATE_SIZE,
  PCI_MBOX_STATE_DATA,
} PCIMBoxHostState;

/**
 * struct PciMboxState - PCI Mailbox Device
 * @parent: System bus device.
 * @ram: the mailbox RAM memory space
 * @content: The content of the PCI mailbox, initialized to 0.
 * @chr: The chardev backend used to communicate with core CPU.
 * @offset: The offset to start transfer.
 */
typedef struct PCIMBoxState {
  SysBusDevice parent;

  MemoryRegion ram;
  uint32_t ram_size;
  uint8_t *content;
  CharFrontend chr;

  /* aux data for receiving host commands. */
  PCIMBoxHostState state;
  uint8_t op;
  hwaddr offset;
  uint8_t size;
  uint64_t data;
  int receive_count;
} PCIMBoxState;

#define TYPE_PCI_MBOX "pci-mbox"
#define PCI_MBOX(obj) OBJECT_CHECK(PCIMBoxState, (obj), TYPE_PCI_MBOX)

#endif /* PCI_MBOX_H */

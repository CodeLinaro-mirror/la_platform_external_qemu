/*
 * eSPI Slave Interface Module
 *
 * Targeting the NPCM840G which supports the Intel Enhanced Serial Peripheral
 * Interface (eSPI) Revision 1.0, January 2016
 *
 * Copyright 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NPCM_ESPI_H
#define NPCM_ESPI_H

#include "hw/sysbus.h"
#include "exec/memory.h"
#include "qom/object.h"
#include "hw/irq.h"

#define NPCM_ESPI_NR_REGS 0x180

typedef struct NPCMESPIState {
    SysBusDevice parent;
    MemoryRegion mmio;

    uint32_t regs[NPCM_ESPI_NR_REGS];
    qemu_irq irq;
} NPCMESPIState;

#define TYPE_NPCM_ESPI "npcm_espi"
OBJECT_DECLARE_SIMPLE_TYPE(NPCMESPIState, NPCM_ESPI)

#endif /* NPCM_ESPI_H */

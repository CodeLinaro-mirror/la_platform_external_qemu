/*
 * NPCM8xx GDMA Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#ifndef NPCM8XX_GDMA_H
#define NPCM8XX_GDMA_H

#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "ui/console.h"
#include "system/dma.h"

#define TYPE_NPCM8XX_GDMA "npcm8xx.gdma"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM8xxGDMA, NPCM8XX_GDMA)
#define TYPE_NPCM8XX_GDMA_CHANNEL "npcm8xx.gdma-channel"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM8xxGDMAChannel, NPCM8XX_GDMA_CHANNEL)

#define NPCM8XX_GDMA_MMIO_SIZE 0x1000

#define NPCM8XX_GDMA_CHANNEL_OFFSET 0x20
#define NPCM8XX_GDMA_CHANNEL_MMIO_SIZE 0x1c
#define NPCM8XX_GDMA_CHANNEL_NR_REGS (NPCM8XX_GDMA_CHANNEL_MMIO_SIZE >> 2)

typedef struct NPCM8xxGDMAChannel {
    SysBusDevice parent;
    MemoryRegion iomem;
    NPCM8xxGDMA *controller;
    /* For DMA transactions. */
    AddressSpace dma_as;
    MemTxAttrs attrs;

    uint32_t regs[NPCM8XX_GDMA_CHANNEL_NR_REGS];
} NPCM8xxGDMAChannel;

typedef struct NPCM8xxGDMA {
    SysBusDevice parent;
    MemoryRegion iomem;
    NPCM8xxGDMAChannel channels[2];

    struct {
        uint8_t id;
    } cfg;

    qemu_irq irq;
} NPCM8xxGDMA;

#endif /* NPCM8XX_GDMA_H */


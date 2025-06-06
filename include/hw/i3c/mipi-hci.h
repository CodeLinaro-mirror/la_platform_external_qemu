/*
 * MIPI HCI I3C Controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MIPI_HCI_H
#define MIPI_HCI_H

#include "hw/core/sysbus.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/hci-core.h"
#include "hw/i3c/hci-dma.h"
#include "hw/i3c/hci-ext.h"
#include "hw/i3c/hci-dat.h"
#include "hw/i3c/hci-dct.h"
#include "hw/i3c/hci-ibi.h"
#include "hw/i3c/hci-pio.h"
#include "hw/i3c/i3c.h"

#define TYPE_MIPI_HCI "mipi.hci"
OBJECT_DECLARE_TYPE(MIPIHCIState, MIPIHCIClass, MIPI_HCI)

#define MIPI_HCI_MMIO_SIZE 0x1000

/* The context in which an IRQ is happening. */
typedef enum MIPIHCIIRQContext {
    MIPI_HCI_IRQ_CONTEXT_CORE = 0,
    MIPI_HCI_IRQ_CONTEXT_DMA = 1,
    MIPI_HCI_IRQ_CONTEXT_PIO = 2,
} MIPIHCIIRQContext;

typedef struct MIPIHCIClass {
    SysBusDeviceClass parent_class;

    /* Overridable in case other implementations have multiple IRQ lines. */
    void (*update_irq)(MIPIHCIState *s, MIPIHCIIRQContext ctx);
    /* Overridable, since halt state could be presented in extended caps. */
    void (*enter_halt)(MIPIHCIState *s);
    /* Aspeed-specific. */
    uint8_t (*get_next_dynamic_addr)(MIPIHCIState *s, uint8_t dat_index);
    uint8_t (*get_dev_dynamic_addr)(MIPIHCIState *s, uint8_t dat_index);
    uint32_t (*dat_dev_index_from_addr)(MIPIHCIState *s, uint8_t addr);
} MIPIHCIClass;

typedef struct MIPIHCIState {
    SysBusDevice parent;

    HCICoreState core;
    HCIDMAState dma;
    HCIExtCapState ext_cap;
    HCIDATState dat;
    HCIDCTState dct;
    HCIPIOState pio;

    struct {
        uint32_t ring_header_section_offset;
        uint32_t ext_caps_section_offset;
        uint32_t num_irqs;
    } cfg;

    MemoryRegion iomem;
    qemu_irq *irq;
    I3CBus *bus;
    /*
     * A pointer to an IBI that's currently in progress.
     * Freed once the IBI is done.
     */
    IbiStatus *ibi_in_progress;
} MIPIHCIState;

#endif /* MIPI_HCI_H */

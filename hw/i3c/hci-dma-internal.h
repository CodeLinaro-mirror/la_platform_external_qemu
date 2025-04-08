/*
 * MIPI HCI I3C controller DMA functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_DMA_INTERNAL_H_
#define HCI_DMA_INTERNAL_H_

#include "hw/i3c/hci-dma.h"
#include "hw/i3c/mipi-hci.h"
#include "hci-cmd.h"

typedef struct DataBufferDescr {
    uint16_t block_size;
    uint16_t res0:14;
    bool ioc:1; /* Interrupt on completion. */
    bool blp:1; /* Buffer vs list pointer (for scatter gather). */
    uint32_t buffer_ptr_lo;
    uint32_t buffer_ptr_hi;
} __attribute__((packed)) DataBufferDescr;
QEMU_BUILD_BUG_ON(sizeof(DataBufferDescr) != 12);

typedef struct TransferDescr {
    CmdDescr cmd;
    DataBufferDescr data_buffer;
} __attribute__((packed)) TransferDescr;
QEMU_BUILD_BUG_ON(sizeof(TransferDescr) != 20);

uint64_t hci_dma_header_read(void *opaque, hwaddr offset, unsigned size);
void hci_dma_header_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size);

uint64_t hci_dma_read(void *opaque, hwaddr offset, unsigned size);
void hci_dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);
int hci_dma_report_ibi(MIPIHCIState *hci);

void hci_dma_reset(HCIDMAState *s);

void hci_dma_xfer(MIPIHCIState *hci);

#endif  /* HCI_DMA_INTERNAL_H_ */

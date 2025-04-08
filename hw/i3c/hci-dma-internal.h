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

uint64_t hci_dma_header_read(void *opaque, hwaddr offset, unsigned size);
void hci_dma_header_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size);

uint64_t hci_dma_read(void *opaque, hwaddr offset, unsigned size);
void hci_dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);

#endif  /* HCI_DMA_INTERNAL_H_ */

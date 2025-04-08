/*
 * MIPI HCI I3C controller DCT functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_DCT_INTERNAL_H_
#define HCI_DCT_INTERNAL_H_

#include "hw/i3c/hci-dct.h"

uint64_t hci_dct_read(void *opaque, hwaddr offset, unsigned size);
void hci_dct_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);

void hci_dct_reset(HCIDCTState *s, uint32_t num_regs);

#endif  /* HCI_DCT_INTERNAL_H_ */

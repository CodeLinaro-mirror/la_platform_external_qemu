/*
 * MIPI HCI I3C controller DAT functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_DAT_INTERNAL_H_
#define HCI_DAT_INTERNAL_H_

#include "hw/i3c/hci-dat.h"

#define HCI_DAT_DEV_NOT_FOUND 0xffffffff

uint64_t hci_dat_read(void *opaque, hwaddr offset, unsigned size);
void hci_dat_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);
void hci_dat_reset(HCIDATState *s, uint32_t num_regs);
uint32_t hci_dat_dev_index_from_addr(MIPIHCIState *hci, uint8_t addr);

#endif  /* HCI_DAT_INTERNAL_H_ */

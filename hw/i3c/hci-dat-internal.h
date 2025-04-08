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

uint64_t hci_dat_read(void *opaque, hwaddr offset, unsigned size);
void hci_dat_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);

#endif  /* HCI_DAT_INTERNAL_H_ */

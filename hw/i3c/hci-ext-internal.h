/*
 * MIPI HCI I3C controller extended capabilities
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_EXT_INTERNAL_H_
#define HCI_EXT_INTERNAL_H_

#include "hw/i3c/hci-ext.h"

uint64_t hci_ext_read(void *opaque, hwaddr offset, unsigned size);
void hci_ext_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);

#endif  /* HCI_EXT_INTERNAL_H_ */

/*
 * MIPI HCI I3C controller core functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_CORE_INTERNAL_H_
#define HCI_CORE_INTERNAL_H_

#include "system/memory.h"

uint64_t hci_core_read(void *opaque, hwaddr offset, unsigned size);
void hci_core_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);

#endif  /* HCI_CORE_INTERNAL_H_ */

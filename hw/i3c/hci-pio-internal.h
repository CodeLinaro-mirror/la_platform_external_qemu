/*
 * MIPI HCI I3C controller PIO functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_PIO_INTERNAL_H_
#define HCI_PIO_INTERNAL_H_

#include "hw/i3c/mipi-hci.h"

uint64_t hci_pio_read(void *opaque, hwaddr offset, unsigned size);
void hci_pio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size);
void hci_pio_reset(HCIPIOState *s);
int hci_pio_report_ibi(MIPIHCIState *hci);

#endif  /* HCI_PIO_INTERNAL_H_ */

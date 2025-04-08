/*
 * MIPI HCI I3C IBI data structures
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_IBI_H
#define HCI_IBI_H

#include "hw/i3c/i3c.h"

#define MAX_IBI_DATA_SIZE 256 /* Arbitrary, but should be more than enough. */

typedef struct IbiDescriptor {
    uint8_t data_length;
    uint8_t ibi_id;
    uint8_t chunks;
    uint8_t last_status:1;
    uint8_t ts:1; /* Timestamp. */
    uint8_t hw_context:3;
    uint8_t status_type:1;
    uint8_t error:1;
    uint8_t ibi_sts:1;
} __attribute__((packed)) IbiDescriptor;
QEMU_BUILD_BUG_ON(sizeof(IbiDescriptor) != sizeof(uint32_t));

typedef struct IbiStatus {
    IbiDescriptor ibi;
    uint8_t data[MAX_IBI_DATA_SIZE];
    ssize_t num_bytes;
} __attribute__((packed)) IbiStatus;

int hci_ibi_handle(I3CBus *bus, uint8_t addr, bool is_recv);
int hci_ibi_recv(I3CBus *bus, uint8_t data);
int hci_ibi_finish(I3CBus *bus);

#endif  /* HCI_IBI_H */

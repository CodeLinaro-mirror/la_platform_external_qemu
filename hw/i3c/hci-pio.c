/*
 * MIPI HCI I3C controller PIO functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hci-dma-internal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hci-cmd.h"
#include "hw/i3c/hci-ibi.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"
#include "hw/i3c/hci-pio.h"
#include "hci-pio-internal.h"

uint64_t hci_pio_read(void *opaque, hwaddr offset, unsigned size)
{
    g_autofree char *path = object_get_canonical_path(OBJECT(opaque));
    qemu_log_mask(LOG_UNIMP, "%s: Unsupported read from PIO offset at %.8"
                  HWADDR_PRIx "\n", path, offset);
    return 0;
}

void hci_pio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    g_autofree char *path = object_get_canonical_path(OBJECT(opaque));
    qemu_log_mask(LOG_UNIMP, "%s: Unsupported write of %.8" PRIx64 "to PIO "
                  "offset at %.8" HWADDR_PRIx "\n",
                  path, value, offset);
}


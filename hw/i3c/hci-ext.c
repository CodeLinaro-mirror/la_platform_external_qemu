/*
 * MIPI HCI I3C controller extended capabilities
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/hci-ext.h"
#include "hci-ext-internal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"

uint64_t hci_ext_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIExtCapState *s = &(MIPI_HCI(opaque)->ext_cap);

    trace_hci_ext_read(DEVICE(opaque)->canonical_path, offset,
                       s->ext_capabilities[offset / 4]);

    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < s->num_ext_capabilities);

    return s->ext_capabilities[offset];
}

void hci_ext_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    /* RO */
  g_autofree char *path = object_get_canonical_path(OBJECT(opaque));
  qemu_log_mask(LOG_GUEST_ERROR, "%s: Write of %.8" PRIx64 " to %.8" HWADDR_PRIx
                " to read-only extended capability registers\n",
                path, value, offset);
}

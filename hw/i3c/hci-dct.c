/*
 * MIPI HCI I3C controller DCT functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"
#include "hw/i3c/hci-dct.h"
#include "hci-dct-internal.h"

void hci_dct_reset(HCIDCTState *s, uint32_t num_regs)
{
    memset(&s->regs, 0, num_regs * sizeof(s->regs[0] * HCI_DCT_ENTRY_SIZE));
}

uint64_t hci_dct_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDCTState *s = &(MIPI_HCI(opaque)->dct);

    trace_hci_dct_read(DEVICE(opaque)->canonical_path, offset,
                       s->regs[offset / 4]);

    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    return s->regs[offset];
}

void hci_dct_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    g_autofree char *path = object_get_canonical_path(OBJECT(opaque));
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Write of %.8" HWADDR_PRIx " to RO DCT"
                  " entry at %.2" PRIx64, path, offset, value);
}

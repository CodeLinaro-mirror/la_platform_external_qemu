/*
 * MIPI HCI I3C controller DAT functionality
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
#include "hw/i3c/hci-dat.h"
#include "hci-dat-internal.h"

#define TARGET_DAT_RO_MASK 0x1ff80f80

uint64_t hci_dat_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDATState *s = &(MIPI_HCI(opaque)->dat);
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    return s->regs[offset];
}

void hci_dat_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    HCIDATState *s = &(MIPI_HCI(opaque)->dat);
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    /* Even numbered offsets are TARGET_DAT_n and have RO bits. */
    if (!(offset % 2)) {
        value &= ~TARGET_DAT_RO_MASK;
    }
    s->regs[offset] = value;
}

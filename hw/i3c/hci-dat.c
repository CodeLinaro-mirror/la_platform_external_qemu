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

void hci_dat_reset(HCIDATState *s, uint32_t num_regs)
{
    memset(&s->regs, 0, num_regs * sizeof(s->regs[0]) * HCI_DAT_ENTRY_SIZE);
}

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

uint32_t hci_dat_dev_index_from_addr(MIPIHCIState *hci, uint8_t addr)
{
    /*
     * In theory this lookup isn't very efficient, but in practice the DAT
     * tables are small enough that it's good enough.
     */
    for (int i = 0; i < hci->core.cfg.dat_table_size; i += HCI_DAT_ENTRY_SIZE) {
        if (FIELD_EX32(hci->dat.regs[i + R_TARGET_DAT], TARGET_DAT,
                       TARGET_DYNAMIC_ADDRESS) == addr) {
            return i;
        }
    }

    return HCI_DAT_DEV_NOT_FOUND;
}

/*
 * MIPI HCI I3C controller DMA functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/hci-dma.h"
#include "hci-dma-internal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"

static const uint32_t hci_dma_header_ro_mask[] = {
    [R_RHS_CONTROL] = 0xfffffff0,
    [R_RH0_OFFSET]  = 0xffffffff,
};

static const uint32_t hci_dma_ro_mask[HCI_DMA_NUM_REGS] = {
    [R_CR_SETUP]              = 0xffffff00,
    [R_IBI_SETUP]             = 0xff00e000,
    [R_RH_INTR_STATUS]        = 0xffffe180,
    [R_RH_INTR_STATUS_ENABLE] = 0xffffe180,
    [R_RH_INTR_SIGNAL_ENABLE] = 0xffffe180,
    [R_RH_INTR_FORCE]         = 0xffffe180,
    [R_RH_STATUS]             = 0xffffffff,
    [R_RH_CONTROL]            = 0xfffffff8,
    [R_RH_OPERATION1]         = 0xff000000,
    [R_RH_OPERATION2]         = 0xffffffff,
};

void hci_dma_reset(HCIDMAState *s)
{
    /* Header */
    s->header_regs[R_RHS_CONTROL] = 0x02050010;
    for (int i = 0; i < s->cfg.num_ring_offsets; ++i) {
        s->header_regs[R_RH0_OFFSET + i] = s->cfg.ring_offsets[i];
    }
    /* Ring status */
    memset(s->regs, 0, sizeof(s->regs));
    ARRAY_FIELD_DP32(s->regs, RHS_CONTROL, PREAMBLE_SIZE, s->cfg.preamble_size);
    ARRAY_FIELD_DP32(s->regs, RHS_CONTROL, HEADER_SIZE, s->cfg.header_size);
    ARRAY_FIELD_DP32(s->regs, CR_SETUP, XFER_STRUCT_SIZE,
                     s->cfg.xfer_struct_size);
    ARRAY_FIELD_DP32(s->regs, CR_SETUP, RESP_STRUCT_SIZE,
                     s->cfg.resp_struct_size);
    ARRAY_FIELD_DP32(s->regs, IBI_SETUP, IBI_STATUS_STRUCT_SIZE,
                     s->cfg.ibi_status_struct_size);
}

uint64_t hci_dma_header_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
    offset /= sizeof(*s->header_regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->header_regs));

    return s->header_regs[offset];
}

void hci_dma_header_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
    offset /= sizeof(*s->header_regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->header_regs));

    value &= ~hci_dma_header_ro_mask[offset];
    s->header_regs[offset] = value;
}

uint64_t hci_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    return s->regs[offset];
}

void hci_dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    HCIDMAState *s = &(MIPI_HCI(opaque)->dma);
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    value &= ~hci_dma_ro_mask[offset];
    s->regs[offset] = value;
}

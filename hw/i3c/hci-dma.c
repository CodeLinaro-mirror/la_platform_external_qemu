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

static bool hci_dma_ring_empty(HCIDMAState *s)
{
    return ARRAY_FIELD_EX32(s->regs, RH_OPERATION1, CR_ENQ_PTR) ==
           ARRAY_FIELD_EX32(s->regs, RH_OPERATION2, CR_DEQ_PTR);
}

static bool hci_dma_can_xfer(HCIDMAState *s)
{
    return ARRAY_FIELD_EX32(s->regs, RH_STATUS, RING_ENABLED) &&
           ARRAY_FIELD_EX32(s->regs, RH_STATUS, RING_RUNNING) &&
           !ARRAY_FIELD_EX32(s->regs, RH_STATUS, RING_ABORTED);
}

static bool hci_dma_ring_ok(HCIDMAState *s)
{
    bool ok = !hci_dma_ring_empty(s) &&
            (ARRAY_FIELD_EX32(s->regs, CR_SETUP, RING_SIZE) >= 2);

    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Attempted to start DMA transfer "
                      "when the ring wasn't set up correctly.", __func__);
    }
    return ok;
}

static void hci_dma_xfer(HCIDMAState *s)
{
    if (!hci_dma_can_xfer(s) || !hci_dma_ring_ok(s)) {
        return;
    }

    /* TODO: Read from the ring and execute transfers. */
}

static void hci_dma_rh_control_w(HCIDMAState *s, uint32_t val)
{
    uint32_t prev_val = s->regs[R_RH_CONTROL];
    s->regs[R_RH_CONTROL] = val;

    /* RH_CONTROL.ENABLED transitioning from 0 to 1 clears pointer states. */
    if (FIELD_EX32(prev_val, RH_CONTROL, RING_ENABLED) == 0 &&
        FIELD_EX32(val, RH_CONTROL, RING_ENABLED)) {
        s->regs[R_RH_OPERATION1] = 0;
        s->regs[R_RH_OPERATION2] = 0;
        s->regs[R_CHUNK_CONTROL] = 0;
    }

    /* Update RING_OP_STAT since we start running or got aborted. */
    if (FIELD_EX32(prev_val, RH_CONTROL, RING_RS) == 0 &&
        FIELD_EX32(val, RH_CONTROL, RING_RS)) {
        ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, RING_OP_STAT, 1);
    }
    if (FIELD_EX32(prev_val, RH_CONTROL, RING_ABORT) == 0 &&
        FIELD_EX32(val, RH_CONTROL, RING_ABORT)) {
        ARRAY_FIELD_DP32(s->regs, RH_INTR_STATUS, RING_OP_STAT, 1);
    }

    ARRAY_FIELD_DP32(s->regs, RH_STATUS, RING_ENABLED,
                     FIELD_EX32(val, RH_CONTROL, RING_ENABLED));
    ARRAY_FIELD_DP32(s->regs, RH_STATUS, RING_RUNNING,
                     FIELD_EX32(val, RH_CONTROL, RING_RS));
    ARRAY_FIELD_DP32(s->regs, RH_STATUS, RING_ABORTED,
                     FIELD_EX32(val, RH_CONTROL, RING_ABORT));

    hci_dma_xfer(s);
}

static void hci_dma_rh_operation1_w(HCIDMAState *s, uint32_t val)
{
    uint32_t prev_val = s->regs[R_RH_OPERATION1];
    s->regs[R_RH_OPERATION1] = val;

    /* Attempt to transfer if the enqueue pointer was updated. */
    if (FIELD_EX32(prev_val, RH_OPERATION1, CR_ENQ_PTR) <
        FIELD_EX32(val, RH_OPERATION1, CR_ENQ_PTR)) {
        hci_dma_xfer(s);
    }
}

static void hci_dma_rh_intr_status_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    hci->dma.regs[R_RH_INTR_STATUS] &= ~val; /* W1C */
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
}

static void hci_dma_rh_intr_force_w(MIPIHCIState *hci, uint32_t val)
{
    MIPIHCIClass *c = MIPI_HCI_GET_CLASS(hci);

    /*
     * Set the interrupt status. If it's not masked, it will be cleared during
     * IRQ updating.
     */
    hci->dma.regs[R_RH_INTR_STATUS] = val;
    c->update_irq(hci, MIPI_HCI_IRQ_CONTEXT_DMA);
}

void hci_dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    MIPIHCIState *hci = MIPI_HCI(opaque);
    HCIDMAState *s = &hci->dma;
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    value &= ~hci_dma_ro_mask[offset];
    uint32_t val32 = (uint32_t)value;

    switch (offset) {
    case R_RH_CONTROL:
        hci_dma_rh_control_w(s, val32);
        break;
    case R_RH_OPERATION1:
        hci_dma_rh_operation1_w(s, val32);
        break;
    case R_RH_INTR_STATUS:
        hci_dma_rh_intr_status_w(hci, val32);
        break;
    case R_RH_INTR_FORCE:
        hci_dma_rh_intr_force_w(hci, val32);
        break;
    default:
        s->regs[offset] = val32;
        break;
    }
}

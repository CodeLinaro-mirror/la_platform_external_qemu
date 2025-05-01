/*
 * MIPI HCI I3C controller core functionality
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/hci-core.h"
#include "hci-core-internal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"
#include "hci-dat-internal.h"
#include "hci-dct-internal.h"
#include "hci-dma-internal.h"

static const uint32_t hci_core_ro_mask[HCI_CORE_NUM_REGS] = {
    [R_HCI_VERSION]                 = 0xffffffff,
    [R_HC_CONTROL]                  = 0x0000ec3e,
    [R_CONTROLLER_DEVICE_ADDR]      = 0x7f00ffff,
    [R_HC_CAPABILITIES]             = 0xffffffff,
    [R_RESET_CONTROL]               = 0xffffffc0,
    [R_PRESENT_STATE]               = 0xffffffff,
    [R_INTR_STATUS]                 = 0xffffc3ff,
    [R_INTR_STATUS_ENABLE]          = 0xffffc3ff,
    [R_INTR_SIGNAL_ENABLE]          = 0xffffc3ff,
    [R_INTR_FORCE]                  = 0xffffc3ff,
    [R_DAT_SECTION_OFFSET]          = 0xffffffff,
    [R_DCT_SECTION_OFFSET]          = 0xff87ffff,
    [R_RING_HEADERS_SECTION_OFFSET] = 0xffff0000,
    [R_PIO_SECTION_OFFSET]          = 0xffffffff,
    [R_EXT_CAPS_SECTION_OFFSET]     = 0xffffffff,
    [R_INT_CTRL_CMDS_EN]            = 0xffffffff,
    [R_IBI_NOTIFY_CTRL]             = 0xfffffff4,
    [R_DEV_CTX_SG]                  = 0x7fff0000,
};

bool hci_core_can_xfer(HCICoreState *s)
{
    return !s->halted && ARRAY_FIELD_EX32(s->regs, HC_CONTROL, ENABLE);
}

void hci_core_reset(HCICoreState *s)
{
    memset(&s->regs, 0, sizeof(s->regs));
    ARRAY_FIELD_DP32(s->regs, RING_HEADERS_SECTION_OFFSET,
                     RING_HEADER_SECTION_OFFSET,
                     s->cfg.ring_header_section_offset);
    ARRAY_FIELD_DP32(s->regs, PIO_SECTION_OFFSET, PIO_SECTION_OFFSET,
                     s->cfg.pio_offset);
    ARRAY_FIELD_DP32(s->regs, EXT_CAPS_SECTION_OFFSET,
                     EXT_CAPS_SECTION_OFFSET, s->cfg.ext_caps_section_offset);
    ARRAY_FIELD_DP32(s->regs, DAT_SECTION_OFFSET, DAT_TABLE_OFFSET,
                     s->cfg.dat_table_offset);
    ARRAY_FIELD_DP32(s->regs, DAT_SECTION_OFFSET, DAT_TABLE_SIZE,
                     s->cfg.dat_table_size);
    ARRAY_FIELD_DP32(s->regs, DCT_SECTION_OFFSET, DCT_TABLE_OFFSET,
                     s->cfg.dct_table_offset);
    ARRAY_FIELD_DP32(s->regs, DCT_SECTION_OFFSET, DCT_TABLE_SIZE,
                     s->cfg.dct_table_size);
    s->regs[R_HCI_VERSION] = s->cfg.hci_version;
    s->regs[R_HC_CAPABILITIES] = s->cfg.hc_capabilities;
    s->regs[R_INT_CTRL_CMDS_EN] = s->cfg.int_ctrl_cmds_en;
    /* We only support 1 controller per bus, so this is always true. */
    ARRAY_FIELD_DP32(s->regs, PRESENT_STATE, CURRENT_CONTROLLER, 1);

    s->halted = false;
}

uint64_t hci_core_read(void *opaque, hwaddr offset, unsigned size)
{
    HCICoreState *s = &(MIPI_HCI(opaque)->core);
    offset /= sizeof(*s->regs);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    return s->regs[offset];
}

static void hci_core_xfer(MIPIHCIState *hci)
{
    if (ARRAY_FIELD_EX32(hci->core.regs, HC_CONTROL, MODE_SELECTOR) ==
        MODE_SELECTOR_PIO) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_UNIMP, "%s: PIO mode is not supported\n", path);
    } else {
        hci_dma_xfer(hci);
    }
}

static void hci_core_hc_control_w(MIPIHCIState *hci, uint32_t val)
{
    HCICoreState *s = &hci->core;
    uint32_t new_mode_selector = FIELD_EX32(val, HC_CONTROL, MODE_SELECTOR);
    uint32_t old_mode_selector = ARRAY_FIELD_EX32(s->regs,
                                                  HC_CONTROL, MODE_SELECTOR);

    if (new_mode_selector != old_mode_selector &&
        ARRAY_FIELD_EX32(s->regs, HC_CONTROL, ENABLE)) {
        g_autofree char *path = object_get_canonical_path(OBJECT(hci));
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Cannot change between PIO and DMA "
                     "modes when controller is enabled.\n", path);
        val = FIELD_DP32(val, HC_CONTROL, MODE_SELECTOR, 0);
    }

    if (FIELD_EX32(val, HC_CONTROL, RESUME)) {
        s->halted = false;
        val = FIELD_DP32(val, HC_CONTROL, RESUME, 0); /* W1C */
        hci_core_xfer(hci);
    }
    s->regs[R_HC_CONTROL] = val;
}

static void hci_core_reset_control_w(MIPIHCIState *hci, uint32_t val)
{
    if (FIELD_EX32(val, RESET_CONTROL, SOFT_RST)) {
        hci_core_reset(&hci->core);
        hci_dma_reset(&hci->dma);
        hci_dat_reset(&hci->dat, hci->core.cfg.dat_table_size);
        hci_dct_reset(&hci->dct, hci->core.cfg.dct_table_size);
    }
}

void hci_core_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    MIPIHCIState *hci = MIPI_HCI(opaque);
    HCICoreState *s = &hci->core;
    offset /= sizeof(*s->regs);
    uint32_t val32 = (uint32_t)value;

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->regs));

    val32 &= ~hci_core_ro_mask[offset];
    switch (offset) {
    case R_HC_CONTROL:
        hci_core_hc_control_w(hci, val32);
        break;
    case R_RESET_CONTROL:
        hci_core_reset_control_w(hci, val32);
        break;
    default:
        s->regs[offset] = val32;
        break;
    }
}

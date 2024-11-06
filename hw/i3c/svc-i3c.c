/*
 * Silvaco I3C Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/svc-i3c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "trace.h"

REG32(MCONFIG, 0x00)
    FIELD(MCONFIG, I2CBAUD, 28, 3)
    FIELD(MCONFIG, ODHPP,   24, 1)
    FIELD(MCONFIG, ODBAUD,  16, 8)
    FIELD(MCONFIG, PPLOW,   12, 4)
    FIELD(MCONFIG, PPBAUD,  8, 4)
    FIELD(MCONFIG, ODSTOP,  6, 1)
    FIELD(MCONFIG, DISTO,   3, 1)
    FIELD(MCONFIG, MSTENA,  0, 2)
REG32(MCTRL, 0x84)
    FIELD(MCTRL, RDTERM,  16, 8)
    FIELD(MCTRL, ADDR,    9, 7)
    FIELD(MCTRL, DIR,     8, 1)
    FIELD(MCTRL, IBIRESP, 6, 2)
    FIELD(MCTRL, TYPE,    4, 2)
    FIELD(MCTRL, REQUEST, 0, 3)
REG32(MSTATUS, 0x88)
    FIELD(MSTATUS, IBIADDR,   24, 7)
    FIELD(MSTATUS, NOWMASTER, 19, 1)
    FIELD(MSTATUS, ERRWARN,   15, 1)
    FIELD(MSTATUS, IBIWON,    13, 1)
    FIELD(MSTATUS, TXNOTFULL, 12, 1)
    FIELD(MSTATUS, RXPEND,    11, 1)
    FIELD(MSTATUS, COMPLETE,  10, 1)
    FIELD(MSTATUS, MCTRLDONE, 9, 1)
    FIELD(MSTATUS, SLVSTART,  8, 1)
    FIELD(MSTATUS, IBITYPE,   6, 2)
    FIELD(MSTATUS, NACKED,    5, 1)
    FIELD(MSTATUS, BETWEEN,   4, 1)
    FIELD(MSTATUS, STATE,     0, 3)
REG32(IBIRULES, 0x8c)
    FIELD(IBIRULES, NOBYTE, 31, 1)
    FIELD(IBIRULES, MSB0,   30, 1)
    FIELD(IBIRULES, ADDR4,  24, 6)
    FIELD(IBIRULES, ADDR3,  18, 6)
    FIELD(IBIRULES, ADDR2,  12, 6)
    FIELD(IBIRULES, ADDR1,  6, 6)
    FIELD(IBIRULES, ADDR0,  0, 6)
REG32(MINTSET, 0x90)
    FIELD(MINTSET, NOWMASTER, 19, 1)
    FIELD(MINTSET, ERRWARN,   15, 1)
    FIELD(MINTSET, IBIWON,    13, 1)
    FIELD(MINTSET, TXNOTFULL, 12, 1)
    FIELD(MINTSET, RXPEND,    11, 1)
    FIELD(MINTSET, COMPLETE,  10, 1)
    FIELD(MINTSET, MCTRLDONE, 9, 1)
    FIELD(MINTSET, SLVSTART,  8, 1)
REG32(MINTCLR, 0x94)
    FIELD(MINTCLR, NOWMASTER, 19, 1)
    FIELD(MINTCLR, ERRWARN,   15, 1)
    FIELD(MINTCLR, IBIWON,    13, 1)
    FIELD(MINTCLR, TXNOTFULL, 12, 1)
    FIELD(MINTCLR, RXPEND,    11, 1)
    FIELD(MINTCLR, COMPLETE,  10, 1)
    FIELD(MINTCLR, MCTRLDONE, 9, 1)
    FIELD(MINTCLR, SLVSTART,  8, 1)
REG32(MINTMASKED, 0x98)
    FIELD(MINTMASKED, NOWMASTER, 19, 1)
    FIELD(MINTMASKED, ERRWARN,   15, 1)
    FIELD(MINTMASKED, IBIWON,    13, 1)
    FIELD(MINTMASKED, TXNOTFULL, 12, 1)
    FIELD(MINTMASKED, RXPEND,    11, 1)
    FIELD(MINTMASKED, COMPLETE,  10, 1)
    FIELD(MINTMASKED, MCTRLDONE, 9, 1)
    FIELD(MINTMASKED, SLVSTART,  8, 1)
REG32(MERRWARN, 0x9c)
    FIELD(MERRWARN, TIMEOUT, 20, 1)
    FIELD(MERRWARN, INVREQ,  19, 1)
    FIELD(MERRWARN, MSGERR,  18, 1)
    FIELD(MERRWARN, OWRITE,  17, 1)
    FIELD(MERRWARN, OREAD,   16, 1)
    FIELD(MERRWARN, TERM,    4, 1)
    FIELD(MERRWARN, WRABT,   3, 1)
    FIELD(MERRWARN, NACK,    2, 1)
REG32(MDMACTRL, 0xa0)
    FIELD(MDMACTRL, DMAWIDTH, 4, 2)
    FIELD(MDMACTRL, DMATB,    2, 2)
    FIELD(MDMACTRL, DMAFB,    0, 2)
REG32(MDATACTRL, 0xac)
    FIELD(MDATACTRL, RXEMPTY, 31, 1)
    FIELD(MDATACTRL, TXFULL,  30, 1)
    FIELD(MDATACTRL, RXCOUNT, 24, 5)
    FIELD(MDATACTRL, TXCOUNT, 16, 5)
    FIELD(MDATACTRL, RXTRIG,  6, 2)
    FIELD(MDATACTRL, TXTRIG,  4, 2)
    FIELD(MDATACTRL, UNLOCK,  3, 1)
    FIELD(MDATACTRL, FLUSHFB, 1, 1)
    FIELD(MDATACTRL, FLUSHTB, 0, 1)
REG32(MWDATAB, 0xb0)
    FIELD(MWDATAB, END_A, 16, 1)
    FIELD(MWDATAB, END_B, 8, 1)
    FIELD(MWDATAB, DATA,  0, 8)
REG32(MWDATABE, 0xb4)
    FIELD(MWDATABE, DATA, 0, 8)
REG32(MWDATAH, 0xb8)
    FIELD(MWDATAH, END,   16, 1)
    FIELD(MWDATAH, DATA1, 8, 8)
    FIELD(MWDATAH, DATA0, 0, 8)
REG32(MWDATAHE, 0xbc)
    FIELD(MWDATAHE, DATA1, 8, 8)
    FIELD(MWDATAHE, DATA0, 0, 8)
REG32(MRDATAB, 0xc0)
    FIELD(MRDATAB, DATA, 0, 8)
REG32(MRDATAH, 0xc8)
    FIELD(MRDATAH, DATA1, 8, 8)
    FIELD(MRDATAH, DATA0, 0, 8)
REG32(MWMSG_SDR, 0xd0)
    FIELD(MWMSG_SDR, LEN,  11, 5)
    FIELD(MWMSG_SDR, I2C,  10, 1)
    FIELD(MWMSG_SDR, END,  8, 1)
    FIELD(MWMSG_SDR, ADDR, 1, 7)
    FIELD(MWMSG_SDR, DIR,  0, 1)
    FIELD(MWMSG_SDR, DATA, 0, 16)
REG32(MRMSG_SDR, 0xd4)
    FIELD(MRMSG_SDR, DATA, 0, 16)

static const uint32_t svc_i3c_resets[SVC_I3C_NR_REGS] = {
    [R_MCONFIG] =   0x00000030,
    [R_MSTATUS] =   0x00001000,
    [R_MDMACTRL] =  0x00000010,
    [R_MDATACTRL] = 0x80000030,
};

static const uint32_t svc_i3c_ro[SVC_I3C_NR_REGS] = {
    [R_MSTATUS]    = 0x0e0000b4,
    [R_MCTRL]      = 0xff000080,
    [R_MSTATUS]    = 0xfff7d8ff,
    [R_MINTSET]    = 0xfff740ff,
    [R_MINTCLR]    = 0xfff740ff,
    [R_MINTMASKED] = 0xffffffff,
    [R_MERRWARN]   = 0xff70f9e3,
    [R_MDMACTRL]   = 0xffffffc0,
    [R_MDATACTRL]  = 0xffffff00,
    [R_MWDATAB]    = 0xfffefe00,
    [R_MWDATABE]   = 0xffffff00,
    [R_MWDATAH]    = 0xfffe0000,
    [R_MWDATAHE]   = 0xffff0000,
    [R_MRDATAB]    = 0xffffff00,
    [R_MRDATAH]    = 0xffff0000,
    [R_MWMSG_SDR]  = 0xffff0000,
    [R_MRMSG_SDR]  = 0xffff0000,
};

static inline bool svc_i3c_is_enabled(SVCI3C *s)
{
    return ARRAY_FIELD_EX32(s->regs, MCONFIG, MSTENA);
}

static inline bool svc_i3c_tx_in_progress(SVCI3C *s)
{
    return ARRAY_FIELD_EX32(s->regs, MSTATUS, STATE) == SVC_I3C_STATE_NORM_ACT;
}

static void svc_i3c_update_irq(SVCI3C *s)
{
    s->regs[R_MINTMASKED] = s->regs[R_MSTATUS] & s->regs[R_MINTSET];
    bool level = !!(s->regs[R_MINTMASKED]);

    qemu_set_irq(s->irq, level);
}

static void svc_i3c_merrwarn_update(SVCI3C *s, uint32_t mask)
{
    s->regs[R_MERRWARN] |= mask;
    ARRAY_FIELD_DP32(s->regs, MSTATUS, ERRWARN, 1);
    svc_i3c_update_irq(s);
}

static void svc_i3c_merrwarn_clear(SVCI3C *s, uint32_t mask)
{
    s->regs[R_MERRWARN] &= ~mask;
    if (s->regs[R_MERRWARN] == 0) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, ERRWARN, 0);
        svc_i3c_update_irq(s);
    }
}

static uint32_t svc_i3c_rxtrig_threshold(SVCI3C *s)
{
    switch (ARRAY_FIELD_EX32(s->regs, MDATACTRL, RXTRIG)) {
    /* Trigger when not empty. */
    case 0:
        return 1;
    /* Trigger when 1/4 full. */
    case 1:
        return SVC_I3C_FIFO_SIZE / 4;
    /* Trigger when 1/2 full. */
    case 2:
        return SVC_I3C_FIFO_SIZE / 2;
    /* Trigger when 3/4 full. */
    case 3:
        return SVC_I3C_FIFO_SIZE * 3 / 4;
    default:
        g_assert_not_reached();
     }
}

static uint32_t svc_i3c_mdatactrl_r(SVCI3C *s)
{
    uint32_t val = s->regs[R_MDATACTRL];

    val = FIELD_DP32(val, MDATACTRL, RXEMPTY, fifo8_is_empty(&s->rx_fifo));
    val = FIELD_DP32(val, MDATACTRL, TXFULL, fifo8_is_full(&s->tx_fifo));
    val = FIELD_DP32(val, MDATACTRL, RXCOUNT, fifo8_num_used(&s->rx_fifo));
    val = FIELD_DP32(val, MDATACTRL, TXCOUNT, fifo8_num_used(&s->tx_fifo));
    return val;
}

static void svc_i3c_rxfifo_pop(SVCI3C *s, uint8_t *buf, int num_bytes)
{
    uint32_t trig_threshold = svc_i3c_rxtrig_threshold(s);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    for (int i = 0; i < num_bytes; i++) {
        if (fifo8_is_empty(&s->rx_fifo)) {
            svc_i3c_merrwarn_update(s, R_MERRWARN_OREAD_MASK);
            break;
        }

        buf[i] = fifo8_pop(&s->rx_fifo);
        trace_svc_i3c_rxfifo_pop(path, buf[i]);
        if (fifo8_num_used(&s->rx_fifo) < trig_threshold) {
            ARRAY_FIELD_DP32(s->regs, MSTATUS, RXPEND, 0);
        }
    }

    svc_i3c_update_irq(s);
}

static uint32_t svc_i3c_mrdatab_r(SVCI3C *s)
{
    uint8_t val = 0;

    svc_i3c_rxfifo_pop(s, &val, sizeof(val));
    return val;
}

static uint32_t svc_i3c_mrdatah_r(SVCI3C *s)
{
    uint16_t word = 0;

    svc_i3c_rxfifo_pop(s, (uint8_t *)&word, sizeof(word));
    return word;
}

static uint32_t svc_i3c_mrmsg_sdr_r(SVCI3C *s)
{
    uint16_t word = 0;
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    if (!s->mwmsg_xfer_in_progress) {
        return 0;
    }

    for (int i = 0; i < sizeof(word); i++) {
        if (s->mwmsg_xfer.len == 0) {
            break;
        }

        if (s->mwmsg_xfer.is_i2c) {
            word |= legacy_i2c_recv(s->bus);
        } else {
            uint32_t bytes_received;
            if (i3c_recv(s->bus, (uint8_t *)&word, sizeof(uint8_t),
                         &bytes_received)) {
                ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
                svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
                return word;
            }
        }
        word <<= 8;
        s->mwmsg_xfer.len--;
    }

    if (s->mwmsg_xfer.len == 0) {
        s->mwmsg_xfer_in_progress = false;
        ARRAY_FIELD_DP32(s->regs, MSTATUS, COMPLETE, 1);
        if (s->mwmsg_xfer.end_with_stop) {
            if (s->mwmsg_xfer.is_i2c) {
                legacy_i2c_end_transfer(s->bus);
                trace_svc_i3c_end_transfer(path);
            }
        }
    }

    trace_svc_i3c_recv(path, sizeof(word));
    return word;
}

static uint64_t svc_i3c_read(void *opaque, hwaddr offset, unsigned size)
{
    SVCI3C *s = SVC_I3C(opaque);
    uint32_t addr = offset >> 2;
    uint32_t value;
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    switch (addr) {
    case R_MDATACTRL:
        value = svc_i3c_mdatactrl_r(s);
        break;
    case R_MRDATAB:
        value = svc_i3c_mrdatab_r(s);
        break;
    case R_MRDATAH:
        value = svc_i3c_mrdatah_r(s);
        break;
    case R_MRMSG_SDR:
        value = svc_i3c_mrmsg_sdr_r(s);
        break;
    default:
        value = s->regs[addr];
    }

    trace_svc_i3c_read(path, offset, value);
    return value;
}

static void svc_i3c_rxfifo_push(SVCI3C *s, const uint8_t *buf, size_t num_bytes)
{
    uint32_t trig_threshold = svc_i3c_rxtrig_threshold(s);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    for (int i = 0; i < num_bytes; i++) {
        if (fifo8_is_full(&s->rx_fifo)) {
            break;
        }

        fifo8_push(&s->rx_fifo, buf[i]);
        trace_svc_i3c_rxfifo_push(path, buf[i]);
        if (fifo8_num_used(&s->rx_fifo) >= trig_threshold) {
            ARRAY_FIELD_DP32(s->regs, MSTATUS, RXPEND, 1);
        }
    }

    svc_i3c_update_irq(s);
}

static int svc_i3c_ibi_handle(I3CBus *bus, uint8_t addr, bool is_recv)
{
    SVCI3C *s = SVC_I3C(bus->qbus.parent);
    int ret = 0;
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    /* Update our status to say we have an IBI. */
    ARRAY_FIELD_DP32(s->regs, MSTATUS, STATE, SVC_I3C_STATE_SLV_REQ);
    ARRAY_FIELD_DP32(s->regs, MSTATUS, IBIWON, 1);
    ARRAY_FIELD_DP32(s->regs, MSTATUS, SLVSTART, 1);

    ARRAY_FIELD_DP32(s->regs, MSTATUS, IBIADDR, addr);
    if (addr == I3C_HJ_ADDR) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, IBITYPE, SVC_I3C_IBI_TYPE_HJ);
    } else if (is_recv) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, IBITYPE, SVC_I3C_IBI_TYPE_IBI);
    } else {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, IBITYPE, SVC_I3C_IBI_TYPE_MR);
    }

    /* If we're auto-responding, respond based on MCTRL.IBIRESP. */
    if (ARRAY_FIELD_EX32(s->regs, MCTRL, REQUEST) == SVC_I3C_REQUEST_AUTO_IBI) {
        /*
         * Only NACK if we're configured to do so. Otherwise ACK.
         * In the case of a manual ACK/NACK, we will ACK/NACK in ibi_recv or
         * ibi_finish.
         */
        ret = (ARRAY_FIELD_EX32(s->regs, MCTRL, IBIRESP) ==
               SVC_I3C_IBI_RESP_NACK);
    } else if (ARRAY_FIELD_EX32(s->regs, MCTRL, IBIRESP) ==
               SVC_I3C_IBI_RESP_MANUAL) {
        /*
         * In this mode, it is expected that the driver IRQ handler ACKs or
         * NACks the incoming IBI, rather than the controller doing it
         * automatically. However, the entire IBI happens atomically, so by the
         * time the guest is able to service the IRQ, the IBI would have
         * finished.
         */
        qemu_log_mask(LOG_UNIMP, "%s: Manual IBI ACKing/NACKing is "
                      "unsupported. NACKing.", path);
        ret = -1;
    }

    trace_svc_i3c_ibi(path, addr, is_recv, ret == 0);
    svc_i3c_update_irq(s);
    return ret;
}

static int svc_i3c_ibi_recv(I3CBus *bus, uint8_t data)
{
    SVCI3C *s = SVC_I3C(bus->qbus.parent);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    /*
     * Check IBIRULES to determine if we should ACK/NACK, if MCTRL.IBIRESP is
     * MDB-aware.
     */
    if (ARRAY_FIELD_EX32(s->regs, MCTRL, IBIRESP) ==
        SVC_I3C_IBI_RESP_ACK_WITH_MDB_CHECK) {
        if (ARRAY_FIELD_EX32(s->regs, IBIRULES, NOBYTE)) {
            return -1;
        }
    }

    trace_svc_i3c_ibi_recv(path, data);
    svc_i3c_rxfifo_push(s, &data, sizeof(data));

    return 0;
}

static int svc_i3c_ibi_finish(I3CBus *bus)
{
    SVCI3C *s = SVC_I3C(bus->qbus.parent);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    if (ARRAY_FIELD_EX32(s->regs, MCTRL, REQUEST) == SVC_I3C_REQUEST_AUTO_IBI) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, COMPLETE, 1);
    }

    trace_svc_i3c_ibi_finish(path);
    return 0;
}

static void svc_i3c_end_transfer(SVCI3C *s)
{
    bool is_i2c = ARRAY_FIELD_EX32(s->regs, MCTRL, TYPE);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    if (is_i2c) {
        legacy_i2c_end_transfer(s->bus);
    } else {
        i3c_end_transfer(s->bus);
    }

    ARRAY_FIELD_DP32(s->regs, MSTATUS, STATE, SVC_I3C_STATE_IDLE);
    ARRAY_FIELD_DP32(s->regs, MSTATUS, COMPLETE, 1);

    /* If this was a 1-shot DMA transfer, clear the DMA bit. */
    if (ARRAY_FIELD_EX32(s->regs, MDMACTRL, DMATB) == 1) {
        ARRAY_FIELD_DP32(s->regs, MDMACTRL, DMATB, 0);
    }
    if (ARRAY_FIELD_EX32(s->regs, MDMACTRL, DMAFB) == 1) {
        ARRAY_FIELD_DP32(s->regs, MDMACTRL, DMAFB, 0);
    }

    s->in_entdaa = false;
    trace_svc_i3c_end_transfer(path);
}

static uint32_t svc_i3c_txtrig_threshold(SVCI3C *s)
{

    switch (ARRAY_FIELD_EX32(s->regs, MDATACTRL, TXTRIG)) {
    /* Trigger when empty. */
    case 0:
        return 0;
    /* Trigger when 1/4 full. */
    case 1:
        return SVC_I3C_FIFO_SIZE / 4;
    /* Trigger when 1/2 full. */
    case 2:
        return SVC_I3C_FIFO_SIZE / 2;
    /* Trigger when FIFO_SIZE - 1 */
    case 3:
        return SVC_I3C_FIFO_SIZE  - 1;
    default:
        g_assert_not_reached();
    }
}

static uint32_t svc_i3c_txfifo_pop(SVCI3C *s, uint8_t *buf, uint32_t num_bytes)
{
    uint32_t trig_threshold = svc_i3c_txtrig_threshold(s);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));
    uint32_t i;

    for (i = 0; i < num_bytes; i++) {
        if (fifo8_is_empty(&s->tx_fifo)) {
            break;
        }
        buf[i] = fifo8_pop(&s->tx_fifo);
        trace_svc_i3c_txfifo_pop(path, buf[i]);

        if (fifo8_num_used(&s->tx_fifo) <= trig_threshold) {
            ARRAY_FIELD_DP32(s->regs, MSTATUS, TXNOTFULL, 0);
        }
    }

    svc_i3c_update_irq(s);
    return i;
}

static void svc_i3c_txfifo_push(SVCI3C *s, const uint8_t *buf, int num_bytes)
{
    uint32_t trig_threshold = svc_i3c_txtrig_threshold(s);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    for (int i = 0; i < num_bytes; i++) {
        if (fifo8_is_full(&s->tx_fifo)) {
            svc_i3c_merrwarn_update(s, R_MERRWARN_OWRITE_MASK);
            break;
        }

        fifo8_push(&s->tx_fifo, buf[i]);
        trace_svc_i3c_txfifo_push(path, buf[i]);
        if (fifo8_num_used(&s->tx_fifo) <= trig_threshold) {
            ARRAY_FIELD_DP32(s->regs, MSTATUS, TXNOTFULL, 1);
        }
    }

    svc_i3c_update_irq(s);
}

static int svc_i3c_tx(SVCI3C *s)
{
    int ret = 0;
    bool is_i2c = ARRAY_FIELD_EX32(s->regs, MCTRL, TYPE);
    uint32_t xfer_size = 0;
    uint8_t i3c_buf[SVC_I3C_FIFO_SIZE];
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    if (!svc_i3c_is_enabled(s)) {
        return 0;
    }
    if (!svc_i3c_tx_in_progress(s)) {
        return 0;
    }

    if (is_i2c) {
        xfer_size = svc_i3c_txfifo_pop(s, i3c_buf, SVC_I3C_FIFO_SIZE);
        for (int i = 0; i < xfer_size; i++) {
            /* Break out if we got NACKed. */
            ret = legacy_i2c_send(s->bus, i3c_buf[i]);
            if (ret) {
                break;
            }
        }
    } else {
        uint32_t bytes_sent;

        xfer_size = svc_i3c_txfifo_pop(s, i3c_buf, SVC_I3C_FIFO_SIZE);
        ret = i3c_send(s->bus, i3c_buf, xfer_size, &bytes_sent);
        if (ret) {
            return ret;
        }
    }

    trace_svc_i3c_send(path, xfer_size);
    return ret;
}

static int svc_i3c_rx(SVCI3C *s)
{
    g_autofree char *path = object_get_canonical_path(OBJECT(s));
    bool is_i2c = ARRAY_FIELD_EX32(s->regs, MCTRL, TYPE);
    uint8_t num_bytes = ARRAY_FIELD_EX32(s->regs, MCTRL, RDTERM);
    uint8_t i3c_buf[SVC_I3C_FIFO_SIZE];
    int ret = 0;
    bool xfer_done;

    if (!svc_i3c_is_enabled(s)) {
        return 0;
    }

    if (is_i2c) {
        uint32_t num_to_read = num_bytes > SVC_I3C_FIFO_SIZE ?
            SVC_I3C_FIFO_SIZE : num_bytes;

        for (int i = 0; i < num_to_read; i++) {
            i3c_buf[i] = legacy_i2c_recv(s->bus);
        }
        svc_i3c_rxfifo_push(s, i3c_buf, num_to_read);

        num_bytes -= num_to_read;
        xfer_done = num_bytes == 0;
        trace_svc_i3c_recv(path, num_to_read);
    } else {
        uint32_t bytes_read;
        uint32_t xfer_size = SVC_I3C_FIFO_SIZE > num_bytes ? num_bytes :
                             SVC_I3C_FIFO_SIZE;
        ret = i3c_recv(s->bus, i3c_buf, xfer_size, &bytes_read);
        if (ret) {
            return ret;
        }

        svc_i3c_rxfifo_push(s, i3c_buf, bytes_read);
        num_bytes -= bytes_read;

        /*
         * The transfer is done if we've received all of the bytes we requested
         * from the target, or If the target gave us less bytes than we
         * requested from it, meaning it has nothing left to send.
         */
        xfer_done = (bytes_read < xfer_size) || (num_bytes == 0);
        trace_svc_i3c_recv(path, bytes_read);
    }

    if (xfer_done) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, COMPLETE, 1);
    }
    ARRAY_FIELD_DP32(s->regs, MCTRL, RDTERM, num_bytes);

    return ret;
}

static void svc_i3c_send_start(SVCI3C *s)
{
    g_autofree char *path = object_get_canonical_path(OBJECT(s));
    bool is_read = ARRAY_FIELD_EX32(s->regs, MCTRL, DIR);
    uint8_t addr = ARRAY_FIELD_EX32(s->regs, MCTRL, ADDR);
    bool is_i2c = ARRAY_FIELD_EX32(s->regs, MCTRL, TYPE);
    int ret = 0;

    if (!svc_i3c_is_enabled(s)) {
        return;
    }

    if (is_i2c) {
        ret = legacy_i2c_start_transfer(s->bus, addr, is_read);
    } else {
        ret = i3c_start_transfer(s->bus, addr, is_read);
    }

    /* MCTRLDONE is set regardless of an ACK or NACK. */
    ARRAY_FIELD_DP32(s->regs, MSTATUS, MCTRLDONE, 1);
    if (ret) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
        svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
    } else {
        /* MSTATUS.NACKED must be cleared by us on any START ACK. */
        ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 0);
    }

    trace_svc_i3c_start_transfer(path);
    svc_i3c_update_irq(s);
}

static void svc_i3c_do_entdaa(SVCI3C *s)
{
    g_autofree char *path = object_get_canonical_path(OBJECT(s));
    uint8_t target_data[I3C_ENTDAA_SIZE];
    uint32_t num_read;

    /*
     * if the bus isn't in ENTDAA, we send a START w/ broadcast, followed by the
     * CCC.
     */
    if (!s->in_entdaa) {
        if (i3c_start_transfer(s->bus, I3C_BROADCAST, /*is_read=*/false)) {
            ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
            svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
            return;
        }
        i3c_send_byte(s->bus, I3C_CCC_ENTDAA);
        s->in_entdaa = true;
        ARRAY_FIELD_DP32(s->regs, MSTATUS, STATE, SVC_I3C_STATE_DAA);
    }

    /*
     * If we're in the BETWEEN state, we should assign the target's address,
     * which the user should write to MWDATAB.
     * We should also send a reSTART and read the next target's PID+BCR+DCR, or
     * get NACKed if no one left on the bus needs an address.
     */
    if (ARRAY_FIELD_EX32(s->regs, MSTATUS, BETWEEN)) {
        uint8_t addr = 0;
        svc_i3c_txfifo_pop(s, &addr, sizeof(addr));
        if (i3c_send_byte(s->bus, addr)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Target NACKed address 0x%.2x"
                          "during assignment in ENTDAA.", path, addr);
            svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
        } else {
            trace_svc_i3c_addr_assign(path, addr);
        }
        ARRAY_FIELD_DP32(s->regs, MSTATUS, BETWEEN, 0);
    }

    if (i3c_start_transfer(s->bus, I3C_BROADCAST, /*is_read=*/true)) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
        /* In ENTDAA, we're completed if no one ACKs the reSTART. */
        ARRAY_FIELD_DP32(s->regs, MSTATUS, COMPLETE, 1);
        ARRAY_FIELD_DP32(s->regs, MSTATUS, STATE, SVC_I3C_STATE_IDLE);
        svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
        return;
    }

    /* If a target NACKed at this point, it's misbehaving, so log it. */
    if (i3c_recv(s->bus, target_data, I3C_ENTDAA_SIZE, &num_read)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Target ACKed ENTDAA reSTART, "
                      "but NACKed PID reading.", path);
        svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
        return;
    }

    /*
     * Similarly, it should send 8 bytes of data. If it doesn't we can move
     * along and things will most likely be fine, but we should log it.
     */
    if (num_read != I3C_ENTDAA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Target sent %d bytes during, "
                      "ENTDAA read instead of %d", path, num_read,
                      I3C_ENTDAA_SIZE);
    }
    svc_i3c_rxfifo_push(s, target_data, num_read);

    /* Set BETWEEN. This tells the user to write the target's address. */
    ARRAY_FIELD_DP32(s->regs, MSTATUS, BETWEEN, 1);
    ARRAY_FIELD_DP32(s->regs, MSTATUS, MCTRLDONE, 1);
    svc_i3c_update_irq(s);
}

static void svc_i3c_mctrl_w(SVCI3C *s, uint32_t val)
{
    bool is_read;

    s->regs[R_MCTRL] = val;
    SVCI3CRequest req = ARRAY_FIELD_EX32(s->regs, MCTRL, REQUEST);

    /* Automatically cleared on an MCTRL write. */
    svc_i3c_merrwarn_clear(s, R_MERRWARN_NACK_MASK);

    switch (req) {
    case SVC_I3C_REQUEST_NONE:
        break;
    case SVC_I3C_REQUEST_EMIT_START_ADDR:
        /* We're initiating a transfer from MCTRL, set state to NORMACT. */
        ARRAY_FIELD_DP32(s->regs, MSTATUS, STATE, SVC_I3C_STATE_NORM_ACT);
        svc_i3c_send_start(s);
        is_read = ARRAY_FIELD_EX32(s->regs, MCTRL, DIR);
        if (is_read) {
            svc_i3c_rx(s);
        } else {
            svc_i3c_tx(s);
        }
        break;
    case SVC_I3C_REQUEST_EMIT_STOP:
        svc_i3c_end_transfer(s);
        break;
    case SVC_I3C_REQUEST_IBI_ACK_NACK:
        break;
    case SVC_I3C_REQUEST_PROCESS_DAA:
        svc_i3c_do_entdaa(s);
        break;
    case SVC_I3C_REQUEST_AUTO_IBI:
        break;
    default:
        g_assert_not_reached();
    }
}

static void svc_i3c_enter_reset(Object *obj, ResetType type)
{
    SVCI3C *s = SVC_I3C(obj);

    for (size_t i = 0; i < ARRAY_SIZE(s->regs); i++) {
        s->regs[i] = svc_i3c_resets[i];
    }

    trace_svc_i3c_reset(object_get_canonical_path(obj));
}

static void svc_i3c_mintclr_w(SVCI3C *s, uint32_t val)
{
    /* Clear the corresponding bits in MINTSET. */
    s->regs[R_MINTSET] &= ~val;
    svc_i3c_update_irq(s);
}

static void svc_i3c_mintset_w(SVCI3C *s, uint32_t val)
{
    s->regs[R_MINTSET] |= val;
    svc_i3c_update_irq(s);
}

static void svc_i3c_mstatus_w(SVCI3C *s, uint32_t val)
{
    /* MSTATUS is W1C. */
    s->regs[R_MSTATUS] &= ~val;
    svc_i3c_update_irq(s);
}

static void svc_i3c_update_fifo_trigger(SVCI3C *s)
{
    uint32_t tx_threshold = svc_i3c_txtrig_threshold(s);
    uint32_t rx_threshold = svc_i3c_rxtrig_threshold(s);

    int ret;
    if (fifo8_num_used(&s->tx_fifo) <= tx_threshold) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, TXNOTFULL, 1);
        if (ARRAY_FIELD_EX32(s->regs, MDMACTRL, DMATB)) {
            ret = svc_i3c_tx(s);
            if (ret) {
                ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
                svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
            }
        }
    }
    if (fifo8_num_used(&s->rx_fifo) > rx_threshold) {
        ARRAY_FIELD_DP32(s->regs, MSTATUS, RXPEND, 1);
        if (ARRAY_FIELD_EX32(s->regs, MDMACTRL, DMAFB)) {
            ret = svc_i3c_rx(s);
            if (ret) {
                ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
                svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
            }
        }
    }

    svc_i3c_update_irq(s);
}

static void svc_i3c_merrwarn_w(SVCI3C *s, uint32_t val)
{
    /* MERRWARN is W1C. */
    svc_i3c_merrwarn_clear(s, val);
}

static void svc_i3c_mdatactrl_w(SVCI3C *s, uint32_t val)
{
    /* FIFO triggers can only be written if UNLOCK is set. */
    if (FIELD_EX32(val, MDATACTRL, UNLOCK)) {
        ARRAY_FIELD_DP32(s->regs, MDATACTRL, RXTRIG,
                         FIELD_EX32(val, MDATACTRL, RXTRIG));
        ARRAY_FIELD_DP32(s->regs, MDATACTRL, TXTRIG,
                         FIELD_EX32(val, MDATACTRL, TXTRIG));

        svc_i3c_update_fifo_trigger(s);
    }

    if (FIELD_EX32(val, MDATACTRL, FLUSHFB)) {
        fifo8_reset(&s->rx_fifo);
    }
    if (FIELD_EX32(val, MDATACTRL, FLUSHTB)) {
        fifo8_reset(&s->tx_fifo);
    }
}

static void svc_i3c_mwdatab_w(SVCI3C *s, uint32_t val)
{
    bool end = FIELD_EX32(val, MWDATAB, END_B) ||
               FIELD_EX32(val, MWDATAB, END_A);
    uint8_t byte = FIELD_EX32(val, MWDATAB, DATA);

    /*
     * If the controller is in the ENTDAA state, this write is an address being
     * assigned to a target.
     */
    if (ARRAY_FIELD_EX32(s->regs, MSTATUS, STATE) == SVC_I3C_STATE_DAA) {
        /*
         * If we're in DAA, this is the address byte to assign to the target.
         * It will be popped from the FIFO when the user continues DAA by
         * writing a process DAA request.
         */
        svc_i3c_txfifo_push(s, &byte, sizeof(byte));
        return;
    }

    svc_i3c_txfifo_push(s, &byte, sizeof(byte));
    svc_i3c_tx(s);

    /* This is the last byte, complete the transfer. */
    if (end) {
        svc_i3c_end_transfer(s);
    }
}

static void svc_i3c_mwdatabe_w(SVCI3C *s, uint32_t val)
{
    uint8_t byte = FIELD_EX32(val, MWDATABE, DATA);

    svc_i3c_txfifo_push(s, &byte, sizeof(byte));
    svc_i3c_tx(s);

    /* MWDATAxE registers always complete transfers. */
    svc_i3c_end_transfer(s);
}

static void svc_i3c_mwdatah_w(SVCI3C *s, uint32_t val)
{
    uint16_t word;
    bool end = FIELD_EX32(val, MWDATAH, END);

    word = FIELD_EX32(val, MWDATAH, DATA0);
    word <<= 8;
    word |= FIELD_EX32(val, MWDATAH, DATA1);
    svc_i3c_txfifo_push(s, (const uint8_t *)&word, sizeof(word));

    svc_i3c_tx(s);
    /* This is the last byte, complete the transfer. */
    if (end) {
        svc_i3c_end_transfer(s);
    }
}

static void svc_i3c_mwdatahe_w(SVCI3C *s, uint32_t val)
{
    uint16_t word;

    word = FIELD_EX32(val, MWDATAHE, DATA0);
    word <<= 8;
    word |= FIELD_EX32(val, MWDATAHE, DATA1);
    svc_i3c_txfifo_push(s, (const uint8_t *)&word, sizeof(word));

    svc_i3c_tx(s);
    /* MWDATAxE registers always complete transfers. */
    svc_i3c_end_transfer(s);
}

static void svc_i3c_mwmsg_sdr_w(SVCI3C *s, uint32_t val)
{
    /*
     * If there isn't a transfer in progress, this first write is the
     * information about the transfer.
     * Otherwise it's data to be transferred.
     */
    if (!s->mwmsg_xfer_in_progress) {
        s->mwmsg_xfer.len = FIELD_EX32(val, MWMSG_SDR, LEN);
        s->mwmsg_xfer.is_i2c = FIELD_EX32(val, MWMSG_SDR, I2C);
        s->mwmsg_xfer.end_with_stop = FIELD_EX32(val, MWMSG_SDR, END);
        s->mwmsg_xfer.addr = FIELD_EX32(val, MWMSG_SDR, ADDR);
        s->mwmsg_xfer.rnw = FIELD_EX32(val, MWMSG_SDR, DIR);

        svc_i3c_send_start(s);
        s->mwmsg_xfer_in_progress = true;
    } else {
        uint16_t word = FIELD_EX32(val, MWMSG_SDR, DATA);
        g_autofree char *path = object_get_canonical_path(OBJECT(s));

        /* Transmit the word. */
        for (int i = 0; i < sizeof(word); i++) {
            if (s->mwmsg_xfer.len == 0) {
                break;
            }

            uint8_t byte = word & 0xff;
            if (s->mwmsg_xfer.is_i2c) {
                if (legacy_i2c_send(s->bus, byte)) {
                    ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
                    svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
                }
                s->mwmsg_xfer.len--;
            } else {
                uint32_t num_sent;
                if (i3c_send(s->bus, &byte, sizeof(byte), &num_sent)) {
                    ARRAY_FIELD_DP32(s->regs, MSTATUS, NACKED, 1);
                    svc_i3c_merrwarn_update(s, R_MERRWARN_NACK_MASK);
                    return;
                }
            }
            trace_svc_i3c_send(path, 1);

            word >>= 8;
            s->mwmsg_xfer.len--;
        }

        /* If we're done and set up to do so, send a STOP. */
        if (s->mwmsg_xfer.len == 0) {
            s->mwmsg_xfer_in_progress = false;
            ARRAY_FIELD_DP32(s->regs, MSTATUS, COMPLETE, 1);
            if (s->mwmsg_xfer.end_with_stop) {
                svc_i3c_end_transfer(s);
            }
        }
    }
}

static void svc_i3c_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    SVCI3C *s = SVC_I3C(opaque);
    uint32_t addr = offset >> 2;
    uint32_t val32 = (uint32_t)value;
    g_autofree char *path = object_get_canonical_path(OBJECT(s));

    val32 &= ~svc_i3c_ro[addr];
    trace_svc_i3c_write(path, offset, val32);
    switch (addr) {
    case R_MSTATUS:
        svc_i3c_mstatus_w(s, val32);
        break;
    case R_MCTRL:
        svc_i3c_mctrl_w(s, val32);
        break;
    case R_MINTSET:
        svc_i3c_mintset_w(s, val32);
        break;
    case R_MINTCLR:
        svc_i3c_mintclr_w(s, val32);
        break;
    case R_MERRWARN:
        svc_i3c_merrwarn_w(s, val32);
        break;
    case R_MDATACTRL:
        svc_i3c_mdatactrl_w(s, val32);
        break;
    case R_MWDATAB:
        svc_i3c_mwdatab_w(s, val32);
        break;
    case R_MWDATABE:
        svc_i3c_mwdatabe_w(s, val32);
        break;
    case R_MWDATAH:
        svc_i3c_mwdatah_w(s, val32);
        break;
    case R_MWDATAHE:
        svc_i3c_mwdatahe_w(s, val32);
        break;
    case R_MWMSG_SDR:
        svc_i3c_mwmsg_sdr_w(s, val32);
        break;
    default:
        s->regs[addr] = val32;
        break;
    }
}

static const MemoryRegionOps svc_i3c_ops = {
    .read = svc_i3c_read,
    .write = svc_i3c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void svc_i3c_realize(DeviceState *dev, Error **errp)
{
    SVCI3C *s = SVC_I3C(dev);
    g_autofree char *name = g_strdup_printf(TYPE_SVC_I3C ".%d",
                                            s->cfg.id);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    memory_region_init_io(&s->mr, OBJECT(s), &svc_i3c_ops,
                          s, name, SVC_I3C_NR_REGS << 2);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);

    fifo8_create(&s->tx_fifo, SVC_I3C_FIFO_SIZE);
    fifo8_create(&s->rx_fifo, SVC_I3C_FIFO_SIZE);

    s->bus = i3c_init_bus(DEVICE(s), name);
    I3CBusClass *bc = I3C_BUS_GET_CLASS(s->bus);
    bc->ibi_handle = svc_i3c_ibi_handle;
    bc->ibi_recv = svc_i3c_ibi_recv;
    bc->ibi_finish = svc_i3c_ibi_finish;
}

static const Property svc_i3c_properties[] = {
    DEFINE_PROP_UINT8("device-id", SVCI3C, cfg.id, 0),
};

static void svc_i3c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Silvaco I3C Controller";
    dc->realize = svc_i3c_realize;
    device_class_set_props(dc, svc_i3c_properties);

    rc->phases.enter = svc_i3c_enter_reset;
}

static const TypeInfo svc_i3c_info = {
    .name = TYPE_SVC_I3C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SVCI3C),
    .class_init = svc_i3c_class_init,
};

static void svc_i3c_register_types(void)
{
    type_register_static(&svc_i3c_info);
}

type_init(svc_i3c_register_types);

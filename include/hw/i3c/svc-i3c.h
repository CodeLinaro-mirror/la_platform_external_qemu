/*
 * Silvaco I3C Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#ifndef SVC_I3C_H
#define SVC_I3C_H

#include "qemu/fifo32.h"
#include "hw/i3c/i3c.h"
#include "hw/irq.h"
#include "hw/sysbus.h"

#define TYPE_SVC_I3C "svc.i3c"
OBJECT_DECLARE_SIMPLE_TYPE(SVCI3C, SVC_I3C)

#define SVC_I3C_NR_REGS (0x100 >> 2)
#define SVC_I3C_FIFO_SIZE 16

/* Request types supported by the MCTRL.REQUEST field. */
typedef enum SVCI3CRequest {
    SVC_I3C_REQUEST_NONE            = 0x00,
    SVC_I3C_REQUEST_EMIT_START_ADDR = 0x01,
    SVC_I3C_REQUEST_EMIT_STOP       = 0x02,
    SVC_I3C_REQUEST_IBI_ACK_NACK    = 0x03,
    SVC_I3C_REQUEST_PROCESS_DAA     = 0x04,
    SVC_I3C_REQUEST_AUTO_IBI        = 0x07,
} SVCI3CRequest;

/* States the controller reports in MSTATUS.STATE. */
typedef enum SVCI3CState {
    SVC_I3C_STATE_IDLE     = 0x00,
    SVC_I3C_STATE_SLV_REQ  = 0x01,
    SVC_I3C_STATE_MSG_SDR  = 0x02,
    SVC_I3C_STATE_NORM_ACT = 0x03,
    SVC_I3C_STATE_DAA      = 0x05,
    SVC_I3C_STATE_IBI_ACK  = 0x06,
    SVC_I3C_STATE_IBI_RCV  = 0x07,
} SVCI3CState;

/*
 * Transfer information used for transfers initiated by the user writing to the
 * MWMSG_SDR register.
 */
typedef struct MWMsgSDRXfer {
    uint8_t len;
    bool is_i2c;
    bool end_with_stop;
    uint8_t addr;
    bool rnw; /* Read, not write. */
} MWMsgSDRXfer;

typedef struct SVCI3C {
    SysBusDevice parent;

    MemoryRegion mr;
    I3CBus *bus;

    struct {
        uint8_t id;
    } cfg;

    Fifo8 tx_fifo;
    Fifo8 rx_fifo;
    bool mwmsg_xfer_in_progress;
    MWMsgSDRXfer mwmsg_xfer;
    uint32_t regs[SVC_I3C_NR_REGS];
    qemu_irq irq;
} SVCI3C;

#endif /* SVC_I3C_H */

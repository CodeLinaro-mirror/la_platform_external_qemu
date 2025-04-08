/*
 * MIPI HCI I3C controller commands
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HCI_CMD_H
#define HCI_CMD_H

#include "hw/i3c/mipi-hci.h"

/*
 * Immediate transfer length is equal to DTT, unless DTT > 4.
 * If DTT > 4, the length becomes (1 + (dtt & 3)), since DTT > 4 means that the
 * first byte in the immediate transfer is the defining byte.
 */
#define DTT_TO_LEN(dtt) ((dtt) > 4 ? ((dtt) - 4) : (dtt))
#define DTT_HAS_DBP(dtt) ((dtt) > 4)

typedef enum {
  CMD_ATTR_REGULAR_XFER = 0x00,
  CMD_ATTR_IMMEDIATE_XFER = 0x01,
  CMD_ATTR_ADDR_ASSIGN = 0x02,
  CMD_ATTR_COMBO_XFER = 0x03,
  CMD_ATTR_INTERNAL_CONTROL = 0x07,
} CmdAttr;

typedef struct AddrCmd {
  uint8_t cmd_attr:3;
  uint8_t tid:4; /* Transaction ID. */
  uint16_t cmd:8;
  uint8_t rsvd:1;
  uint8_t dev_index:7;
  uint16_t rsvd2:3;
  uint8_t dev_count:4;
  uint8_t roc:1; /* Response on completion. */
  uint8_t toc:1; /* Terminate on completion (STOP). */
  uint32_t rsvd3;
} __attribute__((packed)) AddrCmd;
QEMU_BUILD_BUG_ON(sizeof(AddrCmd) != sizeof(uint64_t));

typedef struct ImmediateXfer {
  uint8_t cmd_attr:3;
  uint8_t tid:4; /* Transaction ID. */
  uint16_t cmd:8;
  uint8_t cp:1; /* Command (CCC or HDR) present. */
  uint8_t dev_index:7;
  uint16_t dtt:3; /* Data transfer type. */
  uint8_t mode:3;
  uint8_t rnw:1;
  uint8_t roc:1; /* Response on completion. */
  uint8_t toc:1; /* Terminate on completion (STOP). */
  uint8_t data[4];
} __attribute__((packed)) ImmediateXfer;
QEMU_BUILD_BUG_ON(sizeof(ImmediateXfer) != sizeof(uint64_t));

typedef enum {
    TRANSFER_MODE_SDR0    = 0,
    TRANSFER_MODE_SDR1    = 1,
    TRANSFER_MODE_SDR2    = 2,
    TRANSFER_MODE_SDR3    = 3,
    TRANSFER_MODE_SDR4    = 4,
    TRANSFER_MODE_HDR_TS  = 5,
    TRANSFER_MODE_HDR_DDR = 6,
} TransferMode;

typedef struct RegularXfer {
  uint8_t cmd_attr:3;
  uint8_t tid:4; /* Transaction ID. */
  uint16_t cmd:8;
  uint8_t cp:1; /* Command (CCC or HDR) present. */
  uint8_t dev_index:7;
  uint8_t rsvd:1;
  uint8_t sre:1; /* Short read is error. */
  uint8_t dbp:1; /* Defining byte present. */
  uint8_t mode:3;
  uint8_t rnw:1;
  uint8_t roc:1; /* Response on completion. */
  uint8_t toc:1; /* Terminate on completion (STOP). */
  uint8_t def_byte;
  uint8_t rsvd2;
  uint16_t data_length;
} __attribute__((packed)) RegularXfer;
QEMU_BUILD_BUG_ON(sizeof(RegularXfer) != sizeof(uint64_t));

typedef struct ComboXfer {
  uint8_t cmd_attr:3;
  uint8_t tid:4; /* Transaction ID. */
  uint16_t cmd:8;
  uint8_t cp:1; /* Command (CCC or HDR) present. */
  uint8_t dev_index:5;
  uint8_t rsvd:1;
  uint8_t dlp:2; /* Data length present. */
  uint8_t fpm:1; /* First phase mode. */
  uint8_t sub_offset_16:1;
  uint8_t mode:3;
  uint8_t rnw:1;
  uint8_t roc:1; /* Response on completion. */
  uint8_t toc:1; /* Terminate on completion (STOP). */
  uint16_t offset;
  uint16_t data_length;
} __attribute__((packed)) ComboXfer;
QEMU_BUILD_BUG_ON(sizeof(ComboXfer) != sizeof(uint64_t));

typedef struct InternalControl {
  uint8_t cmd_attr:3;
  uint8_t tid:4; /* Transaction ID. */
  uint8_t vip:1; /* Vendor info present. */
  uint32_t mipi_rsvd:22;
  uint32_t rsvd;
} __attribute__((packed)) InternalControl;
QEMU_BUILD_BUG_ON(sizeof(InternalControl) != sizeof(uint64_t));

typedef union CmdDescr {
  AddrCmd addr_cmd;
  ImmediateXfer immediate_xfer;
  RegularXfer regular_xfer;
  ComboXfer combo_xfer;
  InternalControl internal_control;

  uint8_t cmd_attr:3;
  uint32_t val32[2];
  uint64_t val64;
} __attribute__((packed)) CmdDescr;
QEMU_BUILD_BUG_ON(sizeof(CmdDescr) != sizeof(uint64_t));

typedef enum {
  RESP_STATUS_SUCCESS = 0,
  RESP_STATUS_ERROR_CRC = 0x01,
  RESP_STATUS_ERROR_PARITY = 0x02,
  RESP_STATUS_ERROR_FRAME = 0x03,
  RESP_STATUS_ERROR_ADDR_HEADER = 0x04,
  RESP_STATUS_ERROR_NACK = 0x05,
  RESP_STATUS_ERROR_OVL = 0x06,
  RESP_STATUS_ERROR_SHORT_READ_ERR = 0x07,
  /* Aborted due to internal error. */
  RESP_STATUS_ERROR_HC_ABORTED = 0x08,
  /* I2C NACK or I3C early termination. */
  RESP_STATUS_ERROR_XFER_ABORTED = 0x09,
  RESP_STATUS_ERROR_NOT_SUPPORTED = 0x0a,
} RespStatus;

typedef union RespDescr {
    struct {
        uint16_t length;
        uint8_t rsvd;
        uint8_t tid:4; /* Transaction ID. */
        uint8_t err:4;
    } __attribute__((packed)) resp;

    uint32_t val32;
} __attribute__((packed)) RespDescr;
QEMU_BUILD_BUG_ON(sizeof(RespDescr) != sizeof(uint32_t));

RespStatus hci_cmd_addr_assign(MIPIHCIState *hci, const AddrCmd *desc,
                               RespDescr *resp);

RespStatus hci_cmd_send(MIPIHCIState *hci, const RegularXfer *desc,
                        RespDescr *resp, const uint8_t *data, size_t len);

RespStatus hci_cmd_read(MIPIHCIState *hci, const RegularXfer *desc,
                        RespDescr *resp, uint8_t *data, uint32_t len,
                        uint32_t *num_read);

RespStatus hci_cmd_immediate_xfer(MIPIHCIState *hci, const ImmediateXfer *desc,
                                  RespDescr *resp);

#endif  /* HCI_CMD_H */

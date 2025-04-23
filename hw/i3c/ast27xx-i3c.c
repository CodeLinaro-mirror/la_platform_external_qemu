/*
 * AST27xx I3C Controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/i3c/ast27xx-i3c.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/i3c/dw-i3c.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/hci-dat.h"
#include "hw/core/irq.h"
#include "qobject/qlist.h"

#define AST27XX_I3C_MMIO_SIZE 0x1000
#define AST27XX_I3C_CTRL_OFFSET 0xd00
#define AST27XX_I3C_PHY_OFFSET 0xe00
#define AST27XX_I3C_EXT_CAPS_OFFSET 0xf00
#define AST27XX_I3C_DMAARB_OFFSET 0xf80

/* Control registers. */
REG32(I3C_CONTROL_0, 0x00)
REG32(I3C_STATUS, 0x04)
REG32(I3C_MST_MRL, 0x08)
REG32(I3C_STATUS_C, 0x0c)
REG32(I3C_DAA_INDEX_0, 0x10)
REG32(I3C_DAA_INDEX_1, 0x14)
REG32(I3C_DAA_INDEX_2, 0x18)
REG32(I3C_DAA_INDEX_3, 0x1c)
REG32(I3C_AUTOCMD_0, 0x20)
REG32(I3C_AUTOCMD_1, 0x24)
REG32(I3C_AUTOCMD_2, 0x28)
REG32(I3C_AUTOCMD_3, 0x2c)
REG32(I3C_AUTOCMD_4, 0x30)
REG32(I3C_AUTOCMD_5, 0x34)
REG32(I3C_AUTOCMD_6, 0x38)
REG32(I3C_AUTOCMD_7, 0x3c)
/*
 * Covers 0x40 to 0x7c. Each field is the index of a device whose auto-command
 * capabilities we're setting.
 */
REG32(I3C_AUTOCMD_SEL, 0x40)
REG32(I3C_WDMA_CTL_080, 0x80)
REG32(I3C_WDMA_CTL_084, 0x84)
REG32(I3C_WDMA_CTL_088, 0x88)
REG32(I3C_RDMA_CTL_090, 0x90)
REG32(I3C_RDMA_CTL_094, 0x94)
REG32(I3C_RDMA_CTL_098, 0x98)
REG32(I3C_RING_CTL_09C, 0x9c)
REG32(I3C_SLV_CTL_0A0, 0xa0)
REG32(I3C_SLV_CTL_0A4, 0xa4)
REG32(I3C_SLV_CTL_0A8, 0xa8)
REG32(I3C_SLV_CTL_0AC, 0xac)
REG32(I3C_SLV_CTL_0B0, 0xb0)
REG32(I3C_SLV_CTL_0B4, 0xb4)
REG32(I3C_SLV_CTL_0B8, 0xb8)
REG32(I3C_SLV_CTL_0BC, 0xbc)
REG32(I3C_SLV_CTL_0C0, 0xc0)
REG32(I3C_SLV_CTL_0C4, 0xc4)
REG32(I3C_SLV_CTL_0C8, 0xc8)
REG32(I3C_SLV_CTL_0CC, 0xcc)
REG32(I3C_SLV_CTL_0D0, 0xd0)
REG32(I3C_SLV_CTL_0D4, 0xd4)
REG32(I3C_QUEUE_PTR_0D8, 0xd8)
REG32(I3C_QUEUE_PTR_0DC, 0xdc)
REG32(I3C_INTR_STATUS, 0xe0)
REG32(I3C_INTR_STATUS_ENABLE, 0xe4)
REG32(I3C_INTR_SIGNAL_ENABLE, 0xe8)
REG32(I3C_INTR_FORCE, 0xec)
REG32(I3C_INTR_STATUS_F0, 0xf0)
    FIELD(I3C_INTR_STATUS_F0, CAP_STATUS, 0, 1)
    FIELD(I3C_INTR_STATUS_F0, PIO_STATUS, 1, 1)
    FIELD(I3C_INTR_STATUS_F0, RHS_STATUS, 2, 1)
REG32(I3C_INTR_PROCESS, 0xf4)
REG32(I3C_IBI_TIMEOUT_F8, 0xf8)

/* PHY registers. */
REG32(I3C_PHY_EXT_CAP_OFFSET, 0x00)
REG32(I3C_PHY_SW_CTRL, 0x04)
REG32(I3C_PHY_CR_I2C_OD_FM_STA_STO_CNT, 0x08)
REG32(I3C_PHY_CR_I2C_OD_FM_SCL_CNT, 0x0c)
REG32(I3C_PHY_CR_I2C_OD_FM_ACK_CNT, 0x10)
REG32(I3C_PHY_CR_I2C_OD_FM_SDA_TRAN_CNT, 0x14)
REG32(I3C_PHY_CR_I2C_OD_FMP_STA_STO_CNT, 0x18)
REG32(I3C_PHY_CR_I2C_OD_FMP_SCL_CNT, 0x1c)
REG32(I3C_PHY_CR_I2C_OD_FMP_ACK_CNT, 0x20)
REG32(I3C_PHY_CR_I2C_OD_FMP_SDA_TRAN_CNT, 0x24)
REG32(I3C_PHY_CR_I3C_OD_STA_STO_CNT, 0x28)
REG32(I3C_PHY_CR_I3C_OD_SCL_CNT, 0x2c)
REG32(I3C_PHY_CR_I3C_OD_ACK_CNT, 0x30)
REG32(I3C_PHY_CR_I3C_OD_SDA_TRAN_CNT, 0x34)
REG32(I3C_PHY_CR_I3C_SDR0_PP_SCL_CNT, 0x38)
REG32(I3C_PHY_CR_I3C_SDR0_PP_TBIT_CNT, 0x3c)
REG32(I3C_PHY_CR_I3C_SDR0_PP_SDA_TRAN_CNT, 0x40)
REG32(I3C_PHY_CR_I3C_SDR1_PP_SCL_CNT, 0x44)
REG32(I3C_PHY_CR_I3C_SDR1_PP_TBIT_CNT, 0x48)
REG32(I3C_PHY_CR_I3C_SDR1_PP_SDA_TRAN_CNT, 0x4c)
REG32(I3C_PHY_CR_I3C_SDR2_PP_SCL_CNT, 0x50)
REG32(I3C_PHY_CR_I3C_SDR2_PP_TBIT_CNT, 0x54)
REG32(I3C_PHY_CR_I3C_SDR2_PP_SDA_TRAN_CNT, 0x58)
REG32(I3C_PHY_CR_I3C_SDR3_PP_SCL_CNT, 0x5c)
REG32(I3C_PHY_CR_I3C_SDR3_PP_TBIT_CNT, 0x60)
REG32(I3C_PHY_CR_I3C_SDR3_PP_SDA_TRAN_CNT, 0x64)
REG32(I3C_PHY_CR_I3C_SDR4_PP_SCL_CNT, 0x68)
REG32(I3C_PHY_CR_I3C_SDR4_PP_TBIT_CNT, 0x6c)
REG32(I3C_PHY_CR_I3C_SDR4_PP_SDA_TRAN_CNT, 0x70)
REG32(I3C_PHY_CR_I3C_DDR_PP_SCL_CNT, 0x74)
REG32(I3C_PHY_CR_I3C_DDR_PP_TBIT_CNT, 0x78)
REG32(I3C_PHY_CR_I3C_DDR_PP_SDA_TRAN_CNT, 0x7c)
REG32(I3C_PHY_SR_P_PREPARE_SCL_SDA_CNT, 0x80)
REG32(I3C_PHY_CCR_TO_NCR_OVERLAP_CNT, 0x84)
REG32(I3C_PHY_CR_IBI_ADDR_ACK_PROLONG_CNT, 0x88)
REG32(I3C_PHY_TG_WR_ADDR_ACK_PROLONG, 0x8c)
REG32(I3C_PHY_TG_SDA_TRAN_CNT, 0x90)
REG32(I3C_PHY_DDR_CMD_HANDOFF_EARLY_TM_CNT, 0x94)
REG32(I3C_PHY_CR_SCL_SDA_PULLUP_EN, 0x98)
REG32(I3C_PHY_SPECIAL_PATTERN_SET, 0x9c)
REG32(I3C_PHY_SPECIAL_PATTERN_SW_OPT, 0xa0)
REG32(I3C_PHY_SPECIAL_PATTERN_SCL_TOGGLE_SET, 0xa4)
REG32(I3C_PHY_SPECIAL_PATTERN_SCL_TOGGLE_PAT, 0xa8)
REG32(I3C_PHY_SPECIAL_PATTERN_SCL_TIEL_SET, 0xac)
REG32(I3C_PHY_SDA_DETECTOR_CNT0, 0xb4)
REG32(I3C_PHY_SDA_DETECTOR_CNT1, 0xb8)
REG32(I3C_PHY_SDA_DETECTOR_CNT2, 0xbc)
REG32(I3C_PHY_SDA_STUCK_SET1, 0xc0)
REG32(I3C_PHY_SDA_STUCK_READ, 0xc4)
REG32(I3C_PHY_READ_PHY_STATE_MACHINE, 0xc8)
REG32(I3C_PHY_PHY_OPTION, 0xcc)
REG32(I3C_PHY_CR_SCL_SDA_PULLUP_EN_ADDITIONAL, 0xd0)
REG32(I3C_PHY_SPIKE_FILTER, 0xd4)
REG32(I3C_PHY_SCL_SDA_TIMING_CNT_ADDITIONAL, 0xd8)
REG32(I3C_PHY_BUS_FREE_TIME_CNT, 0xdc)
REG32(I3C_PHY_SPECIAL_PATTERN_SET_ADDITIONAL, 0xe0)
REG32(I3C_PHY_BUS_CONTENTION_CHK0, 0xe4)
REG32(I3C_PHY_BUS_CONTENTION_CNT0, 0xe8)
REG32(I3C_PHY_BUS_CONTENTION_CNT1, 0xec)
REG32(I3C_PHY_BUS_CONTENTION_CNT2, 0xf0)

/* DMA MBUS ARBiter registers. */
REG32(DMA_MBUS_ARB_CTRL0, 0x0)
REG32(DMA_MBUS_ARB_CLR0, 0x4)
REG32(DMA_MBUS_ARB_DBG0, 0x10)
REG32(DMA_MBUS_ARB_DBG1, 0x14)
REG32(DMA_MBUS_ARB_DBG2, 0x18)
REG32(DMA_MBUS_ARB_DBG3, 0x1c)

static const uint32_t ast27xx_i3c_phy_ro_mask[AST27XX_I3C_PHY_NUM_REGS] = {
    [R_I3C_PHY_EXT_CAP_OFFSET]                  = 0xffffffff,
    [R_I3C_PHY_SW_CTRL]                         = 0x00ffc0c0,
    [R_I3C_PHY_CR_I2C_OD_FM_STA_STO_CNT]        = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FM_SCL_CNT]            = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FM_ACK_CNT]            = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FM_SDA_TRAN_CNT]       = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FMP_STA_STO_CNT]       = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FMP_SCL_CNT]           = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FMP_ACK_CNT]           = 0xf100f100,
    [R_I3C_PHY_CR_I2C_OD_FMP_SDA_TRAN_CNT]      = 0xf100f100,
    [R_I3C_PHY_CR_I3C_OD_STA_STO_CNT]           = 0xf100f100,
    [R_I3C_PHY_CR_I3C_OD_SCL_CNT]               = 0xf100f100,
    [R_I3C_PHY_CR_I3C_OD_ACK_CNT]               = 0xf100f100,
    [R_I3C_PHY_CR_I3C_OD_SDA_TRAN_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR0_PP_SCL_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR0_PP_TBIT_CNT]         = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR0_PP_SDA_TRAN_CNT]     = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR1_PP_SCL_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR1_PP_TBIT_CNT]         = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR1_PP_SDA_TRAN_CNT]     = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR2_PP_SCL_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR2_PP_TBIT_CNT]         = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR2_PP_SDA_TRAN_CNT]     = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR3_PP_SCL_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR3_PP_TBIT_CNT]         = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR3_PP_SDA_TRAN_CNT]     = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR4_PP_SCL_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR4_PP_TBIT_CNT]         = 0xf100f100,
    [R_I3C_PHY_CR_I3C_SDR4_PP_SDA_TRAN_CNT]     = 0xf100f100,
    [R_I3C_PHY_CR_I3C_DDR_PP_SCL_CNT]           = 0xf100f100,
    [R_I3C_PHY_CR_I3C_DDR_PP_TBIT_CNT]          = 0xf100f100,
    [R_I3C_PHY_CR_I3C_DDR_PP_SDA_TRAN_CNT]      = 0xf100f100,
    [R_I3C_PHY_SR_P_PREPARE_SCL_SDA_CNT]        = 0xf100f100,
    [R_I3C_PHY_CCR_TO_NCR_OVERLAP_CNT]          = 0xfffff800,
    [R_I3C_PHY_CR_IBI_ADDR_ACK_PROLONG_CNT]     = 0xfffff800,
    [R_I3C_PHY_TG_WR_ADDR_ACK_PROLONG]          = 0x7ffff800,
    [R_I3C_PHY_TG_SDA_TRAN_CNT]                 = 0xf100f100,
    [R_I3C_PHY_DDR_CMD_HANDOFF_EARLY_TM_CNT]    = 0xffff0000,
    [R_I3C_PHY_CR_SCL_SDA_PULLUP_EN]            = 0xff888888,
    [R_I3C_PHY_SPECIAL_PATTERN_SET]             = 0xe0000000,
    [R_I3C_PHY_SPECIAL_PATTERN_SW_OPT]          = 0x7ffff000,
    [R_I3C_PHY_SPECIAL_PATTERN_SCL_TOGGLE_SET]  = 0xfffc00e0,
    [R_I3C_PHY_SDA_DETECTOR_CNT0]               = 0xff000000,
    [R_I3C_PHY_SDA_STUCK_READ]                  = 0xffffffff,
    [R_I3C_PHY_READ_PHY_STATE_MACHINE]          = 0xffffffff,
    [R_I3C_PHY_PHY_OPTION]                      = 0xfffc0000,
    [R_I3C_PHY_CR_SCL_SDA_PULLUP_EN_ADDITIONAL] = 0xff888888,
    [R_I3C_PHY_SPIKE_FILTER]                    = 0xff0000c0,
    [R_I3C_PHY_SCL_SDA_TIMING_CNT_ADDITIONAL]   = 0xf100f100,
    [R_I3C_PHY_BUS_FREE_TIME_CNT]               = 0xfffff800,
    [R_I3C_PHY_SPECIAL_PATTERN_SET_ADDITIONAL]  = 0xf8000000,
    [R_I3C_PHY_BUS_CONTENTION_CHK0]             = 0xfffff000,
    [R_I3C_PHY_BUS_CONTENTION_CNT0]             = 0xf100f100,
    [R_I3C_PHY_BUS_CONTENTION_CNT1]             = 0xf100f100,
    [R_I3C_PHY_BUS_CONTENTION_CNT2]             = 0xf100f100,
};

static const uint32_t ast27xx_i3c_ctrl_ro_mask[AST27XX_I3C_CTRL_NUM_REGS] = {
    [R_I3C_CONTROL_0]          = 0x708200ec,
    [R_I3C_STATUS]             = 0xffffffff,
    [R_I3C_MST_MRL]            = 0xffff0000,
    [R_I3C_STATUS_C]           = 0xffffffff,
    [R_I3C_AUTOCMD_0]          = 0xff0000f8,
    [R_I3C_AUTOCMD_1]          = 0xff0000f8,
    [R_I3C_AUTOCMD_2]          = 0xff0000f8,
    [R_I3C_AUTOCMD_3]          = 0xff0000f8,
    [R_I3C_AUTOCMD_4]          = 0xff0000f8,
    [R_I3C_AUTOCMD_5]          = 0xff0000f8,
    [R_I3C_AUTOCMD_6]          = 0xff0000f8,
    [R_I3C_AUTOCMD_7]          = 0xff0000f8,
    [R_I3C_AUTOCMD_SEL]        = 0x88888888,
    [R_I3C_WDMA_CTL_080]       = 0x0000fffe,
    [R_I3C_WDMA_CTL_084]       = 0xffffffff,
    [R_I3C_WDMA_CTL_088]       = 0xffffffff,
    [R_I3C_RDMA_CTL_090]       = 0x0000fffe,
    [R_I3C_RDMA_CTL_094]       = 0xffffffff,
    [R_I3C_RDMA_CTL_098]       = 0xffffffff,
    [R_I3C_RING_CTL_09C]       = 0xffffffff,
    [R_I3C_SLV_CTL_0A0]        = 0xff000000,
    [R_I3C_SLV_CTL_0A8]        = 0xffff0000,
    [R_I3C_SLV_CTL_0AC]        = 0xffffffff,
    [R_I3C_SLV_CTL_0B0]        = 0xbfcef80c,
    [R_I3C_SLV_CTL_0B4]        = 0xffffff8f,
    [R_I3C_SLV_CTL_0BC]        = 0xffffffff,
    [R_I3C_SLV_CTL_0C0]        = 0xff000000,
    [R_I3C_SLV_CTL_0C4]        = 0xffff0000,
    [R_I3C_SLV_CTL_0C8]        = 0xfc000000,
    [R_I3C_QUEUE_PTR_0D8]      = 0xffffffff,
    [R_I3C_QUEUE_PTR_0DC]      = 0xffffffff,
    [R_I3C_INTR_STATUS]        = 0xfff8e80c,
    [R_I3C_INTR_STATUS_ENABLE] = 0xfff8e80c,
    [R_I3C_INTR_SIGNAL_ENABLE] = 0xfff8e80c,
    [R_I3C_INTR_FORCE]         = 0xfff8e80c,
    [R_I3C_INTR_STATUS_F0]     = 0xffffffff,
    [R_I3C_INTR_PROCESS]       = 0xfffffffe,
    [R_I3C_IBI_TIMEOUT_F8]     = 0xfffe0000,
};

static const uint32_t
    ast27xx_i3c_dmaarb_ro_mask[AST27XX_I3C_DMAARB_NUM_REGS] = {
    [R_DMA_MBUS_ARB_CLR0] = 0xfffffffe,
    [R_DMA_MBUS_ARB_DBG0] = 0xffffffff,
    [R_DMA_MBUS_ARB_DBG1] = 0xffffffff,
    [R_DMA_MBUS_ARB_DBG2] = 0xffffffff,
    [R_DMA_MBUS_ARB_DBG3] = 0xffffffff,
};

static const uint32_t ast27xx_i3c_ctrl_reset[AST27XX_I3C_CTRL_NUM_REGS] = {
    [R_I3C_CONTROL_0]      = 0x00002400,
    [R_I3C_AUTOCMD_0]      = 0x00008000,
    [R_I3C_AUTOCMD_1]      = 0x00008000,
    [R_I3C_AUTOCMD_2]      = 0x00008000,
    [R_I3C_AUTOCMD_3]      = 0x00008000,
    [R_I3C_AUTOCMD_4]      = 0x00008000,
    [R_I3C_AUTOCMD_5]      = 0x00008000,
    [R_I3C_AUTOCMD_6]      = 0x00008000,
    [R_I3C_AUTOCMD_7]      = 0x00008000,
    [R_I3C_WDMA_CTL_080]   = 0x03800000,
    [R_I3C_RDMA_CTL_090]   = 0x03800000,
    [R_I3C_SLV_CTL_0B0]    = 0x00010000,
    [R_I3C_SLV_CTL_0B4]    = 0x00010070,
    [R_I3C_SLV_CTL_0B8]    = 0x00800080,
    [R_I3C_IBI_TIMEOUT_F8] = 0x00000160,
};

static const uint32_t ast27xx_i3c_phy_reset[AST27XX_I3C_PHY_NUM_REGS] = {
    [R_I3C_PHY_EXT_CAP_OFFSET]                 = 0x00000100,
    [R_I3C_PHY_SW_CTRL]                        = 0x00003737,
    [R_I3C_PHY_CR_I2C_OD_FM_STA_STO_CNT]       = 0x00770103,
    [R_I3C_PHY_CR_I2C_OD_FM_SCL_CNT]           = 0x00770103,
    [R_I3C_PHY_CR_I2C_OD_FM_ACK_CNT]           = 0x00770103,
    [R_I3C_PHY_CR_I2C_OD_FM_SDA_TRAN_CNT]      = 0x00010001,
    [R_I3C_PHY_CR_I2C_OD_FMP_STA_STO_CNT]      = 0x00330063,
    [R_I3C_PHY_CR_I2C_OD_FMP_SCL_CNT]          = 0x00330063,
    [R_I3C_PHY_CR_I2C_OD_FMP_ACK_CNT]          = 0x00330063,
    [R_I3C_PHY_CR_I2C_OD_FMP_SDA_TRAN_CNT]     = 0x00010001,
    [R_I3C_PHY_CR_I3C_OD_STA_STO_CNT]          = 0x00070007,
    [R_I3C_PHY_CR_I3C_OD_SCL_CNT]              = 0x00130027,
    [R_I3C_PHY_CR_I3C_OD_ACK_CNT]              = 0x00130027,
    [R_I3C_PHY_CR_I3C_OD_SDA_TRAN_CNT]         = 0x00010001,
    [R_I3C_PHY_CR_I3C_SDR0_PP_SCL_CNT]         = 0x00070007,
    [R_I3C_PHY_CR_I3C_SDR0_PP_TBIT_CNT]        = 0x00070007,
    [R_I3C_PHY_CR_I3C_SDR0_PP_SDA_TRAN_CNT]    = 0x00010001,
    [R_I3C_PHY_CR_I3C_SDR1_PP_SCL_CNT]         = 0x003b0040,
    [R_I3C_PHY_CR_I3C_SDR1_PP_TBIT_CNT]        = 0x003b0040,
    [R_I3C_PHY_CR_I3C_SDR1_PP_SDA_TRAN_CNT]    = 0x00010001,
    [R_I3C_PHY_CR_I3C_SDR2_PP_SCL_CNT]         = 0x00500055,
    [R_I3C_PHY_CR_I3C_SDR2_PP_TBIT_CNT]        = 0x00500055,
    [R_I3C_PHY_CR_I3C_SDR2_PP_SDA_TRAN_CNT]    = 0x00010001,
    [R_I3C_PHY_CR_I3C_SDR3_PP_SCL_CNT]         = 0x00f900f9,
    [R_I3C_PHY_CR_I3C_SDR3_PP_TBIT_CNT]        = 0x007c007c,
    [R_I3C_PHY_CR_I3C_SDR3_PP_SDA_TRAN_CNT]    = 0x00010001,
    [R_I3C_PHY_CR_I3C_SDR4_PP_SCL_CNT]         = 0x00f900f9,
    [R_I3C_PHY_CR_I3C_SDR4_PP_TBIT_CNT]        = 0x00f900f9,
    [R_I3C_PHY_CR_I3C_SDR4_PP_SDA_TRAN_CNT]    = 0x00010001,
    [R_I3C_PHY_CR_I3C_DDR_PP_SCL_CNT]          = 0x00070007,
    [R_I3C_PHY_CR_I3C_DDR_PP_TBIT_CNT]         = 0x00070007,
    [R_I3C_PHY_CR_I3C_DDR_PP_SDA_TRAN_CNT]     = 0x00010001,
    [R_I3C_PHY_SR_P_PREPARE_SCL_SDA_CNT]       = 0x0004000b,
    [R_I3C_PHY_CCR_TO_NCR_OVERLAP_CNT]         = 0x00000027,
    [R_I3C_PHY_CR_IBI_ADDR_ACK_PROLONG_CNT]    = 0x00000004,
    [R_I3C_PHY_TG_WR_ADDR_ACK_PROLONG]         = 0x00000011,
    [R_I3C_PHY_DDR_CMD_HANDOFF_EARLY_TM_CNT]   = 0x00000407,
    [R_I3C_PHY_CR_SCL_SDA_PULLUP_EN]           = 0x00770077,
    [R_I3C_PHY_SPECIAL_PATTERN_SET]            = 0x08090909,
    [R_I3C_PHY_SPECIAL_PATTERN_SW_OPT]         = 0x00000020,
    [R_I3C_PHY_SPECIAL_PATTERN_SCL_TOGGLE_SET] = 0x00000003,
    [R_I3C_PHY_SPECIAL_PATTERN_SCL_TOGGLE_PAT] = 0xffffffff,
    [R_I3C_PHY_SPECIAL_PATTERN_SCL_TIEL_SET]   = 0x001e8480,
    [R_I3C_PHY_SDA_DETECTOR_CNT0]              = 0x00130704,
    [R_I3C_PHY_SDA_DETECTOR_CNT1]              = 0x090c00c7,
    [R_I3C_PHY_SDA_DETECTOR_CNT2]              = 0x00630063,
    [R_I3C_PHY_SDA_STUCK_SET1]                 = 0x75300708,
    [R_I3C_PHY_PHY_OPTION]                     = 0x00000190,
    [R_I3C_PHY_BUS_FREE_TIME_CNT]              = 0x00000006,
    [R_I3C_PHY_SPECIAL_PATTERN_SET_ADDITIONAL] = 0x00c80109,
    [R_I3C_PHY_BUS_CONTENTION_CHK0]            = 0x00000f90,
};

static uint8_t ast27xx_i3c_get_dev_dynamic_addr(MIPIHCIState *hci,
                                                uint8_t dat_index)
{
    return dat_index / HCI_DAT_ENTRY_SIZE;
}

static uint8_t ast27xx_i3c_get_next_dynamic_addr(MIPIHCIState *hci,
                                                 uint8_t dat_index)
{
    AST27xxI3CState *s = container_of(hci, AST27xxI3CState, parent);
    /*
     * These registers aren't documented, but they're bitfields, and the offset
     * of the set bit is the address to use.
     * We're going to assume that it will use use the first set bit, so iterate
     * through each one until we find one that's set.
     */
    uint32_t reg = R_I3C_DAA_INDEX_0;
    for (reg = R_I3C_DAA_INDEX_0; reg <= R_I3C_DAA_INDEX_3; reg++) {
        for (uint32_t i = 0; i < sizeof(uint32_t) * 8; i++) {
            if (s->ctrl_regs[reg] & (1UL << i)) {
                return i;
            }
        }
    }

    g_autofree char *path = object_get_canonical_path(OBJECT(hci));
    qemu_log_mask(LOG_GUEST_ERROR, "%s: could not find a dynamic address to "
                  "use", path);
    return 0;
}

static void ast27xx_i3c_update_irq(MIPIHCIState *hci, MIPIHCIIRQContext ctx)
{
    AST27xxI3CState *s = container_of(hci, AST27xxI3CState, parent);
    HCICoreState *core = &hci->core;
    HCIDMAState *dma = &hci->dma;

    s->ctrl_regs[R_I3C_INTR_STATUS_F0] = 0;

    /* INTR_STATUS is masked before setting the IRQ line. */
    core->regs[R_INTR_STATUS] &= core->regs[R_INTR_SIGNAL_ENABLE];
    dma->regs[R_RH_INTR_STATUS] &= dma->regs[R_RH_INTR_SIGNAL_ENABLE];

    bool level = !!(core->regs[R_INTR_STATUS] &
                    core->regs[R_INTR_SIGNAL_ENABLE]);
    level |= !!(dma->regs[R_RH_INTR_STATUS] &
                dma->regs[R_RH_INTR_SIGNAL_ENABLE]);

    if (level) {
        switch (ctx) {
        case MIPI_HCI_IRQ_CONTEXT_CORE:
            ARRAY_FIELD_DP32(s->ctrl_regs, I3C_INTR_STATUS_F0, CAP_STATUS, 1);
            break;
        case MIPI_HCI_IRQ_CONTEXT_DMA:
            ARRAY_FIELD_DP32(s->ctrl_regs, I3C_INTR_STATUS_F0, RHS_STATUS, 1);
            break;
        case MIPI_HCI_IRQ_CONTEXT_PIO:
            ARRAY_FIELD_DP32(s->ctrl_regs, I3C_INTR_STATUS_F0, PIO_STATUS, 1);
            break;
        default:
            g_assert_not_reached();
        }
    }

    qemu_set_irq(hci->irq[0], level);
}

static uint64_t ast27xx_i3c_ctrl_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    AST27xxI3CState *s = AST27XX_I3C(opaque);
    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->ctrl_regs));

    return s->ctrl_regs[offset];
}

static void ast27xx_i3c_ctrl_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size)
{
    AST27xxI3CState *s = AST27XX_I3C(opaque);
    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->ctrl_regs));

    value &= ~ast27xx_i3c_ctrl_ro_mask[offset];
    s->ctrl_regs[offset] = value;
}

static uint64_t ast27xx_i3c_phy_read(void *opaque, hwaddr offset, unsigned size)
{
    AST27xxI3CState *s = AST27XX_I3C(opaque);
    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->phy_regs));

    return s->phy_regs[offset];
}

static void ast27xx_i3c_phy_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    AST27xxI3CState *s = AST27XX_I3C(opaque);
    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->phy_regs));

    value &= ~ast27xx_i3c_phy_ro_mask[offset];
    s->phy_regs[offset] = value;
}

static uint64_t ast27xx_i3c_dmaarb_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    AST27xxI3CState *s = AST27XX_I3C(opaque);
    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->phy_regs));

    return s->dmaarb_regs[offset];
}

static void ast27xx_i3c_dmaarb_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    AST27xxI3CState *s = AST27XX_I3C(opaque);
    offset /= sizeof(uint32_t);

    /* MMIO region size should prevent this from happening. */
    g_assert(offset < ARRAY_SIZE(s->phy_regs));

    value &= ~ast27xx_i3c_dmaarb_ro_mask[offset];
    s->dmaarb_regs[offset] = value;
}

static const MemoryRegionOps ast27xx_i3c_ops = {
    .read = ast27xx_i3c_ctrl_read,
    .write = ast27xx_i3c_ctrl_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps ast27xx_i3c_phy_ops = {
    .read = ast27xx_i3c_phy_read,
    .write = ast27xx_i3c_phy_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps ast27xx_i3c_dmaarb_ops = {
    .read = ast27xx_i3c_dmaarb_read,
    .write = ast27xx_i3c_dmaarb_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ast27xx_i3c_init_ext_capabilities(AST27xxI3CState *s)
{
    const uint32_t ext_caps[] = {
        0x00000401, /* HW_ID_HEADER */
        0x000003f6, /* HW_ID_MIPI_VENDOR */
        0x00000002, /* HW_ID_I3C_VER */
        0x00000000, /* HW_ID_I3C_PRODUCT*/
        0x00000202, /* CTL_CFG_HEADER */
        0x00000030, /* CTL_CFG_OPERATION_MODE */
        0x000004c0, /* EXTCAPS_HEADER */
        0x00000d00, /* EXTCAPS_CTRL */
        0x00000e00, /* EXTCAPS_PHY */
        0x00000f80, /* EXTCAP_DMAARB */
    };

    QList *ext_capabilities = qlist_new();
    for (int i = 0; i < ARRAY_SIZE(ext_caps); i++) {
        qlist_append_int(ext_capabilities, ext_caps[i]);
    }

    MIPIHCIState *hci = MIPI_HCI(s);
    hci->core.cfg.ext_caps_section_offset = AST27XX_I3C_EXT_CAPS_OFFSET;
    qdev_prop_set_array(DEVICE(hci), "ext-capabilities", ext_capabilities);
}

static void ast27xx_i3c_init_ring_headers(AST27xxI3CState *s)
{
    QList *ring_offsets = qlist_new();
    qlist_append_int(ring_offsets, 0x830);

    MIPIHCIState *hci = MIPI_HCI(s);
    qdev_prop_set_array(DEVICE(hci), "ring-offsets", ring_offsets);
}

static void ast27xx_i3c_instance_init(Object *obj)
{
    AST27xxI3CState *s = AST27XX_I3C(obj);
    DeviceState *dev = DEVICE(obj);

    ast27xx_i3c_init_ext_capabilities(s);
    ast27xx_i3c_init_ring_headers(s);

    /* Values  from AST2750-A1 datasheet. */
    qdev_prop_set_uint32(dev, "hc-capabilities", 0x0468);
    qdev_prop_set_uint32(dev, "dat-table-offset", 0x0100);
    qdev_prop_set_uint32(dev, "dat-table-size", 0x7f);
    qdev_prop_set_uint32(dev, "dct-table-offset", 0x0500);
    qdev_prop_set_uint32(dev, "dct-table-size", 0x01);
    qdev_prop_set_uint32(dev, "ext-caps-section-offset", 0x0f00);
    qdev_prop_set_uint32(dev, "hci-version", 0x0110);
    qdev_prop_set_uint32(dev, "int-ctrl-cmds-en", 0x3f);
    qdev_prop_set_uint32(dev, "ring-header-section-offset", 0x0800);
    qdev_prop_set_uint32(dev, "preamble-size", 0x02);
    qdev_prop_set_uint32(dev, "header-size", 0x05);
    qdev_prop_set_uint32(dev, "xfer-struct-size", 0x14);
    qdev_prop_set_uint32(dev, "resp-struct-size", 0x04);
    qdev_prop_set_uint32(dev, "ibi-stat", 0x04);
}

static void ast27xx_i3c_realize(DeviceState *dev, Error **errp)
{
    AST27xxI3CState *s = AST27XX_I3C(dev);
    AST27xxI3CClass *aic = AST27XX_I3C_GET_CLASS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    Error *local_err = NULL;

    aic->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }


    memory_region_init_io(&s->ctrl_iomem, OBJECT(s), &ast27xx_i3c_ops, s,
                          TYPE_AST27XX_I3C"-ctrl-mmio",
                          AST27XX_I3C_CTRL_NUM_REGS * sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->ctrl_iomem);
    memory_region_init_io(&s->phy_iomem, OBJECT(s), &ast27xx_i3c_phy_ops, s,
                          TYPE_AST27XX_I3C"-phy-mmio",
                          AST27XX_I3C_PHY_NUM_REGS * sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->phy_iomem);
    memory_region_init_io(&s->dmaarb_iomem, OBJECT(s), &ast27xx_i3c_dmaarb_ops,
                          s, TYPE_AST27XX_I3C"-dmaarb-mmio",
                          AST27XX_I3C_DMAARB_NUM_REGS * sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->dmaarb_iomem);

    MemoryRegion *hci_mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->parent),
                                                    0);
    memory_region_add_subregion(hci_mmio, AST27XX_I3C_CTRL_OFFSET,
                                &s->ctrl_iomem);
    memory_region_add_subregion(hci_mmio, AST27XX_I3C_PHY_OFFSET,
                                &s->phy_iomem);
    memory_region_add_subregion(hci_mmio, AST27XX_I3C_DMAARB_OFFSET,
                                &s->dmaarb_iomem);
}

static void ast27xx_i3c_enter_reset(Object *obj, ResetType type)
{
    AST27xxI3CState *s = AST27XX_I3C(obj);
    AST27xxI3CClass *aic = AST27XX_I3C_GET_CLASS(obj);

    if (aic->parent_phases.enter) {
        aic->parent_phases.enter(obj, type);
    }

    for (int i = 0; i < ARRAY_SIZE(s->ctrl_regs); i++) {
        s->ctrl_regs[i] = ast27xx_i3c_ctrl_reset[i];
    }
    for (int i = 0; i < ARRAY_SIZE(s->phy_regs); i++) {
        s->phy_regs[i] = ast27xx_i3c_phy_reset[i];
    }
    memset(s->dmaarb_regs, 0, sizeof(s->dmaarb_regs));
}

static void ast27xx_i3c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    AST27xxI3CClass *aic = AST27XX_I3C_CLASS(klass);
    MIPIHCIClass *mhc = MIPI_HCI_CLASS(aic);

    dc->desc = "AST27xx I3C Controller";

    device_class_set_parent_realize(dc, ast27xx_i3c_realize,
                                    &aic->parent_realize);
    resettable_class_set_parent_phases(rc, ast27xx_i3c_enter_reset, NULL, NULL,
                                       &aic->parent_phases);
    mhc->update_irq = ast27xx_i3c_update_irq;
    mhc->get_next_dynamic_addr = ast27xx_i3c_get_next_dynamic_addr;
    mhc->get_dev_dynamic_addr = ast27xx_i3c_get_dev_dynamic_addr;
}

static const TypeInfo ast27xx_i3c_info = {
    .name = TYPE_AST27XX_I3C,
    .parent = TYPE_MIPI_HCI,
    .instance_init = ast27xx_i3c_instance_init,
    .instance_size = sizeof(AST27xxI3CState),
    .class_init = ast27xx_i3c_class_init,
    .class_size = sizeof(AST27xxI3CClass),
};

static void ast27xx_i3c_register_types(void)
{
    type_register_static(&ast27xx_i3c_info);
}

type_init(ast27xx_i3c_register_types);

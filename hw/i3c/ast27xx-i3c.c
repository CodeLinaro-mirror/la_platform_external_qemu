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
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/core/irq.h"

#define AST27XX_I3C_MMIO_SIZE 0x1000

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

static uint64_t ast27xx_i3c_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void ast27xx_i3c_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
}

static const MemoryRegionOps ast27xx_i3c_ops = {
    .read = ast27xx_i3c_read,
    .write = ast27xx_i3c_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ast27xx_i3c_instance_init(Object *obj)
{
}

static void ast27xx_i3c_realize(DeviceState *dev, Error **errp)
{
    AST27xxI3CState *s = AST27XX_I3C(dev);


    memory_region_init_io(&s->iomem, OBJECT(s), &ast27xx_i3c_ops, s,
                          TYPE_AST27XX_I3C"-mmio", AST27XX_I3C_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void ast27xx_i3c_enter_reset(Object *obj, ResetType type)
{
    AST27xxI3CClass *aic = AST27XX_I3C_GET_CLASS(obj);

    if (aic->parent_phases.enter) {
        aic->parent_phases.enter(obj, type);
    }
}

static void ast27xx_i3c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    AST27xxI3CClass *aic = AST27XX_I3C_CLASS(klass);

    dc->desc = "AST27xx I3C Controller";

    device_class_set_parent_realize(dc, ast27xx_i3c_realize,
                                    &aic->parent_realize);
    resettable_class_set_parent_phases(rc, ast27xx_i3c_enter_reset, NULL, NULL,
                                       &aic->parent_phases);
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

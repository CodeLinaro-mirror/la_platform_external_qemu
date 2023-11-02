/*
 * NPCM8xx GDMA Controller
 *
 * Copyright (c) 2023 Google, LLC
 *
 * This code is licensed under the GPL version 2 or later. See the COPYING flie
 * in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/dma/npcm8xx-gdma.h"
#include "system/dma.h"
#include "hw/stream.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "hw/registerfields.h"

REG32(CTL, 0x00)
    FIELD(CTL, TC,      18, 1)
    FIELD(CTL, SOFTREQ, 16, 1)
    FIELD(CTL, DM,      15, 1)
    FIELD(CTL, TWS,     12, 2)
    FIELD(CTL, BME,     9, 1)
    FIELD(CTL, SIEN,    8, 1)
    FIELD(CTL, SAFIX,   7, 1)
    FIELD(CTL, DAFIX,   6, 1)
    FIELD(CTL, SADIR,   5, 1)
    FIELD(CTL, DADIR,   4, 1)
    FIELD(CTL, GDMAMS,  2, 2)
    FIELD(CTL, GDMAEN,  0, 1)
REG32(SRCB, 0x04)
REG32(DSTB, 0x08)
REG32(TCNT, 0x0c)
    FIELD(TCNT, TFR_CNT, 0, 24)
REG32(CSRC, 0x10)
REG32(CDST, 0x14)
REG32(CTCNT, 0x18)
    FIELD(CTCNT, TFR_CNT, 0, 24)

/* All 0s. */
static const uint32_t
npcm8xx_gdma_channel_resets[NPCM8XX_GDMA_CHANNEL_NR_REGS] = {};

static const uint32_t npcm8xx_gdma_channel_ro[NPCM8XX_GDMA_CHANNEL_NR_REGS] = {
    [R_CTL]   = 0xfffe1802,
    [R_CSRC]  = 0xffffffff,
    [R_CDST]  = 0xffffffff,
    [R_CTCNT] = 0xffffffff,
};

static uint64_t npcm8xx_gdma_channel_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    NPCM8xxGDMAChannel *s = NPCM8XX_GDMA_CHANNEL(opaque);
    uint32_t addr = offset >> 2;

    return s->regs[addr];
}

static void npcm8xx_gdma_update_irq(NPCM8xxGDMAChannel *s)
{
    bool level = ARRAY_FIELD_EX32(s->regs, CTL, SIEN) ||
                 ARRAY_FIELD_EX32(s->regs, CTL, TC);
    qemu_set_irq(s->controller->irq, level);
}

static void npcm8xx_gdma_channel_enter_reset(Object *obj, ResetType type)
{
    NPCM8xxGDMAChannel *s = NPCM8XX_GDMA_CHANNEL(obj);
    for (int i = 0; i < ARRAY_SIZE(s->regs); i++) {
        s->regs[i] = npcm8xx_gdma_channel_resets[i];
    }
}

static void npcm8xx_gdma_channel_do_dma(NPCM8xxGDMAChannel *s)
{
    uint32_t xfer_count = s->regs[R_TCNT];
    uint8_t xfer_size = 1 << ARRAY_FIELD_EX32(s->regs, CTL, TWS);
    uint32_t src_addr = s->regs[R_SRCB];
    uint32_t dest_addr = s->regs[R_DSTB];
    bool dest_fixed = ARRAY_FIELD_EX32(s->regs, CTL, DAFIX);
    bool src_fixed = ARRAY_FIELD_EX32(s->regs, CTL, SAFIX);
    bool dest_decr = ARRAY_FIELD_EX32(s->regs, CTL, DADIR);
    bool src_decr = ARRAY_FIELD_EX32(s->regs, CTL, SADIR);

    /* In burst mode, the xfer count is / 4. */
    if (ARRAY_FIELD_EX32(s->regs, CTL, BME)) {
        xfer_count /= 4;
    }

    for (uint32_t i = 0; i < xfer_count; i++) {
        uint32_t data;

        /* Do the transfer from memory to memory. */
        MemTxResult result = address_space_rw(&s->dma_as, src_addr, s->attrs,
                                              &data, xfer_size,
                                              /*is_write=*/false);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Failed to read data from 0x%x. "
                          "MemTxResult: 0x%x",
                          DEVICE(s)->canonical_path, src_addr,
                          result);
            return;
        }
        result = address_space_rw(&s->dma_as, dest_addr, s->attrs, &data,
                                  xfer_size, /*is_write=*/true);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Failed to write data to 0x%x. "
                          "MemTxResult: 0x%x",
                          DEVICE(s)->canonical_path, dest_addr,
                          result);
            return;
        }

        /* Increment/Decrement things. */
        if (!dest_fixed) {
            if (dest_decr) {
                dest_addr -= xfer_size;
            } else {
                dest_addr += xfer_size;
            }
        }
        if (!src_fixed) {
            if (src_decr) {
                src_addr -= xfer_size;
            } else {
                src_addr += xfer_size;
            }
        }
    }

    /* Update our current status registers. */
    s->regs[R_CSRC] = src_addr;
    s->regs[R_CDST] = dest_addr;
    s->regs[R_CTCNT] = xfer_count;

    npcm8xx_gdma_update_irq(s);
}

static void npcm8xx_gdma_channel_ctl_w(NPCM8xxGDMAChannel *s, uint32_t val)
{
    bool do_dma = FIELD_EX32(val, CTL, GDMAEN) || FIELD_EX32(val, CTL, SOFTREQ);

    /* SOFTREQ is WO. */
    val = FIELD_DP32(val, CTL, SOFTREQ, 0);

    s->regs[R_CTL] = val;
    if (do_dma) {
        npcm8xx_gdma_channel_do_dma(s);
    }
}

static void npcm8xx_gdma_channel_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    NPCM8xxGDMAChannel *s = NPCM8XX_GDMA_CHANNEL(opaque);
    uint32_t val32 = (uint32_t)value;
    uint32_t addr = offset >> 2;

    val32 &= ~npcm8xx_gdma_channel_ro[addr];
    switch (addr) {
    case R_CTL:
        npcm8xx_gdma_channel_ctl_w(s, val32);
        break;
    default:
        s->regs[addr] = val32;
        break;
    }
}

static void npcm8xx_gdma_channel_realize(DeviceState *dev, Error **errp)
{
    NPCM8xxGDMAChannel *s = NPCM8XX_GDMA_CHANNEL(dev);

    address_space_init(&s->dma_as, get_system_memory(), "gdma-dma");
    s->attrs = MEMTXATTRS_UNSPECIFIED;
}

static const MemoryRegionOps npcm8xx_gdma_channel_ops = {
    .read = npcm8xx_gdma_channel_read,
    .write = npcm8xx_gdma_channel_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void npcm8xx_gdma_channel_init(Object *obj)
{
    NPCM8xxGDMAChannel *s = NPCM8XX_GDMA_CHANNEL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &npcm8xx_gdma_channel_ops, s,
                          "npcm8xx.gdma-ch", NPCM8XX_GDMA_CHANNEL_NR_REGS << 2);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void npcm8xx_gdma_channel_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = npcm8xx_gdma_channel_realize;
    rc->phases.enter = npcm8xx_gdma_channel_enter_reset;
}

static uint64_t npcm8xx_gdma_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Read from unimplimented register at "
                  "0x%lx", DEVICE(opaque)->canonical_path, offset);
    return 0;
}

static void npcm8xx_gdma_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Write to unimplimented register at "
                  "0x%lx", DEVICE(opaque)->canonical_path, offset);
}

static void npcm8xx_gdma_realize(DeviceState *dev, Error **errp)
{
    NPCM8xxGDMA *s = NPCM8XX_GDMA(dev);

    for (int i = 0; i < ARRAY_SIZE(s->channels); i++) {
        sysbus_realize(SYS_BUS_DEVICE(&s->channels[i]), errp);
        /*
         * Alias each channel's MMIO region on top of the DMA controller's.
         * The controller itself does not have MMIO behavior, but the channels
         * do.
         */
        memory_region_add_subregion_overlap(&s->iomem,
                                            NPCM8XX_GDMA_CHANNEL_OFFSET * i,
                                            &s->channels[i].iomem, 0);
    }
}

static const MemoryRegionOps npcm8xx_gdma_ops = {
    .read = npcm8xx_gdma_read,
    .write = npcm8xx_gdma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void npcm8xx_gdma_init(Object *obj)
{
    NPCM8xxGDMA *s = NPCM8XX_GDMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    for (int i = 0; i < ARRAY_SIZE(s->channels); i++) {
        object_initialize_child(obj, "gdma-ch[*]", &s->channels[i],
                                TYPE_NPCM8XX_GDMA_CHANNEL);
        s->channels[i].controller = s;
    }

    memory_region_init_io(&s->iomem, obj, &npcm8xx_gdma_ops, s,
                          "npcm8xx.gdma", NPCM8XX_GDMA_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void npcm8xx_gdma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = npcm8xx_gdma_realize;
}

static const TypeInfo npcm8xx_gdma_channel_info = {
    .name          = TYPE_NPCM8XX_GDMA_CHANNEL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM8xxGDMAChannel),
    .class_init    = npcm8xx_gdma_channel_class_init,
    .instance_init = npcm8xx_gdma_channel_init,
};

static const TypeInfo npcm8xx_gdma_info = {
    .name          = TYPE_NPCM8XX_GDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM8xxGDMA),
    .class_init    = npcm8xx_gdma_class_init,
    .instance_init = npcm8xx_gdma_init,
};

static void npcm8xx_gdma_register_types(void)
{
    type_register_static(&npcm8xx_gdma_info);
    type_register_static(&npcm8xx_gdma_channel_info);
}

type_init(npcm8xx_gdma_register_types)

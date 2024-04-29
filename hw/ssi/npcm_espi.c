/*
 * eSPI Slave Interface Module
 *
 * Targeting the NPCM840G which supports the Intel Enhanced Serial Peripheral
 * Interface (eSPI) Revision 1.0, January 2016
 *
 * Copyright 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/irq.h"
#include "hw/registerfields.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/ssi/npcm_espi.h"
#include "trace.h"

REG32(NPCM_ESPIID, 0x0)                     /* eSPI ID */
REG32(NPCM_ESPICFG, 0x4)                    /* eSPI Configuration */
REG32(NPCM_ESPISTS, 0x8)                    /* eSPI Status */
    FIELD(NPCM_ESPISTS, VWUPD, 8, 1)        /* Virtual Wire Updated */
REG32(NPCM_ESPIIE, 0xC)                     /* eSPI Interrupt Enable */
    FIELD(NPCM_ESPIIE, VWUPDIE, 8, 1)       /* Virtual Wire Interrupt Enable */
REG32(NPCM_ESPIERR, 0x3C)                   /* eSPI Error */
    FIELD(NPCM_ESPIIE, VWERR, 11, 1)        /* Virtual Wire Access Error */
REG32(NPCM_VWGPSM, 0x180)                   /* Virtual Wire Slave-to-Master */
    FIELD(NPCM_VWGPSM, IE, 18, 1)           /* Interrupt Enable */
    FIELD(NPCM_VWGPSM, ENPLTRST, 17, 1)     /* Enable resetting with PLTRST */
    FIELD(NPCM_VWGPSM, MODIFIED, 16, 1)     /* Set on VW state change */
    FIELD(NPCM_VWGPSM, INDEX_EN, 15, 1)     /* Set to enable register */
    FIELD(NPCM_VWGPSM, VALID, 4, 4)         /* Bitfield determining validity */
                                            /* of each virtual GPIO */
    FIELD(NPCM_VWGPSM, STATE, 0, 4)         /* Value of virtual GPIO*/
REG32(NPCM_VWGPMS, 0x1C0)                   /* Virtual Wire Master-to-Slave */
    FIELD(NPCM_VWGPMS, ENESPIRST, 19, 1)    /* Enable resetting with SPIRST */
    FIELD(NPCM_VWGPMS, IE, 18, 1)           /* Interrupt Enable */
    FIELD(NPCM_VWGPMS, ENPLTRST, 17, 1)     /* Enable resetting with PLTRST */
    FIELD(NPCM_VWGPMS, MODIFIED, 16, 1)     /* Set on VW state change */
    FIELD(NPCM_VWGPMS, INDEX_EN, 15, 1)     /* Set to enable register */
    FIELD(NPCM_VWGPMS, VALID, 4, 4)         /* Bitfield determining validity */
                                            /* of each virtual GPIO */
    FIELD(NPCM_VWGPMS, STATE, 0, 4)         /* Value of virtual GPIO*/
REG32(NPCM_VWCTL, 0x2FC)                    /* Virtual Wire Control */

/* There are 16 Virtual Wire registers in each direction, each with 4 wires */
#define NPCM_ESPI_VW_REG_NUM        16

#define NPCM_ESPIID_DEFAULT         0x801
#define NPCM_ESPICFG_DEFAULT        0x3000010

static const char vw_valid[] = "vwire_valid";
static const char vw_state[] = "vwire_state";

static uint64_t npcm_espi_core_read(void *opaque, hwaddr offset, uint32_t size)
{
    NPCMESPIState *es = NPCM_ESPI(opaque);
    uint64_t ret;

    ret = es->regs[offset >> 2];
    trace_npcm_espi_read(offset, ret);
    return ret;
}

static void npcm_espi_core_write(void *opaque, hwaddr offset, uint64_t input,
    unsigned size)
{
    NPCMESPIState *es = NPCM_ESPI(opaque);

    trace_npcm_espi_write(offset, input);

    switch (offset) {
    case A_NPCM_ESPISTS:                /* eSPI Status is Write 1 to clear */
        es->regs[R_NPCM_ESPIERR] &= ~input;
        break;

    case A_NPCM_ESPIIE:
        es->regs[R_NPCM_ESPIIE] = input;
        break;

    case A_NPCM_ESPIERR:                /* eSPI Error is Write 1 to clear */
        es->regs[R_NPCM_ESPIERR] &= ~input;
        break;

    case A_NPCM_VWGPMS ... (A_NPCM_VWGPMS + NPCM_ESPI_VW_REG_NUM * 4):
        es->regs[offset >> 2] = input;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: write: data=0x$x offset=0x%lx\n",
                      __func__, offset);
        es->regs[offset >> 2] = input;
        break;
    }
}

static void npcm_espi_enter_reset(Object *dev, ResetType type)
{
    NPCMESPIState *es = NPCM_ESPI(dev);

    es->regs[R_NPCM_ESPIID] = NPCM_ESPIID_DEFAULT;
    es->regs[R_NPCM_ESPICFG] = NPCM_ESPICFG_DEFAULT;
}

static const MemoryRegionOps npcm_espi_memops = {
    .read = npcm_espi_core_read,
    .write = npcm_espi_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    }
};

static void npcm_vwire_get(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    NPCMESPIState *es = NPCM_ESPI(obj);
    uint64_t value = 0;

    if (strncmp(name, vw_state, sizeof(vw_state)) == 0) {
        for (int i = NPCM_ESPI_VW_REG_NUM - 1; i >= 0; i--) {
            value |= es->regs[R_NPCM_VWGPMS + i] & R_NPCM_VWGPMS_STATE_MASK;
            value <<= R_NPCM_VWGPMS_STATE_LENGTH;
        }
    } else if (strncmp(name, vw_valid, sizeof(vw_valid)) == 0) {
        for (int i = NPCM_ESPI_VW_REG_NUM - 1; i >= 0; i--) {
            value |= extract32(es->regs[R_NPCM_VWGPMS + i],
                R_NPCM_VWGPMS_VALID_SHIFT, R_NPCM_VWGPMS_VALID_LENGTH);
            value <<= R_NPCM_VWGPMS_VALID_LENGTH;
        }
    }

    visit_type_uint64(v, name, &value, errp);
}

static void npcm_vwire_valid_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp) {
    NPCMESPIState *es = NPCM_ESPI(obj);
    uint64_t value = 0;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    for (int i = 0; i < NPCM_ESPI_VW_REG_NUM;
         i++, value >>= R_NPCM_VWGPMS_VALID_LENGTH) {
        es->regs[R_NPCM_VWGPMS + i] = deposit32(es->regs[R_NPCM_VWGPMS + i],
            R_NPCM_VWGPMS_VALID_SHIFT, R_NPCM_VWGPMS_VALID_LENGTH, value);
        if (value & MAKE_64BIT_MASK(0, R_NPCM_VWGPMS_VALID_LENGTH)) {
            es->regs[R_NPCM_VWGPMS + i] |= R_NPCM_VWGPMS_INDEX_EN_MASK;
        }
    }
}

static void npcm_vwire_state_set(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    NPCMESPIState *es = NPCM_ESPI(obj);
    uint64_t value = 0;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    for (int i = 0; i < NPCM_ESPI_VW_REG_NUM;
         i++, value >>= R_NPCM_VWGPMS_STATE_LENGTH) {
        es->regs[R_NPCM_VWGPMS + i] &= ~R_NPCM_VWGPMS_STATE_MASK;
        es->regs[R_NPCM_VWGPMS + i] = deposit32(es->regs[R_NPCM_VWGPMS + i],
            R_NPCM_VWGPMS_STATE_SHIFT, R_NPCM_VWGPMS_STATE_LENGTH, value);
    }

}

static void npcm_espi_realize(DeviceState *dev, Error **errp)
{
    NPCMESPIState *es = NPCM_ESPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&es->mmio, OBJECT(es), &npcm_espi_memops, es,
                          TYPE_NPCM_ESPI, 4 * KiB);

    sysbus_init_mmio(sbd, &es->mmio);
    sysbus_init_irq(sbd, &es->irq);

    object_property_add(OBJECT(dev), vw_valid, "uint64_t",
                        npcm_vwire_get, npcm_vwire_valid_set, NULL, NULL);
    object_property_add(OBJECT(dev), vw_state, "uint64_t",
                        npcm_vwire_get, npcm_vwire_state_set, NULL, NULL);

}

static void npcm_espi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NPCM eSPI";
    dc->realize = npcm_espi_realize;
    rc->phases.enter = npcm_espi_enter_reset;
}

static const TypeInfo npcm_espi_types[] = {
    {
        .name = TYPE_NPCM_ESPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .class_init = npcm_espi_class_init,
        .instance_size = sizeof(NPCMESPIState),
    },
};

DEFINE_TYPES(npcm_espi_types)

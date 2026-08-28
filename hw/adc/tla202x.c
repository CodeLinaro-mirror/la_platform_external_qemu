/*
 * I2C Fake TLA202X object
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/i2c/i2c.h"
#include "qemu/log.h"
#include "qom/object.h"

#define TYPE_I2C_TLA202X "tla202x"

OBJECT_DECLARE_SIMPLE_TYPE(TLA202XState, I2C_TLA202X)

#define CONVERSION_DATA_REG         0x0
#define CONFIGURATION_REG           0x1
#define CONFIGURATION_REG_DEFAULT   0x8583

/*
 * struct TLA202XState - The TLA202X state object.
 */
typedef struct TLA202XState {
    I2CSlave parent;
    uint16_t cfg_reg;
    uint16_t conversion_data_reg;
    bool pending_msb;
    bool pending_cfg;
    bool pending_conversion;
} TLA202XState;

static int tla202x_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    switch (event) {
    case I2C_START_RECV:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: START_RECV\n", __func__);
        break;

    case I2C_START_SEND:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: START_SEND\n", __func__);
        break;

    case I2C_FINISH:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: FINISH\n", __func__);
        break;

    case I2C_NACK:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: NACK\n", __func__);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unknown event 0x%x\n",
                      __func__, event);
        return -1;
    }

    return 0;
}

static uint8_t tla202x_receive_byte(I2CSlave *dev)
{
    TLA202XState *t = I2C_TLA202X(dev);
    /* If we're pending a config read, the next two reads do that */
    if (t->pending_cfg) {
        if (t->pending_msb) {
            t->pending_msb = false;
            return (uint8_t)(t->cfg_reg >> 8);
        } else {
            t->pending_cfg = false;
            return (uint8_t)(t->cfg_reg & 0xFF);
        }
    }

    /* Do the same thing for a conversion data read */
    if (t->pending_conversion) {
        if (t->pending_msb) {
            t->pending_msb = false;
            return (uint8_t)(t->conversion_data_reg >> 8);
        } else {
            t->pending_cfg = false;
            return (uint8_t)(t->conversion_data_reg & 0xFF);
        }
    }
    /* Always return 0. */
    qemu_log_mask(LOG_GUEST_ERROR, "%s with unknown command\n", __func__);
    return 0;
}

static int tla202x_write_data(I2CSlave *dev, uint8_t data)
{
    TLA202XState *t = I2C_TLA202X(dev);
    /* If we're pending a config write, the next two writes do that */
    if (t->pending_cfg) {
        if (t->pending_msb) {
            t->pending_msb = false;
            t->cfg_reg = (data << 8);
        } else {
            t->pending_cfg = false;
            t->cfg_reg |= data ;
        }
        return 0;
    }

    switch (data) {
    case CONFIGURATION_REG:
        t->pending_cfg = true;
        t->pending_msb = true;
        break;
    case CONVERSION_DATA_REG:
        t->pending_conversion = true;
        t->pending_msb = true;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: with unknown data: %d\n",
            __func__, data);
    }

    /* Always return 0 */
    return 0;
}

static void tla202x_realize(DeviceState *dev, Error **errp)
{
    TLA202XState *t = I2C_TLA202X(dev);
    t->cfg_reg = CONFIGURATION_REG_DEFAULT;
    t->conversion_data_reg = 0;
    t->pending_cfg = false;
    t->pending_conversion = false;
    t->pending_msb = false;
}

static void tla202x_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tla202x_realize;

    sc->event = tla202x_i2c_event;
    sc->recv = tla202x_receive_byte;
    sc->send = tla202x_write_data;
}

static const TypeInfo tla202x_info = {
    .name          = TYPE_I2C_TLA202X,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TLA202XState),
    .class_init    = tla202x_class_init,
};

static void tla202x_register_types(void)
{
    type_register_static(&tla202x_info);
}

type_init(tla202x_register_types)

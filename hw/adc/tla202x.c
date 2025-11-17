/*
 * I2C TLA202X object
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

/*
 * struct TLA202XState - The TLA202X state object.
 */
typedef struct TLA202XState {
    I2CSlave parent;
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
    /* Always return 0. */
    qemu_log_mask(LOG_GUEST_ERROR, "%s\n", __func__);
    return 0;
}

static int tla202x_write_data(I2CSlave *dev, uint8_t data)
{
    /* Always return 0. */
    qemu_log_mask(LOG_GUEST_ERROR, "%s: data: %d\n", __func__, data);
    return 0;
}

static void tla202x_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = tla202x_i2c_event;
    k->recv = tla202x_receive_byte;
    k->send = tla202x_write_data;
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

/*
 * Linear Technology LTC2991 Octal I2C Voltage, Current, and Temperature Monitor
 *
 * Copyright 2025 Google LLC
 *
 * Datasheet:
 * https://www.analog.com/media/en/technical-documentation/data-sheets/2991ff.pdf
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/sensor/ltc2991.h"
#include <stdint.h>
#include <string.h>
#include "hw/core/registerfields.h"
#include "qapi/visitor.h"
#include "trace.h"

#define LTC2991_NUM_VOLTAGE         8
#define LTC2991_NUM_TEMPERATURE     4

static uint8_t ltc2991_recv(I2CSlave *i2c)
{
    LTC2991State *s = LTC2991(i2c);
    uint8_t data;

    if (s->reg < LTC2991_NUM_REGS) {
        data = s->regs[s->reg];
    } else {
        data = 0xff;
    }

    /* TODO: b/406796187: reading a sensor reg should clear its freshness bit */

    trace_ltc2991_recv(DEVICE(s)->canonical_path, s->reg, data);
    return data;
}

static int ltc2991_send(I2CSlave *i2c, uint8_t data)
{
    LTC2991State *s = LTC2991(i2c);

    if (s->i2c_cmd) {
        s->reg = data;
        s->i2c_cmd = false;
        return 0;
    }

    trace_ltc2991_send(DEVICE(s)->canonical_path, s->reg, data);

    if (s->reg < LTC2991_NUM_REGS) {
        s->regs[s->reg] = data;
    } else {
        return -1;
    }

    return 0;
}

static int ltc2991_event(I2CSlave *i2c, enum i2c_event event)
{
    LTC2991State *s = LTC2991(i2c);

    switch (event) {
    case I2C_START_RECV:
        trace_ltc2991_event(DEVICE(s)->canonical_path, "START_RECV");
        break;

    case I2C_START_SEND:
        trace_ltc2991_event(DEVICE(s)->canonical_path, "START_SEND");
        s->i2c_cmd = true;
        break;

    case I2C_FINISH:
        trace_ltc2991_event(DEVICE(s)->canonical_path, "FINISH");
        break;

    case I2C_NACK:
        trace_ltc2991_event(DEVICE(s)->canonical_path, "NACK");
        break;

    default:
        return -1;
    }
    return 0;
}

/* TODO: b/406796187: implement QMP voltage/temperature adjustments */

static void ltc2991_realize(DeviceState *dev, Error **errp)
{
    LTC2991State *s = LTC2991(dev);

    memset(s->regs, 0, sizeof(s->regs));

    for (int i = 0; i < LTC2991_NUM_VOLTAGE; i += 2) {
        s->regs[LTC2991_REG_V1_MSB + i] = 0x9F;         /* 2.5V */
        s->regs[LTC2991_REG_V1_MSB + i + 1] = 0xFF;     /* LSB */
    }

    /* 25°C Temperature */
    s->regs[LTC2991_REG_T_INTERNAL_MSB] = 0x01;
    s->regs[LTC2991_REG_T_INTERNAL_MSB] = 0x90;
}

static void ltc2991_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = ltc2991_realize;
    k->event = ltc2991_event;
    k->recv = ltc2991_recv;
    k->send = ltc2991_send;
}

static const TypeInfo ltc2991_types[] = {
    {
        .name = TYPE_LTC2991,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(LTC2991State),
        .class_init = ltc2991_class_init,
    }
};

DEFINE_TYPES(ltc2991_types)

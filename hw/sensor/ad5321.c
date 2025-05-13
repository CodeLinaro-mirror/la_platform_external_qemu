/*
 * Analog Devices Buffered Voltage Output 12 bit DAC (AD5321)
 * https://www.analog.com/media/en/technical-documentation/data-sheets/AD5301_5311_5321.pdf
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "trace.h"
#define TYPE_AD5321 "ad5321"

OBJECT_DECLARE_SIMPLE_TYPE(AD5321State, AD5321)

/*
 * struct AD5321State - The AD5321 state object.
 */
typedef struct AD5321State {
    I2CSlaveClass parent;
    uint16_t reg;
    uint8_t reg_msb;
    bool is_msb;
} AD5321State;

static int ad5321_event(I2CSlave *i2c, enum i2c_event event)
{
    AD5321State *s = AD5321(i2c);
    switch (event) {
    case I2C_START_RECV:
       trace_ad5321_event(DEVICE(s)->canonical_path, "START_RECV");
       s->is_msb = true;
       break;

    case I2C_START_SEND:
       trace_ad5321_event(DEVICE(s)->canonical_path, "START_SEND");
       s->is_msb = true;
       break;

    case I2C_FINISH:
       trace_ad5321_event(DEVICE(s)->canonical_path, "FINISH");
       break;

    case I2C_NACK:
       trace_ad5321_event(DEVICE(s)->canonical_path, "NACK");
       break;

    default:
       qemu_log_mask(LOG_GUEST_ERROR, "%s: unknown event 0x%x\n",
                      __func__, event);
       return -1;
    }
    return 0;
}

static uint8_t ad5321_recv(I2CSlave *i2c)
{
    AD5321State *s = AD5321(i2c);
    uint8_t data;

    if (s->is_msb) {
        data = s->reg >> 8;
        s->is_msb = false;
    } else {
        data = (uint8_t)s->reg;
        s->is_msb = true;
    }
    trace_ad5321_recv(DEVICE(s)->canonical_path, data, s->reg);
    return data;
}

static int ad5321_send(I2CSlave *i2c, uint8_t data)
{
    /*
     * During the write cycle, each multiple of
     * two data bytes write updates the DAC output
     */
    AD5321State *s = AD5321(i2c);
    if (s->is_msb) {
        s->reg_msb = data;
        s->is_msb = false;
    } else {
        s->reg = data | (s->reg_msb << 8);
        s->is_msb = true;
    }
    trace_ad5321_send(DEVICE(s)->canonical_path, data, s->reg);
    return 0;
}

static void ad5321_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->event = ad5321_event;
    sc->recv = ad5321_recv;
    sc->send = ad5321_send;
}

static const TypeInfo ad5321_info = {
    .name          = TYPE_AD5321,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AD5321State),
    .class_init    = ad5321_class_init,
};

static void ad5321_register_types(void)
{
    type_register_static(&ad5321_info);
}

type_init(ad5321_register_types)

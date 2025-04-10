/*
 * PI7C9X3G606GP PCIe Switch Management Module
 *
 * Copyright 2025 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "hw/i2c/smbus_slave.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define TYPE_PI7C9X3G606GP "pi7c9x3g606gp"
OBJECT_DECLARE_SIMPLE_TYPE(PI7C9X3G606GPState, PI7C9X3G606GP)

#define PI7C_VERSION_ADDR       (0x700 >> 2)
#define PI7C_TEMP_ADDR(i)       ((0x5d8 >> 2) + (i))
#define PI7C_TEMP_COUNT         3
#define PI7C_WRITE_CMD          3
#define PI7C_READ_CMD           4
#define PI7C_REG_SIZE           (1 << 10)

/*
 * From Diodes Pi7c9x3g606gp Datasheet DS43484 Rev 1-2
 * Table 5-11 Page 43/303.
 */
#define PI7C_COMMAND(c)         extract32((c), 0, 3)
#define PI7C_PORT(c) \
        ((extract32((c), 8, 4) << 1) + extract32((c), 17, 1))
#define PI7C_BYTE_ENABLED(c)    extract32((c), 18, 4)
#define PI7C_ADDRESS(c) \
        ((extract32((c), 16, 2) << 8) + extract32((c), 24, 8))

#define PI7C_DEFAULT_VERSION    0x01020304
#define PI7C_TEMP_MASK          0xfff00
#define PI7C_TEMP_DATA_READY    0x800000

/*
 * PI7C9X3G606GPState:
 * @data: internal register data.
 * @address: The address to be read/written by the host.
 * @bytes_enabled: indicate if switch register byte will be modified.
 * @port: indicate which port to access.
 */
typedef struct PI7C9X3G606GPState {
    SMBusDevice parent;

    uint32_t data[PI7C_REG_SIZE];
    uint32_t address;
    uint8_t bytes_enabled;
    uint8_t port;
    uint8_t bytes_transferred;
} PI7C9X3G606GPState;

static uint8_t pi7c9x3g606gp_read_byte(SMBusDevice *d)
{
    PI7C9X3G606GPState *s = PI7C9X3G606GP(d);
    uint8_t value = 0;

    if (unlikely(s->address >= PI7C_REG_SIZE)) {
        qemu_log_mask(LOG_GUEST_ERROR, "Reading from 0x%" PRIx32
             "out of range.", s->address);
        return 0;
    }

    if (unlikely(s->bytes_transferred >= 4)) {
        qemu_log_mask(LOG_GUEST_ERROR, "Reading from 0x%" PRIx32
             "reading too many bytes.", s->address);
        return 0;
    }
    /* Reading is big endian for this device. */
    if (s->bytes_enabled && BIT(s->bytes_transferred)) {
        /* Only read bytes that are enabled. */
        value = extract32(s->data[s->address],
                          24 - s->bytes_transferred * 8, 8);
    }
    ++s->bytes_transferred;
    trace_pi7c9x3g606gp_read_byte(DEVICE(s)->canonical_path, value);
    return value;
}

static void pi7c9x3g606gp_parse_cmd(PI7C9X3G606GPState *s, uint32_t cmd)
{
    s->bytes_enabled = PI7C_BYTE_ENABLED(cmd);
    s->port = PI7C_PORT(cmd);
    s->address = PI7C_ADDRESS(cmd);
    s->bytes_transferred = 0;
    trace_pi7c9x3g606gp_byte_enabled(DEVICE(s)->canonical_path,
        s->bytes_enabled);
    trace_pi7c9x3g606gp_port(DEVICE(s)->canonical_path, s->port);
    trace_pi7c9x3g606gp_address(DEVICE(s)->canonical_path, s->address << 2);
}

static void pi7c9x3g606gp_write_data(
    PI7C9X3G606GPState *s, uint8_t *buf, uint8_t len)
{
    if (s->address >= PI7C_TEMP_ADDR(0) &&
        s->address < PI7C_TEMP_ADDR(PI7C_TEMP_COUNT)) {
        /* temperature register write. Do not overwrite the temperature. */
        return;
    }
    /* Write data as desired. */
    s->data[s->address] = 0;
    for (int i = 0; i < len - 4; ++i) {
        if (unlikely(i >= 4)) {
            /* Excessive bytes. */
            qemu_log_mask(LOG_GUEST_ERROR, "Writing to 0x%" PRIx32
                 "has too many bytes: %" PRIu8, s->address, len);
            return;
        }
        /* Only write bytes that are enabled. */
        if (likely(s->bytes_enabled && BIT(i))) {
            /* Writing is big endian for this device. */
            trace_pi7c9x3g606gp_write_byte(DEVICE(s)->canonical_path,
                                           buf[i + 4]);
            s->data[s->address] =
                deposit32(s->data[s->address], 24 - i * 8, 8, buf[i + 4]);
        }
    }
    trace_pi7c9x3g606gp_write_reg(
        DEVICE(s)->canonical_path, s->address << 2, s->data[s->address]);
}

static int pi7c9x3g606gp_write_buf(SMBusDevice *d, uint8_t *buf, uint8_t len)
{
    PI7C9X3G606GPState *s = PI7C9X3G606GP(d);
    uint32_t cmd;
    uint8_t op;

    if (len < 4) {
        /* not enough command bytes. */
        qemu_log_mask(LOG_GUEST_ERROR, "Command bytes not enough: %" PRIu8,
                      len);
        return 0;
    }
    cmd = *(uint32_t *)buf;
    pi7c9x3g606gp_parse_cmd(s, cmd);
    if (unlikely(s->address >= PI7C_REG_SIZE)) {
        qemu_log_mask(LOG_GUEST_ERROR, "Accessing 0x%" PRIx32
             " out of range.", s->address);
        return 4;
    }
    op = PI7C_COMMAND(cmd);
    switch (op) {
    case PI7C_WRITE_CMD:
        pi7c9x3g606gp_write_data(s, buf + 4, len - 4);
        break;
    case PI7C_READ_CMD:
        trace_pi7c9x3g606gp_read_reg(DEVICE(s)->canonical_path,
            s->address << 2, s->data[s->address]);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Unknown command: 0x%" PRIx32, op);
    }

    return len;
}

/*
 * The formula is from Diodes Pi7c9x3g606gp Datasheet Rev 0.8
 * Section 9, page 37/53
 */
static void pi7c9x3g606gp_get_temperature(Object *obj, Visitor *v,
    const char *name, void *opaque, Error **errp) {
    uint32_t n = *(uint32_t *)opaque;
    int32_t temp;

    n = extract32(n, 8, 12);
    temp = (int32_t)((((double)n / 4094) * 237.7 - 79.925) * 1000);
    visit_type_int32(v, name, &temp, errp);
}

/*
 * The formula is from Diodes Pi7c9x3g606gp Datasheet Rev 0.8
 * Section 9, page 37/53
 */
static void pi7c9x3g606gp_set_temperature(Object *obj, Visitor *v,
    const char *name, void *opaque, Error **errp) {
    int32_t temp;
    uint32_t *n = (uint32_t *)opaque;

    if (!visit_type_int32(v, name, &temp, errp)) {
        return;
    }
    temp = ((double)temp / 1000 + 79.925) / 237.7 * 4094;
    /*
     * The converted temperature is stored as unsigned in the register.
     * Avoid overflow if the temperature value is too low.
     */
    if (temp < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "Temperature %" PRIx32
             "out of range.", temp);
        temp = 0;
    }
    *n = PI7C_TEMP_DATA_READY | (temp << 8);
}

static const VMStateDescription vmstate_pi7c9x3g606gp = {
    .name = "PI7C9X3G606GP",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(data, PI7C9X3G606GPState, PI7C_REG_SIZE),
        VMSTATE_UINT32(address, PI7C9X3G606GPState),
        VMSTATE_UINT8(bytes_enabled, PI7C9X3G606GPState),
        VMSTATE_UINT8(port, PI7C9X3G606GPState),
        VMSTATE_END_OF_LIST()
    }
};

static void pi7c9x3g606gp_enter_reset(Object *obj, ResetType type)
{
    PI7C9X3G606GPState *s = PI7C9X3G606GP(obj);

    s->address = 0;
    s->bytes_enabled = 0;
    s->port = 0;

    /* Set default version. */
    stl_le_p(&s->data[PI7C_VERSION_ADDR], PI7C_DEFAULT_VERSION);
}

static void pi7c9x3g606gp_init(Object *obj)
{
    PI7C9X3G606GPState *s = PI7C9X3G606GP(obj);

    /* Current temperature in millidegrees. */
    for (int i = 0; i < PI7C_TEMP_COUNT; ++i) {
        object_property_add(obj, "temp[*]", "int32",
                            pi7c9x3g606gp_get_temperature,
                            pi7c9x3g606gp_set_temperature, NULL,
                            &s->data[PI7C_TEMP_ADDR(i)]);
    }
}

static void pi7c9x3g606gp_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *k = SMBUS_DEVICE_CLASS(klass);

    dc->desc = "PI7C9X3G606GP";
    dc->vmsd = &vmstate_pi7c9x3g606gp;
    k->write_data = pi7c9x3g606gp_write_buf;
    k->receive_byte = pi7c9x3g606gp_read_byte;
    rc->phases.enter = pi7c9x3g606gp_enter_reset;
}

static const TypeInfo pi7c9x3g606gp_info[] = {
    {
        .name          = TYPE_PI7C9X3G606GP,
        .parent        = TYPE_SMBUS_DEVICE,
        .instance_size = sizeof(PI7C9X3G606GPState),
        .instance_init = pi7c9x3g606gp_init,
        .class_init    = pi7c9x3g606gp_class_init,
    },
};
DEFINE_TYPES(pi7c9x3g606gp_info)


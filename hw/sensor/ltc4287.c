/*
 * Analog Devices LTC4287 PMBus High Power Positive Hot Swap Controller
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include "qemu/osdep.h"
#include "hw/i2c/pmbus_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"

#define TYPE_LTC4287 "ltc4287"
OBJECT_DECLARE_SIMPLE_TYPE(LTC4287State, LTC4287)

#define LTC4287_NUM_PAGES       1

#define LTC4287_DEFAULT_OPERATION               0x80 /* Device Enabled */
#define LTC4287_DEFAULT_CAPABILITY              0x20 /* 1MHz bus speed */
#define LTC4287_DEFAULT_VOUT_MODE               0x40 /* DIRECT Mode */
#define LTC4287_DEFAULT_REVISION                0x11 /* Latest */

#define LTC4287_MFR_REBOOT_CONTROL 0xFD

typedef struct LTC4287State {
    PMBusDevice parent;
    uint8_t mfr_revision;
    uint8_t mfr_reboot_control;
} LTC4287State;

static uint8_t ltc4287_read_byte(PMBusDevice *pmdev)
{
    LTC4287State *s = LTC4287(pmdev);

    switch (pmdev->code) {
    case PMBUS_MFR_ID:
        pmbus_send_string(pmdev, "LTC");
        break;

    case PMBUS_MFR_MODEL:
        pmbus_send_string(pmdev, "LTC4287");
        break;

    /* TODO(b/374361893): confirm that send8 has the same semantics as rd_block
     * for a single byte */
    case PMBUS_MFR_REVISION:
        pmbus_send8(pmdev, s->mfr_revision);
        break;

    case PMBUS_IC_DEVICE_ID:
        pmbus_send_string(pmdev, "LTC4287");
        break;

    /* TODO(b/374361893): confirm that send8 has the same semantics as rd_block
     * for a single byte */
    case PMBUS_IC_DEVICE_REV:
        pmbus_send8(pmdev, s->mfr_revision);
        break;

    case LTC4287_MFR_REBOOT_CONTROL:
        pmbus_send8(pmdev, s->mfr_reboot_control);
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: reading from unimplemented register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        return PMBUS_ERR_BYTE;
    }
    return 0;
}

static int ltc4287_write_data(PMBusDevice *pmdev, const uint8_t *buf,
                              uint8_t len)
{
    LTC4287State *s = LTC4287(pmdev);

    switch (pmdev->code) {

    case PMBUS_MFR_ID:
    case PMBUS_MFR_MODEL:
    case PMBUS_MFR_REVISION:
    case PMBUS_IC_DEVICE_ID:
    case PMBUS_IC_DEVICE_REV:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s: writing to read only register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        break;

    case LTC4287_MFR_REBOOT_CONTROL:
        s->mfr_reboot_control = pmbus_receive8(pmdev);
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: %s: writing to unimplemented register: 0x%02x\n",
                      DEVICE(s)->canonical_path, __func__, pmdev->code);
        return 0;
    }
    return 0;
}

static void ltc4287_exit_reset(Object *obj, ResetType type)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    LTC4287State *s = LTC4287(obj);

    pmdev->capability = LTC4287_DEFAULT_CAPABILITY;
    pmdev->pages[0].operation = LTC4287_DEFAULT_OPERATION;
    pmdev->pages[0].vout_mode = LTC4287_DEFAULT_VOUT_MODE;
    pmdev->pages[0].revision = LTC4287_DEFAULT_REVISION;
    s->mfr_revision = LTC4287_DEFAULT_REVISION;
    s->mfr_reboot_control = 0x24;

    /* random sensor readings */
    pmdev->pages[0].read_vin = 0x100;
    pmdev->pages[0].read_vout = 0x100;
    pmdev->pages[0].read_iin = 0x10;
    pmdev->pages[0].read_temperature_1 = 30;
    pmdev->pages[0].read_iout = 0x10;
    pmdev->pages[0].read_pin = 0x110;
}

static void ltc4287_get(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    uint32_t value;

    value = *(uint16_t *)opaque;

    if (!(strcmp(name, "temperature") == 0 || strcmp(name, "pin") == 0)) {
        value *= 1000; /* use milliunits for qmp */
    }

    visit_type_uint32(v, name, &value, errp);
}

static void ltc4287_set(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    uint16_t *internal = opaque;
    uint32_t value;
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    if (!(strcmp(name, "temperature") == 0 || strcmp(name, "pin") == 0)) {
        /* use milliunits for qmp */
        value /= 1000;
    }

    if (strcmp(name, "vout") == 0) {
        *internal = (uint16_t)value;
    } else {
        *internal = value;
    }
    pmbus_check_limits(pmdev);
}

static const VMStateDescription vmstate_ltc4287 = {
    .name = TYPE_LTC4287,
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]){
        VMSTATE_PMBUS_DEVICE(parent, LTC4287State),
        VMSTATE_END_OF_LIST()
    }
};

static void ltc4287_init(Object *obj)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    uint64_t psu_flags = PB_HAS_VIN | PB_HAS_VOUT | PB_HAS_VOUT_MARGIN |
                         PB_HAS_VOUT_MODE | PB_HAS_IIN | PB_HAS_TEMPERATURE |
                         PB_HAS_IOUT | PB_HAS_PIN;

    pmbus_page_config(pmdev, 0, psu_flags);
    object_property_add(obj, "vin", "uint32", ltc4287_get, ltc4287_set,
                        NULL, &pmdev->pages[0].read_vin);
    object_property_add(obj, "iin", "uint32", ltc4287_get, ltc4287_set,
                        NULL, &pmdev->pages[0].read_iin);
    object_property_add(obj, "vout", "uint32", ltc4287_get, ltc4287_set,
                        NULL, &pmdev->pages[0].read_vout);
    object_property_add(obj, "temperature", "uint32", ltc4287_get, ltc4287_set,
                        NULL, &pmdev->pages[0].read_temperature_1);
    object_property_add(obj, "iout", "uint32", ltc4287_get, ltc4287_set,
                        NULL, &pmdev->pages[0].read_iout);
    object_property_add(obj, "pin", "uint32", ltc4287_get, ltc4287_set,
                        NULL, &pmdev->pages[0].read_pin);
}

static void ltc4287_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    PMBusDeviceClass *k = PMBUS_DEVICE_CLASS(klass);

    dc->desc = "Analog Devices LTC4287 Hot Swap Controller";
    dc->vmsd = &vmstate_ltc4287;
    k->write_data = ltc4287_write_data;
    k->receive_byte = ltc4287_read_byte;
    k->device_num_pages = LTC4287_NUM_PAGES;
    rc->phases.exit = ltc4287_exit_reset;
}

static const TypeInfo ltc4287_info = {
    .name = TYPE_LTC4287,
    .parent = TYPE_PMBUS_DEVICE,
    .instance_size = sizeof(LTC4287State),
    .instance_init = ltc4287_init,
    .class_init = ltc4287_class_init,
};

static void ltc4287_register_types(void)
{
    type_register_static(&ltc4287_info);
}

type_init(ltc4287_register_types)

/*
 * MAX31732 temperature sensor
 *
 * Features:
 *  - 4 external diode temperature inputs
 *
 * Copyright 2023 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sensor/max31732.h"
#include "libqos/i2c.h"
#include "libqos/libqos-malloc.h"
#include "libqos/qgraph.h"
#include "libqtest-single.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define TEST_ID "max31732-test"


static int32_t qmp_max31732_get(const char *id, const char *property)
{
    QDict *response;
    int32_t ret;
    response = qmp("{ 'execute': 'qom-get', 'arguments': { 'path': %s, "
                   "'property': %s } }", id, property);
    g_assert(qdict_haskey(response, "return"));
    ret = qdict_get_int(response, "return");
    qobject_unref(response);
    return ret;
}

static void qmp_max31732_set(const char *id,
                             const char *property,
                             int32_t value)
{
    QDict *response;

    response = qmp("{ 'execute': 'qom-set', 'arguments': { 'path': %s, "
                   "'property': %s, 'value': %d } }",
                   id, property, value);
    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}

/* Test read write registers can be written to */
static void test_read_write(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t value;
    int32_t temperature; /* millidegrees */

    /* R/W should be writable */
    i2c_set8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE, 0x1A);
    value = i2c_get8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE);
    g_assert_cmphex(value, ==, 0x1A);

    i2c_set8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE, 0x1A);
    value = i2c_get8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE);
    g_assert_cmphex(value, ==, 0x1A);

    /* RO should not be writable*/
    i2c_set8(i2cdev, A_REMOTE_1_TEMPERATURE, 0xAB);
    value = i2c_get8(i2cdev, A_REMOTE_1_TEMPERATURE);
    g_assert_cmphex(value, !=, 0x1F);

    i2c_set8(i2cdev, A_THERMAL_STATUS_HIGH_TEMPERATURE, 0xCD);
    value = i2c_get8(i2cdev, A_THERMAL_STATUS_HIGH_TEMPERATURE);
    g_assert_cmphex(value, !=, 0xCD);

    /* QMP should read and write temperatures */
    i2c_set8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE, 0x1F);

    qmp_max31732_set(TEST_ID, "temperature[0]", 127500);
    temperature = qmp_max31732_get(TEST_ID, "temperature[0]");
    g_assert_cmpint(temperature, ==, 127500);

    qmp_max31732_set(TEST_ID, "temperature[4]", 127875);
    temperature = qmp_max31732_get(TEST_ID, "temperature[4]");
    g_assert_cmpint(temperature, ==, 127875);

    qmp_max31732_set(TEST_ID, "temperature[1]", 500);
    temperature = qmp_max31732_get(TEST_ID, "temperature[1]");
    g_assert_cmpint(temperature, ==, 500);

    qmp_max31732_set(TEST_ID, "temperature[2]", -500);
    temperature = qmp_max31732_get(TEST_ID, "temperature[2]");
    g_assert_cmpint(temperature, ==, -500);

    qmp_max31732_set(TEST_ID, "temperature[3]", -63875);
    temperature = qmp_max31732_get(TEST_ID, "temperature[3]");
    g_assert_cmpint(temperature, ==, -63875);
}
/**
 *
 *  Test Status Registers
 *
 *  The thermal status registers indicate over-temperature and under-temperature
 *  faults.
 *  - The Primary Thermal High Status register indicates whether a local or
 *      remote temperature has exceeded threshold limits set in the associated
 *      Primary Over-Temperature Threshold registers.
 *  - The Primary Thermal Low Status register indicates whether the measured
 *      temperature has fallen below the threshold limit set in the
 *      *All Channel Primary Under-Temperature Threshold* registers for the
 *      local or remote sensing diodes.
 *
 *  Bits in the thermal status registers are cleared by a successful read but
 *  set again after the next conversion unless the fault is corrected, either by
 *  a change in the measured temperature or by a change in the threshold
 *  temperature.
 */
/* Test over temperature status */
static void test_primary_over_temperature(void *obj,
    void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    char *path;
    uint8_t value;

    /* enable all channels */
    i2c_set8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE, 0x1F);

    /* set thresholds to [60C - 64C] */
    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        i2c_set8(i2cdev,
                 A_REMOTE_1_PRIMARY_OVER_TEMPERATURE_THRESHOLD + (2 * i),
                 60 + i);
    }

    /* use qmp to set temperatures to [61C - 65C] so each diode is 1C over */
    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max31732_set(TEST_ID, path, 61000 + i * 1000);

    }
    /* check that the over temperature status gets raised */
    value = i2c_get8(i2cdev, A_THERMAL_STATUS_HIGH_TEMPERATURE);
    g_assert_cmphex(value, ==, 0x1F);

    /* use qmp to set temperatures to [59C - 63C] so each diode is 1C under */
    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max31732_set(TEST_ID, path, 59000 + i * 1000);

    }

    /* check that the over temperature status gets lowered */
    value = i2c_get8(i2cdev, A_THERMAL_STATUS_HIGH_TEMPERATURE);
    g_assert_cmphex(value, ==, 0);
}

/* Test secondary over temperature status */
static void test_secondary_over_temperature(void *obj,
    void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    char *path;
    uint8_t value;

    /* enable all channels */
    i2c_set8(i2cdev, A_TEMPERATURE_CHANNEL_ENABLE, 0x1F);

    /* set thresholds to [-62C..-58C] */
    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        i2c_set8(i2cdev,
                 A_REMOTE_1_SECONDARY_THRESHOLD_HIGH_LIMIT + i,
                 -62 + i);
    }

    /* use qmp to set temperatures to [-61C..-57C] so each diode is 1C over */
    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max31732_set(TEST_ID, path, -61000 + i * 1000);
    }
    /* check that the over temperature status gets raised */
    value = i2c_get8(i2cdev, A_SECONDARY_THERMAL_STATUS_HIGH_TEMPERATURE);
    g_assert_cmphex(value, ==, 0x1F);

    /* use qmp to set temperatures to [-63C..-59C] so each diode is 1C under */
    for (int i = 0; i < MAX31732_NUM_TEMPS; i++) {
        path = g_strdup_printf("temperature[%d]", i);
        qmp_max31732_set(TEST_ID, path, -63000 + i * 1000);

    }

    /* check that the over temperature status gets lowered */
    value = i2c_get8(i2cdev, A_SECONDARY_THERMAL_STATUS_HIGH_TEMPERATURE);
    g_assert_cmphex(value, ==, 0);
}

static void max31732_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" TEST_ID ",address=0x1B"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { 0x1B });

    qos_node_create_driver("max31732", i2c_device_create);
    qos_node_consumes("max31732", "i2c-bus", &opts);

    qos_add_test("test_read_write", "max31732", test_read_write, NULL);
    qos_add_test("test_primary_over_temp", "max31732",
                 test_primary_over_temperature, NULL);
    qos_add_test("test_secondary_over_temp", "max31732",
                 test_secondary_over_temperature, NULL);
}
libqos_init(max31732_register_nodes);

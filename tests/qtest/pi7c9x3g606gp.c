/*
 * QTest testcase for PI7C9X3G606GP PCIe switch manager
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

#include "libqtest-single.h"
#include "libqos/qgraph.h"
#include "libqos/i2c.h"
#include "qobject/qdict.h"
#include "qemu/bitops.h"

#define PI7C_TEST_ID   "pi7c9x3g606gp-test"
#define PI7C_TEST_ADDR 0x6f

#define PI7C_VERSION_REG        0x700
#define PI7C_VERSION_DEFAULT    0x04030201
#define PI7C_TEMP_REG(i)        (0x5d8 + ((i) << 2))

static int qmp_pi7c9x3g606gp_get_temperature(const char *id, int n)
{
    QDict *response;
    int ret;
    g_autofree char *property = g_strdup_printf("temp[%d]", n);

    response = qmp("{ 'execute': 'qom-get', 'arguments': { 'path': %s, "
                   "'property': %s } }", id, property);
    g_assert(qdict_haskey(response, "return"));
    ret = qdict_get_int(response, "return");
    qobject_unref(response);
    return ret;
}

static void qmp_pi7c9x3g606gp_set_temperature(const char *id, int n, int value)
{
    QDict *response;
    g_autofree char *property = g_strdup_printf("temp[%d]", n);

    response = qmp("{ 'execute': 'qom-set', 'arguments': { 'path': %s, "
                   "'property': %s, 'value': %d } }", id, property, value);
    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);
}

static uint32_t read_data(QI2CDevice *i2cdev, uint16_t addr)
{
    /* Read command with all bytes enabled. */
    uint8_t cmd[4] = {0x4, 0x0, 0x3c, 0x0};
    uint32_t data;

    /* Deposit address bit 11:10. */
    cmd[2] |= extract16(addr, 10, 2);

    /* Deposit address bit 9:2. */
    cmd[3] |= extract16(addr, 2, 8);

    /* Send the command. */
    qi2c_send(i2cdev, cmd, 4);

    /* Receive the data. */
    qi2c_recv(i2cdev, (uint8_t *)&data, sizeof(uint32_t));

    return data;
}

static void test_version(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint32_t value;

    value = read_data(i2cdev, PI7C_VERSION_REG);
    g_assert_cmphex(value, ==, PI7C_VERSION_DEFAULT);
}

static void test_temp(QI2CDevice *i2cdev, int n)
{
    uint32_t value;

    value = read_data(i2cdev, PI7C_TEMP_REG(n));
    g_assert_cmphex(value, ==, 0);

    qmp_pi7c9x3g606gp_set_temperature(PI7C_TEST_ID, n, 20000);
    value = qmp_pi7c9x3g606gp_get_temperature(PI7C_TEST_ID, n);
    /* Allows 0.1% rounding error. */
    g_assert_cmpuint(value, >, 19990);
    g_assert_cmpuint(value, <, 20010);

    /**
     * From Diodes PI7C9X Datasheet Rev 0.8 Section 9
     * TEMP = (N/4094)*237.7-79.925
     * REG[8:19] = N
     * REG[24] = DATA_READY (1)
     */
    value = read_data(i2cdev, PI7C_TEMP_REG(n));
    g_assert_cmphex(value, ==, 0xb98600);
}

static void test_temp0(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    test_temp(i2cdev, 0);
}

static void test_temp1(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    test_temp(i2cdev, 1);
}

static void test_temp2(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    test_temp(i2cdev, 2);
}

static void pi7c9x3g606gp_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" PI7C_TEST_ID ",address=0x6f"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { 0x6f });

    qos_node_create_driver("pi7c9x3g606gp", i2c_device_create);
    qos_node_consumes("pi7c9x3g606gp", "i2c-bus", &opts);

    qos_add_test("test_version", "pi7c9x3g606gp", test_version, NULL);
    qos_add_test("test_temp0", "pi7c9x3g606gp", test_temp0, NULL);
    qos_add_test("test_temp1", "pi7c9x3g606gp", test_temp1, NULL);
    qos_add_test("test_temp2", "pi7c9x3g606gp", test_temp2, NULL);
}
libqos_init(pi7c9x3g606gp_register_nodes);

/*
 * QTest for AD5321 Voltage Output 12-bit DAC
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "libqos/i2c.h"
#include "qemu/bitops.h"

#define TEST_ID "ad5321-test"
#define TEST_ADDR (0x0c)

/* test write a byte doesn't update the shift register */
static void test_reg_unchange(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    /* Initialize the shift register with 0x00 */
    uint16_t reg = 0x00;
    qi2c_send(i2cdev, (uint8_t *)&reg, sizeof(uint16_t));

    /* Send a single byte */
    uint8_t byte = 0x11;
    uint16_t resp;
    qi2c_send(i2cdev, &byte, sizeof(uint8_t));

    /* The shift register doesn't change */
    qi2c_recv(i2cdev, (uint8_t *)&resp, sizeof(uint16_t));
    g_assert_cmphex(resp, ==, reg);
}

/* test r/w a word to the shift register */
static void test_rw_reg(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint16_t resp;

    /* Write a word and read it back */
    uint16_t reg = 0x0110;
    qi2c_send(i2cdev, (uint8_t *)&reg, sizeof(uint16_t));

    qi2c_recv(i2cdev, (uint8_t *)&resp, sizeof(uint16_t));
    g_assert_cmphex(resp, ==, reg);

    /* Write the second word and read it back */
    reg = 0x1001;
    qi2c_send(i2cdev, (uint8_t *)&reg, sizeof(uint16_t));

    qi2c_recv(i2cdev, (uint8_t *)&resp, sizeof(uint16_t));
    g_assert_cmphex(resp, ==, reg);
}

static void ad5321_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" TEST_ID ",address=0x0c"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { TEST_ADDR });

    qos_node_create_driver("ad5321", i2c_device_create);
    qos_node_consumes("ad5321", "i2c-bus", &opts);

    qos_add_test("test_reg_unchange", "ad5321", test_reg_unchange, NULL);
    qos_add_test("test_rw_reg", "ad5321", test_rw_reg, NULL);
}

libqos_init(ad5321_register_nodes);

/*
 * QTests for Nuvoton NPCM8mnx USB Device Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "fake-usbredir-guest.h"
#include "libqtest-single.h"
#include "socket_util.h"

#define NPCM8xxUDC6_BASE_ADDR 0xf0836000

/* Register offsets */
#define R_DCCPARAMS 0x124
#define R_USBSTS 0x144
#define R_PORTSC1 0x184
#define R_ENDPTCTRL0 0x1C0

typedef struct TestData {
    int fd;
} TestData;

static void test_register_access(void)
{
    const uint32_t test_write_value = 0xffffffff;
    uint32_t init_value = 0;
    uint32_t new_value = 0;

    /* Test DCCPARAMS register */
    init_value = readl(NPCM8xxUDC6_BASE_ADDR + R_DCCPARAMS);
    writel(NPCM8xxUDC6_BASE_ADDR + R_DCCPARAMS, ~init_value);
    g_assert_cmphex(readl(NPCM8xxUDC6_BASE_ADDR + R_DCCPARAMS), ==, init_value);

    /* Test USBSTS register */
    init_value = readl(NPCM8xxUDC6_BASE_ADDR + R_USBSTS);
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBSTS, test_write_value);

    new_value = readl(NPCM8xxUDC6_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0x100);

    /* Test PORTSC1 register */
    init_value = readl(NPCM8xxUDC6_BASE_ADDR + R_PORTSC1);
    writel(NPCM8xxUDC6_BASE_ADDR + R_PORTSC1, test_write_value);

    new_value = readl(NPCM8xxUDC6_BASE_ADDR + R_PORTSC1);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0xDBFFF27E);

    /* Test ENDPTCTRL0 register */
    init_value = readl(NPCM8xxUDC6_BASE_ADDR + R_ENDPTCTRL0);
    writel(NPCM8xxUDC6_BASE_ADDR + R_ENDPTCTRL0, test_write_value);

    new_value = readl(NPCM8xxUDC6_BASE_ADDR + R_ENDPTCTRL0);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0xffffffff);
}

static void test_attach_device(const void *data)
{
    FakeUsbredirGuest faker;
    TestData *test_data = (TestData *)data;
    fake_usbredir_guest_init(&faker, test_data->fd);
    fake_usbredir_guest_start(&faker);
    g_assert_true(fake_usbredir_guest_helloed(&faker));
    fake_usbredir_guest_stop(&faker);
}

int main(int argc, char **argv)
{
    int ret, sock, port;
    TestData test_data;

    g_test_init(&argc, &argv, NULL);
    g_test_set_nonfatal_assertions();

    /* Setup test socket. */
    struct timeval timeout = {.tv_usec = 1000};
    port = socket_util_open_socket(&sock, &timeout);

    global_qtest = qtest_initf(
        "-machine npcm845-evb,remote-udc=testcd "
        "-chardev socket,id=testcd,port=%d,host=localhost",
        port);

    test_data.fd = socket_util_setup_fd(sock);
    qtest_add_func("/npcm8xx_udc/register_access", test_register_access);
    qtest_add_data_func("/npcm8xx_udc/attach_device", &test_data,
                        test_attach_device);

    ret = g_test_run();
    qtest_end();

    return ret;
}

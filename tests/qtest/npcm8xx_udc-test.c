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
#define NPCM8xxUDC6_IRQ 57
#define EP_TD_BASE_ADDR 0x800000

/* Register offsets */
#define R_DCCPARAMS 0x124
#define M_DCCPARAMS_DEVICE_ENDPOINT_NUMBER 0x1f

#define R_USBCMD 0x140
#define F_USBCMD_RUN 0x1
#define F_USBCMD_RESET 0x2

#define R_USBSTS 0x144
#define M_USBSTS_PORT_CHANGE_DETECT 0x4

#define R_USBINTR 0x148
#define F_USBINTR_USB_INTERRUPT 0x1
#define F_USBINTR_USB_PORT_CHANGE_DETECT_INTERRUPT 0x4

#define R_ENDPOINTLISTADDR 0x158

#define R_PORTSC1 0x184
#define M_PORTSC1_CONNECT_STATUS 0x1

#define R_USBMODE 0x1A8
#define F_USBMODE_BIG_ENDIANNESS 0x4
#define F_USBMODE_SETUP_LOCKOUT_OFF 0x8

#define R_ENDPTCTRL0 0x1C0
#define F_ENDPTCTRL_TX_BULK_TYPE 0x800000
#define F_ENDPTCTRL_RX_BULK_TYPE 0x8

typedef struct TestData {
    int sock;
} TestData;

static inline void npcm8xx_udc_reset(void)
{
    uint32_t command;
    /* Disable interrupt */
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBINTR, 0);

    /* Stop the UDC */
    command = readl(NPCM8xxUDC6_BASE_ADDR + R_USBCMD);
    command &= ~F_USBCMD_RUN;
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBCMD, command);

    /* Reset the UDC */
    command |= F_USBCMD_RESET;
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBCMD, command);

    /* Make sure the UDC has reset */
    command = readl(NPCM8xxUDC6_BASE_ADDR + R_USBCMD);
    g_assert((command & F_USBCMD_RESET) == 0);
}

static inline void npcm8xx_udc_init(void)
{
    int ep_count;
    uint32_t mode, params, ep_ctrl;
    npcm8xx_udc_reset();

    /* Set up UDC mode */
    mode = readl(NPCM8xxUDC6_BASE_ADDR + R_USBMODE);
    mode |= F_USBMODE_BIG_ENDIANNESS | F_USBMODE_SETUP_LOCKOUT_OFF;
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBMODE, mode);

    /* Set endpoint transfer descriptor address */
    writel(NPCM8xxUDC6_BASE_ADDR + R_ENDPOINTLISTADDR, EP_TD_BASE_ADDR);

    /*
     * Initialize all endpoints except endpoint 0 because ep0 is initialized by
     * default.
     */
    params = readl(NPCM8xxUDC6_BASE_ADDR + R_DCCPARAMS);
    ep_count = params & M_DCCPARAMS_DEVICE_ENDPOINT_NUMBER;
    for (int i = 1; i < ep_count; i++) {
        uint64_t addr = NPCM8xxUDC6_BASE_ADDR + R_ENDPTCTRL0 + (4 * i);
        ep_ctrl = readl(addr);
        ep_ctrl |= F_ENDPTCTRL_TX_BULK_TYPE | F_ENDPTCTRL_RX_BULK_TYPE;
        writel(addr, ep_ctrl);
    }
}

static inline void npcm8xx_udc_run(void)
{
    uint32_t command, interrupt;

    /* Enable interrupts */
    interrupt =
        F_USBINTR_USB_INTERRUPT | F_USBINTR_USB_PORT_CHANGE_DETECT_INTERRUPT;
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBINTR, interrupt);

    /* Run the UDC */
    command = readl(NPCM8xxUDC6_BASE_ADDR + R_USBCMD);
    command |= F_USBCMD_RUN;
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBCMD, command);
}

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
    int fd = socket_util_setup_fd(test_data->sock);
    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);
    g_assert_true(fake_usbredir_guest_helloed(&faker));
    fake_usbredir_guest_stop(&faker);
    close(fd);
}

static void test_run_device(const void *data)
{
    FakeUsbredirGuest faker;
    uint32_t port_status, usb_status;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    npcm8xx_udc_init();
    npcm8xx_udc_run();

    fake_usbredir_guest_stop(&faker);

    /* Verify the UDC is in run state */
    g_assert_true(get_irq(NPCM8xxUDC6_IRQ));
    port_status = readl(NPCM8xxUDC6_BASE_ADDR + R_PORTSC1);
    g_assert_true(port_status & M_PORTSC1_CONNECT_STATUS);
    usb_status = readl(NPCM8xxUDC6_BASE_ADDR + R_USBSTS);
    g_assert_true(usb_status & M_USBSTS_PORT_CHANGE_DETECT);

    close(fd);
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
        "-chardev socket,id=testcd,port=%d,host=localhost,reconnect=1",
        port);
    qtest_irq_intercept_in(global_qtest, "/machine/soc/gic");

    test_data.sock = sock;
    qtest_add_func("/npcm8xx_udc/register_access", test_register_access);
    qtest_add_data_func("/npcm8xx_udc/attach_device", &test_data,
                        test_attach_device);
    qtest_add_data_func("/npcm8xx_udc/run_device", &test_data, test_run_device);

    ret = g_test_run();
    qtest_end();

    return ret;
}

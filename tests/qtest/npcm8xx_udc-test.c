/*
 * QTests for Nuvoton NPCM8mnx USB Device Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include <libusb-1.0/libusb.h>

#include "qemu/osdep.h"
#include "hw/usb/npcm8xx-udc.h"
#include "fake-usbredir-guest.h"
#include "libqtest-single.h"
#include "socket_util.h"

#define MILLI_SECOND 1000

/* Device values */
#define NPCM8xxUDC6_BASE_ADDR 0xf0836000
#define NPCM8xxUDC6_IRQ 57

/* Fake test values */
#define EP_TD_BASE_ADDR 0x800000
#define COMMON_EP_NEXT_TD_POINTER 0x810000
#define COMMON_EP_TD_BUFFER_POINTER 0x900000

/* Register offsets */
#define R_DCCPARAMS 0x124
#define M_DCCPARAMS_DEVICE_ENDPOINT_NUMBER 0x1f

#define R_USBCMD 0x140
#define F_USBCMD_RUN 0x1
#define F_USBCMD_RESET 0x2

#define R_USBSTS 0x144
#define F_USBSTS_USB_INTERRUPT 0x1
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

#define R_ENDPTSETUPSTAT 0x1AC

#define R_ENDPTPRIME 0x1B0
#define R_ENDPTPRIME_TX_BUFFER_SHIFT 16

#define R_ENDPTCOMPLETE 0x1BC
#define R_ENDPTCOMPLETE_TX_BUFFER_SHIFT 16

#define R_ENDPTSTAT 0x1B8
#define R_ENDPTSTAT_TX_BUFFER_SHIFT 16

#define R_ENDPTCTRL0 0x1C0
#define F_ENDPTCTRL_TX_BULK_TYPE 0x800000
#define F_ENDPTCTRL_RX_BULK_TYPE 0x8

/* Fake USB device descriptors */
const struct libusb_config_descriptor fake_usb_config_desc = {
    .bLength = LIBUSB_DT_CONFIG_SIZE,
    .bDescriptorType = LIBUSB_DT_CONFIG,
    .wTotalLength = LIBUSB_DT_CONFIG_SIZE + LIBUSB_DT_INTERFACE_SIZE +
                    LIBUSB_DT_ENDPOINT_SIZE * 2,
    .bNumInterfaces = 1,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = 0,
    .MaxPower = 1,
};

#define FAKE_USB_NUM_ENDPOINTS 2
const struct libusb_interface_descriptor fake_usb_if_desc = {
    .bLength = LIBUSB_DT_INTERFACE_SIZE,
    .bDescriptorType = LIBUSB_DT_INTERFACE,
    .bInterfaceNumber = 1,
    .bAlternateSetting = 1,
    .bNumEndpoints = FAKE_USB_NUM_ENDPOINTS,
    .bInterfaceClass = 1,
    .bInterfaceSubClass = 2,
    .bInterfaceProtocol = 3,
    .iInterface = 0,
};

const struct libusb_endpoint_descriptor
    fake_usb_ep_desc[FAKE_USB_NUM_ENDPOINTS] = {
        {
            .bLength = LIBUSB_DT_ENDPOINT_SIZE,
            .bDescriptorType = LIBUSB_DT_ENDPOINT,
            .bEndpointAddress = LIBUSB_ENDPOINT_OUT + 1,
            .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 0,
            .bRefresh = 0,
        },
        {
            .bLength = LIBUSB_DT_ENDPOINT_SIZE,
            .bDescriptorType = LIBUSB_DT_ENDPOINT,
            .bEndpointAddress = LIBUSB_ENDPOINT_IN + 1,
            .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 0,
            .bRefresh = 0,
        },
};

const struct libusb_device_descriptor fake_usb_device_desc = {
    .bLength = LIBUSB_DT_DEVICE_SIZE,
    .bDescriptorType = LIBUSB_DT_DEVICE,
    /* USB 2.0 */
    .bcdUSB = 0x0200,
    .bDeviceClass = LIBUSB_CLASS_COMM,
    .bDeviceSubClass = 6,
    .bDeviceProtocol = 26,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x123,
    .idProduct = 0x546,
    .bcdDevice = 0xC001,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

typedef struct TestData {
    int sock;
    uint8_t *serialized_config_desc;
    int serialized_config_desc_length;
} TestData;

/* Test helpers */
static inline void make_control_transfer_packet(uint32_t *target,
                                                uint8_t request_type,
                                                uint8_t request, uint16_t value,
                                                uint16_t index, uint16_t length)
{
    target[0] = request_type | (request << 8) | (value << 16);
    target[1] = index | (length << 16);
}

/* NPCM UDC Driver */
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

static inline void npcm8xx_udc_handle_port_connect(void)
{
    uint32_t port_status, usb_status;

    /* Assert IRQ */
    port_status = readl(NPCM8xxUDC6_BASE_ADDR + R_PORTSC1);
    g_assert_true(port_status & M_PORTSC1_CONNECT_STATUS);
    usb_status = readl(NPCM8xxUDC6_BASE_ADDR + R_USBSTS);
    g_assert_true(usb_status & M_USBSTS_PORT_CHANGE_DETECT);

    /* Clear USB interrupt status */
    usb_status &= M_USBSTS_PORT_CHANGE_DETECT;
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBSTS, usb_status);
}

static inline void npcm8xx_udc_init_tx_queue_head(void)
{
    QueueHead ep0_tx_qh;
    ep0_tx_qh.td.next_pointer = COMMON_EP_NEXT_TD_POINTER;
    memwrite(EP_TD_BASE_ADDR + sizeof(QueueHead), &ep0_tx_qh,
             sizeof(QueueHead));
}

static inline void npcm8xx_udc_assert_and_clear_irq(uint32_t expected_status)
{
    uint32_t actual_status;

    g_assert_true(get_irq(NPCM8xxUDC6_IRQ));
    actual_status = readl(NPCM8xxUDC6_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(actual_status, ==, expected_status);
    writel(NPCM8xxUDC6_BASE_ADDR + R_USBSTS, actual_status);
}

static inline void npcm8xx_udc_assert_receive_control_transfer(
    uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
    uint16_t length)
{
    uint32_t ep_setup_status;
    uint32_t expected_control_packets[2];
    QueueHead ep0_rx_qh;

    make_control_transfer_packet(expected_control_packets, request_type,
                                 request, value, index, length);

    /* Assert IRQ and interrupt status */
    npcm8xx_udc_assert_and_clear_irq(F_USBSTS_USB_INTERRUPT);
    ep_setup_status = readl(NPCM8xxUDC6_BASE_ADDR + R_ENDPTSETUPSTAT);
    g_assert_true(ep_setup_status & 1);

    /* Verify the control transfer is a standard GET_DESCRIPTOR */
    memread(EP_TD_BASE_ADDR, &ep0_rx_qh, sizeof(QueueHead));
    g_assert_cmphex(ep0_rx_qh.setup[0], ==, expected_control_packets[0]);
    g_assert_cmphex(ep0_rx_qh.setup[1], ==, expected_control_packets[1]);
}

static inline void npcm8xx_udc_send(uint32_t endpoint_mask, uint8_t *data,
                                    int length)
{
    TransferDescriptor ep0_tx_td;

    /* Setup transfer descriptor and fill the tx buffer */
    ep0_tx_td.next_pointer = 1;
    ep0_tx_td.info = length << TD_INFO_TOTAL_BYTES_SHIFT;
    ep0_tx_td.buffer_pointers[0] = COMMON_EP_TD_BUFFER_POINTER;
    memwrite(COMMON_EP_NEXT_TD_POINTER, &ep0_tx_td, sizeof(TransferDescriptor));
    memwrite(COMMON_EP_TD_BUFFER_POINTER, data, length);

    /* Prime the endpoint */
    writel(NPCM8xxUDC6_BASE_ADDR + R_ENDPTPRIME, endpoint_mask);
}

static inline void npcm8xx_udc_assert_sent(uint32_t endpoint_mask)
{
    uint32_t endpoint_complete, actual_status;

    g_assert_true(get_irq(NPCM8xxUDC6_IRQ));
    actual_status = readl(NPCM8xxUDC6_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(actual_status, ==, F_USBSTS_USB_INTERRUPT);
    endpoint_complete = readl(NPCM8xxUDC6_BASE_ADDR + R_ENDPTCOMPLETE);
    g_assert_cmphex(endpoint_complete, ==, endpoint_mask);

    /* Clear endpoint complete */
    writel(NPCM8xxUDC6_BASE_ADDR + R_ENDPTCOMPLETE, endpoint_complete);
    endpoint_complete = readl(NPCM8xxUDC6_BASE_ADDR + R_ENDPTCOMPLETE);
    g_assert_cmphex(endpoint_complete, ==, 0);
}

/* NPCM UDC Unit Tests */
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
    g_assert(fake_usbredir_guest_helloed(&faker));
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

static void test_connect_device_port(const void *data)
{
    FakeUsbredirGuest faker;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before checking port status. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm8xx_udc_init();
    npcm8xx_udc_run();
    npcm8xx_udc_handle_port_connect();

    fake_usbredir_guest_stop(&faker);

    npcm8xx_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);

    close(fd);
}

static void test_connect_device(const void *data)
{
    FakeUsbredirGuest faker;
    uint32_t endpoint_mask;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm8xx_udc_init();
    npcm8xx_udc_run();
    npcm8xx_udc_handle_port_connect();
    npcm8xx_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);

    /* Reply configuration descriptor */
    endpoint_mask = 1 << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    npcm8xx_udc_init_tx_queue_head();
    npcm8xx_udc_send(endpoint_mask, test_data->serialized_config_desc,
                     test_data->serialized_config_desc_length);
    npcm8xx_udc_assert_sent(endpoint_mask);

    /* Verify faker's interface */
    fake_usbredir_guest_assert_num_interfaces(&faker, 1);
    fake_usbredir_guest_assert_contains_interface(&faker, &fake_usb_if_desc);

    /* Verify faker's endpoint */
    fake_usbredir_guest_assert_num_endpoints(&faker, FAKE_USB_NUM_ENDPOINTS);
    fake_usbredir_guest_assert_contains_endpoint(
        &faker, &fake_usb_ep_desc[0], fake_usb_if_desc.bInterfaceNumber);
    fake_usbredir_guest_assert_contains_endpoint(
        &faker, &fake_usb_ep_desc[1], fake_usb_if_desc.bInterfaceNumber);

    /* NPCM UDC should send device descriptor now */
    npcm8xx_udc_assert_receive_control_transfer(
        LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
        LIBUSB_DT_DEVICE << 8, 0, LIBUSB_DT_DEVICE_SIZE);
    npcm8xx_udc_send(endpoint_mask, (void *)&fake_usb_device_desc,
                     fake_usb_device_desc.bLength);
    npcm8xx_udc_assert_sent(endpoint_mask);

    fake_usbredir_guest_stop(&faker);

    /* Verify faker's device */
    fake_usbredir_guest_assert_device(&faker, &fake_usb_device_desc);

    close(fd);
}

static inline void setup_test_data(TestData *test_data, int sock)
{
    size_t length = 0;

    test_data->sock = sock;
    test_data->serialized_config_desc =
        g_new(uint8_t, fake_usb_config_desc.wTotalLength);
    memcpy(test_data->serialized_config_desc, &fake_usb_config_desc,
           LIBUSB_DT_CONFIG_SIZE);
    length += LIBUSB_DT_CONFIG_SIZE;
    memcpy(test_data->serialized_config_desc + length, &fake_usb_if_desc,
           LIBUSB_DT_INTERFACE_SIZE);
    length += LIBUSB_DT_INTERFACE_SIZE;

    for (int i = 0; i < FAKE_USB_NUM_ENDPOINTS; i++) {
        memcpy(test_data->serialized_config_desc + length, &fake_usb_ep_desc[i],
               LIBUSB_DT_ENDPOINT_SIZE);
        length += LIBUSB_DT_ENDPOINT_SIZE;
    }

    g_assert_cmphex(length, ==, fake_usb_config_desc.wTotalLength);
    test_data->serialized_config_desc_length = length;
}

static inline void cleanup_test_data(TestData *test_data)
{
    g_free(test_data->serialized_config_desc);
}

int main(int argc, char **argv)
{
    int ret, sock, port;
    TestData test_data;

    g_test_init(&argc, &argv, NULL);
    g_test_set_nonfatal_assertions();

    /* Setup test socket. */
    struct timeval timeout = {.tv_usec = 200 * MILLI_SECOND};
    port = socket_util_open_socket(&sock, &timeout, &timeout);

    global_qtest = qtest_initf(
        "-machine npcm845-evb,remote-udc=testcd "
        "-chardev socket,id=testcd,port=%d,host=localhost,reconnect=1",
        port);
    qtest_irq_intercept_in(global_qtest, "/machine/soc/gic");

    setup_test_data(&test_data, sock);
    qtest_add_func("/npcm8xx_udc/register_access", test_register_access);
    qtest_add_data_func("/npcm8xx_udc/attach_device", &test_data,
                        test_attach_device);
    qtest_add_data_func("/npcm8xx_udc/run_device", &test_data, test_run_device);
    qtest_add_data_func("/npcm8xx_udc/connect_device_port", &test_data,
                        test_connect_device_port);
    qtest_add_data_func("/npcm8xx_udc/connect_device", &test_data,
                        test_connect_device);

    ret = g_test_run();
    qtest_end();
    cleanup_test_data(&test_data);

    return ret;
}

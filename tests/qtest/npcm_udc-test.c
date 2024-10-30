/*
 * QTests for Nuvoton NPCM USB Device Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "hw/usb/npcm-udc.h"

#include <libusb-1.0/libusb.h>

#include "fake-usbredir-guest.h"
#include "libqtest-single.h"
#include "socket_util.h"

#define MILLI_SECOND 1000

/* Device values */
#define NPCM_UDC6_BASE_ADDR 0xf0836000
#define NPCM_UDC6_IRQ 57

/* Fake test values */
#define EP_TD_BASE_ADDR 0x800000
#define BASE_EP_IN_NEXT_TD_POINTER 0x810000
#define BASE_EP_OUT_NEXT_TD_POINTER 0x820000
#define BASE_EP_IN_TD_BUFFER_POINTER 0x900000
#define BASE_EP_OUT_TD_BUFFER_POINTER 0x910000
#define EP_TD_BUFFER_PADDING 1024

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
static inline void npcm_udc_reset(void)
{
    uint32_t command;
    /* Disable interrupt */
    writel(NPCM_UDC6_BASE_ADDR + R_USBINTR, 0);

    /* Stop the UDC */
    command = readl(NPCM_UDC6_BASE_ADDR + R_USBCMD);
    command &= ~F_USBCMD_RUN;
    writel(NPCM_UDC6_BASE_ADDR + R_USBCMD, command);

    /* Reset the UDC */
    command |= F_USBCMD_RESET;
    writel(NPCM_UDC6_BASE_ADDR + R_USBCMD, command);

    /* Make sure the UDC has reset */
    command = readl(NPCM_UDC6_BASE_ADDR + R_USBCMD);
    g_assert((command & F_USBCMD_RESET) == 0);
}

static inline void npcm_udc_init(void)
{
    int ep_count;
    uint32_t mode, params, ep_ctrl;
    npcm_udc_reset();

    /* Set up UDC mode */
    mode = readl(NPCM_UDC6_BASE_ADDR + R_USBMODE);
    mode |= F_USBMODE_BIG_ENDIANNESS | F_USBMODE_SETUP_LOCKOUT_OFF;
    writel(NPCM_UDC6_BASE_ADDR + R_USBMODE, mode);

    /* Set endpoint transfer descriptor address */
    writel(NPCM_UDC6_BASE_ADDR + R_ENDPOINTLISTADDR, EP_TD_BASE_ADDR);

    /*
     * Initialize all endpoints except endpoint 0 because ep0 is initialized by
     * default.
     */
    params = readl(NPCM_UDC6_BASE_ADDR + R_DCCPARAMS);
    ep_count = params & M_DCCPARAMS_DEVICE_ENDPOINT_NUMBER;
    for (int i = 1; i < ep_count; i++) {
        uint64_t addr = NPCM_UDC6_BASE_ADDR + R_ENDPTCTRL0 + (4 * i);
        ep_ctrl = readl(addr);
        ep_ctrl |= F_ENDPTCTRL_TX_BULK_TYPE | F_ENDPTCTRL_RX_BULK_TYPE;
        writel(addr, ep_ctrl);
    }
}

static inline void npcm_udc_run(void)
{
    uint32_t command, interrupt;

    /* Enable interrupts */
    interrupt =
        F_USBINTR_USB_INTERRUPT | F_USBINTR_USB_PORT_CHANGE_DETECT_INTERRUPT;
    writel(NPCM_UDC6_BASE_ADDR + R_USBINTR, interrupt);

    /* Run the UDC */
    command = readl(NPCM_UDC6_BASE_ADDR + R_USBCMD);
    command |= F_USBCMD_RUN;
    writel(NPCM_UDC6_BASE_ADDR + R_USBCMD, command);
}

static inline void npcm_udc_handle_port_connect(void)
{
    uint32_t port_status, usb_status;

    /* Assert IRQ */
    port_status = readl(NPCM_UDC6_BASE_ADDR + R_PORTSC1);
    g_assert_true(port_status & M_PORTSC1_CONNECT_STATUS);
    usb_status = readl(NPCM_UDC6_BASE_ADDR + R_USBSTS);
    g_assert_true(usb_status & M_USBSTS_PORT_CHANGE_DETECT);

    /* Clear USB interrupt status */
    usb_status &= M_USBSTS_PORT_CHANGE_DETECT;
    writel(NPCM_UDC6_BASE_ADDR + R_USBSTS, usb_status);
}

static inline void npcm_udc_init_tx_queue_head(uint32_t endpoint_mask)
{
    QueueHead tx_qh = {};
    TransferDescriptor tx_td = {};
    uint16_t tx_endpoint_bitmap = endpoint_mask >> R_ENDPTPRIME_TX_BUFFER_SHIFT;

    for (uint8_t ep_num = 0; tx_endpoint_bitmap != 0;
         tx_endpoint_bitmap >>= 1, ++ep_num) {
        if (!(tx_endpoint_bitmap & 1)) {
            continue;
        }

        tx_qh.td.next_pointer =
            BASE_EP_IN_NEXT_TD_POINTER + (ep_num * sizeof(TransferDescriptor));
        tx_td.info = TD_INFO_INTERRUPT_ON_COMPLETE_MASK;
        memwrite(EP_TD_BASE_ADDR + (((ep_num << 1) + 1) * sizeof(QueueHead)),
                 &tx_qh, sizeof(QueueHead));
        memwrite(tx_qh.td.next_pointer, &tx_td, sizeof(TransferDescriptor));
    }
}

static inline void npcm_udc_init_rx_queue_head(uint32_t endpoint_mask)
{
    QueueHead rx_qh = {};
    TransferDescriptor rx_td = {};

    for (uint8_t ep_num = 0; endpoint_mask != 0;
         endpoint_mask >>= 1, ++ep_num) {
        if (!(endpoint_mask & 1)) {
            continue;
        }

        rx_qh.td.next_pointer =
            BASE_EP_OUT_NEXT_TD_POINTER + (ep_num * sizeof(TransferDescriptor));
        rx_td.info = TD_INFO_INTERRUPT_ON_COMPLETE_MASK;
        memwrite(EP_TD_BASE_ADDR + ((ep_num << 1) * sizeof(QueueHead)), &rx_qh,
                 sizeof(QueueHead));
        memwrite(rx_qh.td.next_pointer, &rx_td, sizeof(TransferDescriptor));
    }
}

static inline void npcm_udc_assert_and_clear_irq(uint32_t expected_status)
{
    uint32_t actual_status;

    g_assert_true(get_irq(NPCM_UDC6_IRQ));
    actual_status = readl(NPCM_UDC6_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(actual_status, ==, expected_status);
    writel(NPCM_UDC6_BASE_ADDR + R_USBSTS, actual_status);
}

static inline void npcm_udc_assert_receive_control_transfer(
    uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
    uint16_t length)
{
    uint32_t ep_setup_status;
    uint32_t expected_control_packets[2];
    QueueHead ep0_rx_qh;

    make_control_transfer_packet(expected_control_packets, request_type,
                                 request, value, index, length);

    /* Assert IRQ and interrupt status */
    npcm_udc_assert_and_clear_irq(F_USBSTS_USB_INTERRUPT);
    ep_setup_status = readl(NPCM_UDC6_BASE_ADDR + R_ENDPTSETUPSTAT);
    g_assert_true(ep_setup_status & 1);

    /* Verify the control transfer is a standard GET_DESCRIPTOR */
    memread(EP_TD_BASE_ADDR, &ep0_rx_qh, sizeof(QueueHead));
    g_assert_cmphex(ep0_rx_qh.setup[0], ==, expected_control_packets[0]);
    g_assert_cmphex(ep0_rx_qh.setup[1], ==, expected_control_packets[1]);
}

static inline void npcm_udc_send(uint8_t endpoint_address, uint8_t *data,
                                 int length)
{
    TransferDescriptor tx_td;
    uint8_t ep_num = endpoint_address & LIBUSB_ENDPOINT_ADDRESS_MASK;
    uint32_t endpoint_mask = (1 << ep_num) << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    uint64_t td_pointer =
        BASE_EP_IN_NEXT_TD_POINTER + (ep_num * sizeof(TransferDescriptor));

    /* Setup transfer descriptor and fill the tx buffer */
    tx_td.next_pointer = 1;
    tx_td.info = length << TD_INFO_TOTAL_BYTES_SHIFT;
    tx_td.buffer_pointers[0] = BASE_EP_IN_TD_BUFFER_POINTER;
    memwrite(td_pointer, &tx_td, sizeof(TransferDescriptor));
    memwrite(BASE_EP_IN_TD_BUFFER_POINTER, data, length);

    /* Prime the endpoint */
    writel(NPCM_UDC6_BASE_ADDR + R_ENDPTPRIME, endpoint_mask);
}

static inline void npcm_udc_assert_sent(uint8_t endpoint_address)
{
    uint32_t endpoint_complete, actual_status;
    uint8_t ep_num = endpoint_address & LIBUSB_ENDPOINT_ADDRESS_MASK;
    uint32_t endpoint_mask = (1 << ep_num) << R_ENDPTPRIME_TX_BUFFER_SHIFT;

    g_assert_true(get_irq(NPCM_UDC6_IRQ));
    actual_status = readl(NPCM_UDC6_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(actual_status, ==, F_USBSTS_USB_INTERRUPT);
    endpoint_complete = readl(NPCM_UDC6_BASE_ADDR + R_ENDPTCOMPLETE);
    g_assert_cmphex(endpoint_complete, ==, endpoint_mask);

    /* Clear endpoint complete */
    writel(NPCM_UDC6_BASE_ADDR + R_ENDPTCOMPLETE, endpoint_complete);
    endpoint_complete = readl(NPCM_UDC6_BASE_ADDR + R_ENDPTCOMPLETE);
    g_assert_cmphex(endpoint_complete, ==, 0);
}

static inline void npcm_udc_prepare_receive(uint8_t endpoint_address,
                                            uint16_t buffer_length)
{
    TransferDescriptor rx_td = {};
    uint64_t td_pointer;

    g_assert((endpoint_address & LIBUSB_ENDPOINT_IN) == 0);

    td_pointer = BASE_EP_OUT_NEXT_TD_POINTER +
                 (endpoint_address * sizeof(TransferDescriptor));
    rx_td.info = buffer_length << TD_INFO_TOTAL_BYTES_SHIFT |
                 TD_INFO_INTERRUPT_ON_COMPLETE_MASK;
    rx_td.buffer_pointers[0] = BASE_EP_OUT_TD_BUFFER_POINTER +
                               (endpoint_address * EP_TD_BUFFER_PADDING);
    memwrite(td_pointer, &rx_td, sizeof(TransferDescriptor));
}

static inline void npcm_udc_assert_received(uint8_t endpoint_address,
                                            uint32_t buffer_length,
                                            uint8_t *expected_data,
                                            uint32_t expected_length)
{
    TransferDescriptor rx_td = {};
    uint64_t td_pointer = BASE_EP_OUT_NEXT_TD_POINTER +
                          (endpoint_address * sizeof(TransferDescriptor));
    uint64_t buffer_pointer = BASE_EP_OUT_TD_BUFFER_POINTER +
                              (endpoint_address * EP_TD_BUFFER_PADDING);
    uint8_t *actual_data_buffer = g_malloc(expected_length);
    uint16_t actual_remaining_buffer_length;

    g_assert((endpoint_address & LIBUSB_ENDPOINT_IN) == 0);

    memread(td_pointer, &rx_td, sizeof(TransferDescriptor));
    actual_remaining_buffer_length = rx_td.info >> TD_INFO_TOTAL_BYTES_SHIFT;
    g_assert_cmphex(actual_remaining_buffer_length, ==,
                    buffer_length - expected_length);

    memread(buffer_pointer, actual_data_buffer, expected_length);
    g_assert_cmpmem(actual_data_buffer, expected_length, expected_data,
                    expected_length);

    g_free(actual_data_buffer);
}

static inline void npcm_udc_connect_device(uint8_t *config_desc,
                                           int config_desc_length,
                                           uint8_t *dev_desc,
                                           int dev_desc_length)
{
    uint32_t endpoint_mask;

    npcm_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);

    endpoint_mask = 1 << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    npcm_udc_init_tx_queue_head(endpoint_mask);
    npcm_udc_send(0, config_desc, config_desc_length);
    npcm_udc_assert_sent(0);

    /* NPCM UDC should send device descriptor now */
    npcm_udc_assert_receive_control_transfer(
        LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
        LIBUSB_DT_DEVICE << 8, 0, LIBUSB_DT_DEVICE_SIZE);
    npcm_udc_send(0, (void *)&fake_usb_device_desc,
                     fake_usb_device_desc.bLength);
    npcm_udc_assert_sent(0);
    npcm_udc_assert_and_clear_irq(F_USBSTS_USB_INTERRUPT);
}

/* NPCM UDC Unit Tests */
static void test_register_access(void)
{
    const uint32_t test_write_value = 0xffffffff;
    uint32_t init_value = 0;
    uint32_t new_value = 0;

    /* Test DCCPARAMS register */
    init_value = readl(NPCM_UDC6_BASE_ADDR + R_DCCPARAMS);
    writel(NPCM_UDC6_BASE_ADDR + R_DCCPARAMS, ~init_value);
    g_assert_cmphex(readl(NPCM_UDC6_BASE_ADDR + R_DCCPARAMS), ==, init_value);

    /* Test USBSTS register */
    init_value = readl(NPCM_UDC6_BASE_ADDR + R_USBSTS);
    writel(NPCM_UDC6_BASE_ADDR + R_USBSTS, test_write_value);

    new_value = readl(NPCM_UDC6_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0x100);

    /* Test PORTSC1 register */
    init_value = readl(NPCM_UDC6_BASE_ADDR + R_PORTSC1);
    writel(NPCM_UDC6_BASE_ADDR + R_PORTSC1, test_write_value);

    new_value = readl(NPCM_UDC6_BASE_ADDR + R_PORTSC1);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0xDBFFF27E);

    /* Test ENDPTCTRL0 register */
    init_value = readl(NPCM_UDC6_BASE_ADDR + R_ENDPTCTRL0);
    writel(NPCM_UDC6_BASE_ADDR + R_ENDPTCTRL0, test_write_value);

    new_value = readl(NPCM_UDC6_BASE_ADDR + R_ENDPTCTRL0);
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

    npcm_udc_init();
    npcm_udc_run();

    fake_usbredir_guest_stop(&faker);

    /* Verify the UDC is in run state */
    g_assert_true(get_irq(NPCM_UDC6_IRQ));
    port_status = readl(NPCM_UDC6_BASE_ADDR + R_PORTSC1);
    g_assert_true(port_status & M_PORTSC1_CONNECT_STATUS);
    usb_status = readl(NPCM_UDC6_BASE_ADDR + R_USBSTS);
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

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();

    fake_usbredir_guest_stop(&faker);

    npcm_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
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

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);

    /* Reply configuration descriptor */
    endpoint_mask = 1 << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    npcm_udc_init_tx_queue_head(endpoint_mask);
    npcm_udc_send(0, test_data->serialized_config_desc,
                     test_data->serialized_config_desc_length);
    npcm_udc_assert_sent(0);

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
    npcm_udc_assert_receive_control_transfer(
        LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
        LIBUSB_DT_DEVICE << 8, 0, LIBUSB_DT_DEVICE_SIZE);
    npcm_udc_send(0, (void *)&fake_usb_device_desc,
                     fake_usb_device_desc.bLength);
    npcm_udc_assert_sent(0);

    fake_usbredir_guest_stop(&faker);

    /* Verify faker's device */
    fake_usbredir_guest_assert_device(&faker, &fake_usb_device_desc);

    close(fd);
}

static void test_control_transfer(const void *data)
{
    FakeUsbredirGuest faker;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    fake_usbredir_guest_control_transfer(
        &faker,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD |
            LIBUSB_RECIPIENT_DEVICE,
        LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_CONFIG << 8, 0, NULL, 512);

    /* Wait for the control transfer to come through. */
    while (!get_irq(NPCM_UDC6_IRQ)) {
        /* Do nothing. */
    }

    npcm_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);
    npcm_udc_send(0, (void *)&fake_usb_config_desc,
                     fake_usb_config_desc.bLength);
    npcm_udc_assert_sent(0);
    fake_usbredir_guest_assert_control_transfer_received(
        &faker, (void *)&fake_usb_config_desc, fake_usb_config_desc.bLength);

    fake_usbredir_guest_stop(&faker);

    close(fd);
}

static void test_usbredir_host_set_configuration(const void *data)
{
    FakeUsbredirGuest faker;
    uint32_t endpoint_mask = 1 << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    fake_usbredir_guest_set_configuration(
        &faker, fake_usb_config_desc.bConfigurationValue);

    /* Wait for the control transfer to come through. */
    while (!get_irq(NPCM_UDC6_IRQ)) {
        /* Do nothing. */
    }

    /* First receive the set configuration control transfer */
    npcm_udc_assert_receive_control_transfer(
        LIBUSB_ENDPOINT_OUT, LIBUSB_REQUEST_SET_CONFIGURATION,
        fake_usb_config_desc.bConfigurationValue, 0, 0);
    /* Response with empty data to ACK the request */
    npcm_udc_send(endpoint_mask, NULL, 0);

    npcm_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);
    npcm_udc_send(0, test_data->serialized_config_desc,
                     test_data->serialized_config_desc_length);
    npcm_udc_assert_sent(0);

    fake_usbredir_guest_stop(&faker);
    close(fd);
}

static void test_usbredir_host_set_alt_setting(const void *data)
{
    FakeUsbredirGuest faker;
    uint32_t endpoint_mask = 1 << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    fake_usbredir_guest_set_alt_interface(&faker,
                                          fake_usb_if_desc.bInterfaceNumber,
                                          fake_usb_if_desc.bAlternateSetting);

    /* Wait for the control transfer to come through. */
    while (!get_irq(NPCM_UDC6_IRQ)) {
        /* Do nothing. */
    }

    /* First receive the set alt setting control transfer */
    npcm_udc_assert_receive_control_transfer(
        LIBUSB_ENDPOINT_OUT | LIBUSB_RECIPIENT_INTERFACE,
        LIBUSB_REQUEST_SET_INTERFACE, fake_usb_if_desc.bAlternateSetting,
        fake_usb_if_desc.bInterfaceNumber, 0);
    /* Response with empty data to ACK the request */
    npcm_udc_send(endpoint_mask, NULL, 0);

    npcm_udc_assert_receive_control_transfer(LIBUSB_ENDPOINT_IN,
                                                LIBUSB_REQUEST_GET_DESCRIPTOR,
                                                LIBUSB_DT_CONFIG << 8, 0, 512);
    npcm_udc_send(0, test_data->serialized_config_desc,
                     test_data->serialized_config_desc_length);
    npcm_udc_assert_sent(0);

    fake_usbredir_guest_stop(&faker);
    close(fd);
}

static void test_usbredir_host_bulk_transfer_write(const void *data)
{
    FakeUsbredirGuest faker = {};
    uint32_t endpoint_address = 1;
    uint32_t endpoint_mask = 0b10;
    uint8_t test_bulk_data[] = {0x1, 0x2, 0x3, 0x4};
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);
    uint32_t ep_complete;

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    npcm_udc_init_rx_queue_head(endpoint_mask);
    npcm_udc_prepare_receive(endpoint_address, sizeof(test_bulk_data));

    /* Write test bulk data. */
    fake_usbredir_guest_bulk_transfer(&faker, endpoint_address, test_bulk_data,
                                      sizeof(test_bulk_data));

    /* Wait for the bulk transfer to come through. */
    while (!get_irq(NPCM_UDC6_IRQ)) {
        /* Do nothing. */
    }

    /* Assert transfer status */
    npcm_udc_assert_and_clear_irq(F_USBSTS_USB_INTERRUPT);
    ep_complete = readl(NPCM_UDC6_BASE_ADDR + R_ENDPTCOMPLETE);
    g_assert_cmphex(ep_complete, ==, endpoint_mask);

    /* Assert the correctness of the transferred data */
    npcm_udc_assert_received(endpoint_address, sizeof(test_bulk_data),
                                test_bulk_data, sizeof(test_bulk_data));

    /* Ack the bulk transfer write */
    writel(NPCM_UDC6_BASE_ADDR + R_ENDPTPRIME, endpoint_mask);

    fake_usbredir_guest_assert_bulk_transfer(&faker, NULL, 0);

    fake_usbredir_guest_stop(&faker);
    close(fd);
}

static void test_usbredir_host_bulk_transfer_read(const void *data)
{
    FakeUsbredirGuest faker = {};
    uint32_t endpoint_address = LIBUSB_ENDPOINT_IN + 1;
    uint32_t endpoint_mask = 0b10 << R_ENDPTPRIME_TX_BUFFER_SHIFT;
    uint8_t test_bulk_data[] = {0x1, 0x2, 0x3, 0x4};
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    npcm_udc_init_tx_queue_head(endpoint_mask);

    /* Request bulk transfer out. */
    fake_usbredir_guest_bulk_transfer(&faker, endpoint_address, NULL,
                                      sizeof(test_bulk_data));

    npcm_udc_send(endpoint_address, test_bulk_data, sizeof(test_bulk_data));
    npcm_udc_assert_sent(endpoint_address);

    fake_usbredir_guest_assert_bulk_transfer(&faker, test_bulk_data,
                                             sizeof(test_bulk_data));

    fake_usbredir_guest_stop(&faker);
    close(fd);
}

static void test_usbredir_host_cancel_data_packet(const void *data)
{
    FakeUsbredirGuest faker = {};
    uint32_t endpoint_address = LIBUSB_ENDPOINT_IN + 1;
    uint8_t test_bulk_data[] = {0x1, 0x2, 0x3, 0x4};
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    /* Send dummy control transfer. */
    fake_usbredir_guest_control_transfer(&faker, 0, 0, 0, 0, NULL, 0);

    /* Cancel it. */
    fake_usbredir_guest_cancel_transfer(&faker);

    /* Assert that fake received canceled packet. */
    fake_usbredir_guest_assert_control_transfer_received(&faker, NULL, 0);

    /* Send bulk transfer OUT. */
    fake_usbredir_guest_bulk_transfer(&faker, 1, test_bulk_data,
                                      sizeof(test_bulk_data));

    /* Cancel it. */
    fake_usbredir_guest_cancel_transfer(&faker);

    /* Assert that fake received canceled packet. */
    fake_usbredir_guest_assert_bulk_transfer(&faker, NULL, 0);

    /* Send bulk transfer IN. */
    fake_usbredir_guest_bulk_transfer(&faker, endpoint_address, NULL,
                                      sizeof(test_bulk_data));

    /* Cancel it. */
    fake_usbredir_guest_cancel_transfer(&faker);

    /* Assert that fake received canceled packet. */
    fake_usbredir_guest_assert_bulk_transfer(&faker, NULL, 0);

    fake_usbredir_guest_stop(&faker);
    close(fd);
}

static void test_usbredir_host_cancel_burst_data_packets(const void *data)
{
    FakeUsbredirGuest faker = {};
    uint32_t endpoint_address = LIBUSB_ENDPOINT_IN + 1;
    uint8_t test_bulk_data[] = {0x1, 0x2, 0x3, 0x4};
    const int transfer_count = 10;
    TestData *test_data = (TestData *)data;
    int fd = socket_util_setup_fd(test_data->sock);

    fake_usbredir_guest_init(&faker, fd);
    fake_usbredir_guest_start(&faker);

    /* Make sure fake usbredir guest is ready before writing to it. */
    g_assert(fake_usbredir_guest_helloed(&faker));

    npcm_udc_init();
    npcm_udc_run();
    npcm_udc_handle_port_connect();
    npcm_udc_connect_device(test_data->serialized_config_desc,
                               test_data->serialized_config_desc_length,
                               (void *)&fake_usb_device_desc,
                               fake_usb_device_desc.bLength);

    /* Do series of read transfer. */
    for (int i = 0; i < transfer_count; ++i) {
        fake_usbredir_guest_bulk_transfer(&faker, endpoint_address, NULL,
                                        sizeof(test_bulk_data));
    }

    fake_usbredir_guest_cancel_transfer(&faker);

    /* Expect all read transfers to be canceled. */
    for (int i = 0; i < transfer_count; ++i) {
        fake_usbredir_guest_assert_bulk_transfer(&faker, NULL, 0);
    }

    fake_usbredir_guest_stop(&faker);
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
    struct timeval timeout = {.tv_usec = 300 * MILLI_SECOND};
    port = socket_util_open_socket(&sock, &timeout, &timeout);

    global_qtest = qtest_initf(
        "-machine npcm845-evb,remote-udc=testcd "
        "-chardev socket,id=testcd,port=%d,host=localhost,reconnect=1",
        port);
    qtest_irq_intercept_in(global_qtest, "/machine/soc/gic");

    setup_test_data(&test_data, sock);
    qtest_add_func("/npcm_udc/register_access", test_register_access);
    qtest_add_data_func("/npcm_udc/attach_device", &test_data,
                        test_attach_device);
    qtest_add_data_func("/npcm_udc/run_device", &test_data, test_run_device);
    qtest_add_data_func("/npcm_udc/connect_device_port", &test_data,
                        test_connect_device_port);
    qtest_add_data_func("/npcm_udc/connect_device", &test_data,
                        test_connect_device);
    qtest_add_data_func("/npcm_udc/control_transfer", &test_data,
                        test_control_transfer);
    qtest_add_data_func("/npcm_udc/usbredir_host_set_configuration",
                        &test_data, test_usbredir_host_set_configuration);
    qtest_add_data_func("/npcm_udc/usbredir_host_set_alt_setting",
                        &test_data, test_usbredir_host_set_alt_setting);
    qtest_add_data_func("/npcm_udc/usbredir_host_bulk_transfer_write",
                        &test_data, test_usbredir_host_bulk_transfer_write);
    qtest_add_data_func("/npcm_udc/usbredir_host_bulk_transfer_read",
                        &test_data, test_usbredir_host_bulk_transfer_read);
    qtest_add_data_func("/npcm_udc/usbredir_host_cancel_data_packet",
                        &test_data, test_usbredir_host_cancel_data_packet);
    qtest_add_data_func("/npcm_udc/usbredir_host_cancel_burst_data_packets",
                        &test_data,
                        test_usbredir_host_cancel_burst_data_packets);

    ret = g_test_run();
    qtest_end();
    cleanup_test_data(&test_data);

    return ret;
}

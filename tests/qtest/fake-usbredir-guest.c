/*
 * Fake USB Redirect Guest
 *
 * Copyright (C) 2024 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include <libusb-1.0/libusb.h>
#include <usbredirparser.h>
#include <usbredirproto.h>

#include "qemu/osdep.h"
#include "fake-usbredir-guest.h"

#define EP_INDEX(endpoint_address) \
    ((endpoint_address & 0x80) >> 3 | (endpoint_address & 0x0f))

#define VERSION "qemu fake usb-redir guest " QEMU_VERSION
#define USB_REDIR_MAX_NUM_ENDPOINTS 32

static inline void wait_for_flag(FakeUsbredirGuest *faker, sem_t *sem,
                                 bool *flag)
{
    g_mutex_lock(&faker->flag_mu);
    if (*flag) {
        g_mutex_unlock(&faker->flag_mu);
        return;
    }
    g_mutex_unlock(&faker->flag_mu);
    sem_wait(sem);
}

static void parser_log(void *priv, int level, const char *msg)
{
    /* Do nothing. We don't care about logs in test. */
    return;
}

static int parser_read(void *priv, uint8_t *data, int count)
{
    FakeUsbredirGuest *faker = priv;
    return read(faker->fd, data, count);
}

static int parser_write(void *priv, uint8_t *data, int count)
{
    FakeUsbredirGuest *faker = priv;
    return write(faker->fd, data, count);
}

static void parser_hello(void *priv, struct usb_redir_hello_header *h)
{
    FakeUsbredirGuest *faker = priv;

    sem_post(&faker->helloed_sem);
}

static void parser_interface_info(
    void *priv, struct usb_redir_interface_info_header *interface_info)
{
    FakeUsbredirGuest *faker = priv;

    g_mutex_lock(&faker->flag_mu);
    faker->received_if_info = true;
    memcpy(&faker->if_info, interface_info,
           sizeof(struct usb_redir_interface_info_header));
    sem_post(&faker->if_info_sem);
    g_mutex_unlock(&faker->flag_mu);
}

static void parser_ep_info(void *priv, struct usb_redir_ep_info_header *ep_info)
{
    FakeUsbredirGuest *faker = priv;

    g_mutex_lock(&faker->flag_mu);
    faker->received_ep_info = true;
    memcpy(&faker->ep_info, ep_info, sizeof(struct usb_redir_ep_info_header));
    sem_post(&faker->ep_info_sem);
    g_mutex_unlock(&faker->flag_mu);
}

static void parser_device_connect(
    void *priv, struct usb_redir_device_connect_header *device_connect)
{
    FakeUsbredirGuest *faker = priv;

    memcpy(&faker->device_info, device_connect,
           sizeof(struct usb_redir_device_connect_header));
    sem_post(&faker->device_info_sem);

    usbredirparser_send_reset(faker->parser);
    usbredirparser_do_write(faker->parser);
}

void fake_usbredir_guest_init(FakeUsbredirGuest *faker, int fd)
{
    faker->parser = usbredirparser_create();
    g_assert(faker->parser);

    g_mutex_init(&faker->flag_mu);
    sem_init(&faker->helloed_sem, 0, 0);
    sem_init(&faker->if_info_sem, 0, 0);
    sem_init(&faker->ep_info_sem, 0, 0);
    sem_init(&faker->device_info_sem, 0, 0);

    faker->fd = fd;

    faker->parser->priv = faker;
    faker->parser->log_func = parser_log;
    faker->parser->read_func = parser_read;
    faker->parser->write_func = parser_write;
    faker->parser->hello_func = parser_hello;
    faker->parser->interface_info_func = parser_interface_info;
    faker->parser->ep_info_func = parser_ep_info;
    faker->parser->device_connect_func = parser_device_connect;
}

static void *fake_usbredir_guest_server_thread(void *data)
{
    FakeUsbredirGuest *faker = data;
    while (true) {
        if (usbredirparser_do_read(faker->parser)) {
            return NULL;
        }

        if (usbredirparser_has_data_to_write(faker->parser)) {
            if (usbredirparser_do_write(faker->parser)) {
                return NULL;
            }
        }
    }
    return NULL;
}

void fake_usbredir_guest_start(FakeUsbredirGuest *faker)
{
    uint32_t caps[USB_REDIR_CAPS_SIZE] = {
        0,
    };
    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
    usbredirparser_init(faker->parser, VERSION, caps, USB_REDIR_CAPS_SIZE, 0);
    g_assert(usbredirparser_do_write(faker->parser) == 0);
    faker->parser_thread =
        g_thread_new("faker", fake_usbredir_guest_server_thread, faker);
}

void fake_usbredir_guest_stop(FakeUsbredirGuest *faker)
{
    g_thread_join(faker->parser_thread);
    g_mutex_clear(&faker->flag_mu);
    sem_destroy(&faker->helloed_sem);
    sem_destroy(&faker->if_info_sem);
    sem_destroy(&faker->ep_info_sem);
    sem_destroy(&faker->device_info_sem);

    if (faker->parser) {
        usbredirparser_destroy(faker->parser);
    }
}

bool fake_usbredir_guest_helloed(FakeUsbredirGuest *faker)
{
    struct timespec ts;
    int result;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    result = sem_timedwait(&faker->helloed_sem, &ts);
    return result == 0;
}

void fake_usbredir_guest_assert_num_interfaces(FakeUsbredirGuest *faker,
                                               int expected_num_interfaces)
{
    wait_for_flag(faker, &faker->if_info_sem, &faker->received_if_info);
    g_assert_cmphex(faker->if_info.interface_count, ==,
                    expected_num_interfaces);
}

void fake_usbredir_guest_assert_contains_interface(
    FakeUsbredirGuest *faker, const struct libusb_interface_descriptor *if_desc)
{
    uint8_t interface_num;

    wait_for_flag(faker, &faker->if_info_sem, &faker->received_if_info);
    interface_num = if_desc->bInterfaceNumber;

    g_assert_cmphex(faker->if_info.interface[interface_num], ==, interface_num);
    g_assert_cmphex(faker->if_info.interface_class[interface_num], ==,
                    if_desc->bInterfaceClass);
    g_assert_cmphex(faker->if_info.interface_subclass[interface_num], ==,
                    if_desc->bInterfaceSubClass);
    g_assert_cmphex(faker->if_info.interface_protocol[interface_num], ==,
                    if_desc->bInterfaceProtocol);
}

void fake_usbredir_guest_assert_num_endpoints(FakeUsbredirGuest *faker,
                                              int expected_num_endpoints)
{
    int actual_num_endpoints = 0;

    wait_for_flag(faker, &faker->ep_info_sem, &faker->received_ep_info);
    for (int i = 0; i < USB_REDIR_MAX_NUM_ENDPOINTS; i++) {
        if (faker->ep_info.type[i] != usb_redir_type_invalid) {
            ++actual_num_endpoints;
        }
    }

    g_assert_cmphex(actual_num_endpoints, ==, expected_num_endpoints);
}

void fake_usbredir_guest_assert_contains_endpoint(
    FakeUsbredirGuest *faker, const struct libusb_endpoint_descriptor *ep_desc,
    uint8_t interface_num)
{
    int ep_index;

    wait_for_flag(faker, &faker->ep_info_sem, &faker->received_ep_info);
    ep_index = EP_INDEX(ep_desc->bEndpointAddress);

    g_assert_cmphex(faker->ep_info.type[ep_index], ==,
                    ep_desc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK);
    g_assert_cmphex(faker->ep_info.interface[ep_index], ==, interface_num);
    g_assert_cmphex(faker->ep_info.max_packet_size[ep_index], ==,
                    ep_desc->wMaxPacketSize);
    g_assert_cmphex(faker->ep_info.interval[ep_index], ==, ep_desc->bInterval);
}

void fake_usbredir_guest_assert_device(
    FakeUsbredirGuest *faker,
    const struct libusb_device_descriptor *device_desc)
{
    sem_wait(&faker->device_info_sem);
    g_assert_cmphex(faker->device_info.speed, ==, usb_redir_speed_high);
    g_assert_cmphex(faker->device_info.device_class, ==,
                    device_desc->bDeviceClass);
    g_assert_cmphex(faker->device_info.device_subclass, ==,
                    device_desc->bDeviceSubClass);
    g_assert_cmphex(faker->device_info.device_protocol, ==,
                    device_desc->bDeviceProtocol);
    g_assert_cmphex(faker->device_info.device_version_bcd, ==,
                    device_desc->bcdUSB);
    g_assert_cmphex(faker->device_info.vendor_id, ==, device_desc->idVendor);
    g_assert_cmphex(faker->device_info.product_id, ==, device_desc->idProduct);
}

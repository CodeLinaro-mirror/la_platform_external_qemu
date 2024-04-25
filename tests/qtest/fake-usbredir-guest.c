/*
 * Fake USB Redirect Guest
 *
 * Copyright (C) 2024 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "fake-usbredir-guest.h"

#include <usbredirparser.h>
#include <usbredirproto.h>

#define VERSION "qemu fake usb-redir guest " QEMU_VERSION

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
    faker->helloed = true;
}

void fake_usbredir_guest_init(FakeUsbredirGuest *faker, int fd)
{
    faker->parser = usbredirparser_create();
    faker->fd = fd;
    faker->helloed = false;

    faker->parser->priv = faker;
    faker->parser->log_func = parser_log;
    faker->parser->read_func = parser_read;
    faker->parser->write_func = parser_write;
    faker->parser->hello_func = parser_hello;
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
    if (faker->parser) {
        usbredirparser_destroy(faker->parser);
    }
}

bool fake_usbredir_guest_helloed(FakeUsbredirGuest *faker)
{
    g_thread_join(faker->parser_thread);
    return faker->helloed;
}

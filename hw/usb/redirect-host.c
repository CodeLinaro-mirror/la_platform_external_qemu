/*
 * USB Redirector usb-host
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "hw/usb/redirect-host.h"

#include <usbredirparser.h>
#include <usbredirproto.h>

#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties-system.h"
#include "qapi/qapi-types-run-state.h"
#include "qemu/error-report.h"
#include "qemu/log-for-trace.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "system/runstate.h"

#define VERSION "qemu usb-redir host " QEMU_VERSION

static void usbredir_host_parser_log(void *priv, int level, const char *msg)
{
    switch (level) {
    case usbredirparser_error:
        error_report("[ERROR] usbredir-host: %s", msg);
        break;
    case usbredirparser_warning:
        error_report("[WARNING] usbredir-host: %s", msg);
        break;
    case usbredirparser_info:
        error_report("[INFO] usbredir-host: %s", msg);
        break;
    default:
        qemu_log_mask(LOG_TRACE, "[DEBUG] usbredir-host: %s", msg);
    }
}

static int usbredir_host_parser_read(void *priv, uint8_t *data, int count)
{
    USBRedirectHost *usbredir_host = priv;

    if (!usbredir_host->read_cache) {
        return 0;
    }

    if (usbredir_host->read_size < count) {
        count = usbredir_host->read_size;
    }

    memcpy(data, usbredir_host->read_cache, count);

    usbredir_host->read_size -= count;
    if (usbredir_host->read_size) {
        usbredir_host->read_cache += count;
    } else {
        usbredir_host->read_cache = NULL;
    }

    return count;
}

static gboolean usbredir_host_parser_write_ready(void *_, GIOCondition cond,
                                                 void *opaque)
{
    USBRedirectHost *usbredir_host = opaque;
    usbredir_host->write_ready_watch = 0;
    usbredirparser_do_write(usbredir_host->parser);

    return FALSE;
}

static int usbredir_host_parser_write(void *priv, uint8_t *data, int count)
{
    USBRedirectHost *usbredir_host = priv;
    int written_bytes = 0;

    if (!qemu_chr_fe_backend_open(&usbredir_host->chr)) {
        return 0;
    }

    if (!runstate_check(RUN_STATE_RUNNING)) {
        return 0;
    }

    written_bytes = qemu_chr_fe_write(&usbredir_host->chr, data, count);

    if (written_bytes < count) {
        if (!usbredir_host->write_ready_watch) {
            usbredir_host->write_ready_watch = qemu_chr_fe_add_watch(
                &usbredir_host->chr, G_IO_OUT | G_IO_HUP,
                usbredir_host_parser_write_ready, usbredir_host);
        }
        if (written_bytes < 0) {
            written_bytes = 0;
        }
    }

    return written_bytes;
}

static void usbredir_host_parser_hello(void *priv,
                                       struct usb_redir_hello_header *h)
{
    USBRedirectHost *usbredir_host = priv;
    usbredir_host->device_ops->on_attach(usbredir_host->opaque);
}

static void usbredir_host_parser_reset(void *priv)
{
    USBRedirectHost *usbredir_host = priv;
    usbredir_host->device_ops->reset(usbredir_host->opaque);
}

static void usbredir_host_create_parser(USBRedirectHost *usbredir_host)
{
    uint32_t caps[USB_REDIR_CAPS_SIZE] = {
        0,
    };

    usbredir_host->parser = usbredirparser_create();

    if (!usbredir_host->parser) {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(usbredir_host));
        error_report("%s: usbredirparser_create() failed", path);
        exit(1);
    }

    usbredir_host->parser->priv = usbredir_host;
    usbredir_host->parser->log_func = usbredir_host_parser_log;
    usbredir_host->parser->read_func = usbredir_host_parser_read;
    usbredir_host->parser->write_func = usbredir_host_parser_write;
    usbredir_host->parser->reset_func = usbredir_host_parser_reset;
    usbredir_host->parser->hello_func = usbredir_host_parser_hello;

    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_32bits_bulk_length);
    usbredirparser_init(usbredir_host->parser, VERSION, caps,
                        USB_REDIR_CAPS_SIZE, usbredirparser_fl_usb_host);
    usbredirparser_do_write(usbredir_host->parser);
}

static void usbredir_host_destroy_parser(USBRedirectHost *usbredir_host)
{
    if (usbredir_host->parser) {
        usbredirparser_destroy(usbredir_host->parser);
    }
}

static void usbredir_host_chardev_event_handler(void *opaque,
                                                QEMUChrEvent event)
{
    switch (event) {
    case CHR_EVENT_OPENED:
        usbredir_host_create_parser(USB_REDIR_HOST(opaque));
        break;
    case CHR_EVENT_CLOSED:
        usbredir_host_destroy_parser(USB_REDIR_HOST(opaque));
    default:
        break;
    }
}

static void usbredir_host_chardev_read(void *opaque, const uint8_t *buf,
                                       int size)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(opaque);

    usbredir_host->read_cache = buf;
    usbredir_host->read_size = size;

    usbredirparser_do_read(usbredir_host->parser);
}

static int usbredir_host_can_read(void *opaque)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(opaque);

    if (!usbredir_host->parser) {
        return 0;
    }

    if (!runstate_check(RUN_STATE_RUNNING)) {
        return 0;
    }

    return 1 * MiB;
}

static void usbredir_host_realize(DeviceState *dev, Error **errp)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(dev);

    if (qemu_chr_fe_backend_connected(&usbredir_host->chr)) {
        qemu_chr_fe_set_handlers(&usbredir_host->chr, usbredir_host_can_read,
                                 usbredir_host_chardev_read,
                                 usbredir_host_chardev_event_handler, NULL,
                                 usbredir_host, NULL, true);
    } else {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(dev));
        qemu_log_mask(LOG_TRACE, "%s: continuing without chardev", path);
    }
}

static void usbredir_host_unrealize(DeviceState *dev)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(dev);
    qemu_chr_fe_deinit(&usbredir_host->chr, true);
    usbredir_host_destroy_parser(usbredir_host);
}

static const Property usbredir_host_properties[] = {
    DEFINE_PROP_CHR("chardev", USBRedirectHost, chr),
};

static void usbredir_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, usbredir_host_properties);
    dc->desc = "USB Redirect Host";
    dc->realize = usbredir_host_realize;
    dc->unrealize = usbredir_host_unrealize;
}

static const TypeInfo usbredir_host_info = {
    .name = TYPE_USB_REDIR_HOST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(USBRedirectHost),
    .class_init = usbredir_host_class_init,
};

static void usbredir_host_register_type(void)
{
    type_register_static(&usbredir_host_info);
}

type_init(usbredir_host_register_type)

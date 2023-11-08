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

#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties-system.h"
#include "qemu/log-for-trace.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"

static void usbredir_host_event_handler(void *opaque, QEMUChrEvent event)
{
    /* TODO(b/309701600): To be implemented in the following CLs. */
}

static void usbredir_host_read(void *opaque, const uint8_t *buf, int size)
{
    /* TODO(b/309701600): To be implemented in the following CLs. */
}

static int usbredir_host_can_read(void *opaque)
{
    /* TODO(b/309701600): To be implemented in the following CLs. */
    return 0;
}

static void usbredir_host_realize(DeviceState *dev, Error **errp)
{
    USBRedirectHost *usbredir_host = USB_REDIR_HOST(dev);

    if (qemu_chr_fe_backend_connected(&usbredir_host->chr)) {
        qemu_chr_fe_set_handlers(
            &usbredir_host->chr, usbredir_host_can_read, usbredir_host_read,
            usbredir_host_event_handler, NULL, usbredir_host, NULL, true);
    } else {
        g_autofree char *path = object_get_canonical_path(
            OBJECT(dev));
        qemu_log_mask(LOG_TRACE, "%s: continuing without chardev", path);
    }
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

/*
 * USB Redirector usb-host
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef REDIRECT_HOST_H
#define REDIRECT_HOST_H

#include "chardev/char-fe.h"
#include "hw/qdev-core.h"
#include "qemu/typedefs.h"

#define TYPE_USB_REDIR_HOST "usbredir-host"
OBJECT_DECLARE_SIMPLE_TYPE(USBRedirectHost, USB_REDIR_HOST)

typedef struct USBRedirectHost {
    DeviceState parent_obj;
    CharBackend chr;
} USBRedirectHost;

#endif /* REDIRECT_HOST_H */

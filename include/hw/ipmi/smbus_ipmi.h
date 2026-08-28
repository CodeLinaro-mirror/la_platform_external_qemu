/*
 * QEMU IPMI SMBus (SSIF) emulation
 *
 * Copyright (c) 2015,2016 Corey Minyard, MontaVista Software, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SMBUS_IPMI_H
#define SMBUS_IPMI_H

#define SSIF_IPMI_REQUEST                       2
#define SSIF_IPMI_MULTI_PART_REQUEST_START      6
#define SSIF_IPMI_MULTI_PART_REQUEST_MIDDLE     7
#define SSIF_IPMI_MULTI_PART_REQUEST_END        8
#define SSIF_IPMI_RESPONSE                      3
#define SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE    9
#define SSIF_IPMI_MULTI_PART_RETRY              0xa

#define MAX_SSIF_IPMI_MSG_SIZE 255
#define MAX_SSIF_IPMI_MSG_CHUNK 32

#define IPMI_GET_SYS_INTF_CAP_CMD 0x57

#endif /* SMBUS_IPMI_H */

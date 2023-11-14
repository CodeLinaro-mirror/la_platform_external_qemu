/*
 * Nuvoton NPCM8mnx USB 2.0 Device Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef NPCM8XX_UDC_H
#define NPCM8XX_UDC_H

#include "hw/sysbus.h"
#include "hw/usb/redirect-host.h"
#include "qemu/typedefs.h"
#include "qom/object.h"

#define TYPE_NPCM8XX_UDC "npcm8xx-udc"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM8xxUDC, NPCM8XX_UDC)

typedef struct TransferDescriptor {
    uint32_t next_pointer;
    uint32_t info;
    uint32_t buffer_pointers[5];
} TransferDescriptor;

#define TD_NEXT_POINTER_VALID_MASK 1
#define TD_INFO_TOTAL_BYTES_SHIFT 16
#define TD_INFO_TOTAL_BYTES_MASK 0x7FFF0000
#define TD_INFO_INTERRUPT_ON_COMPLETE_MASK 0x8000
#define TD_INFO_STATUS_MASK 0xF

typedef struct QueueHead {
    uint32_t endpoint_info;
    uint32_t current_pointer;
    TransferDescriptor td;
    uint32_t reserved;
    uint32_t setup[2];
    uint32_t padding[4];
} QueueHead;

#define QH_EP_INFO_MAX_PACKET_LENGTH_SHIFT 16
#define QH_EP_INFO_MAX_PACKET_LENGTH_MASK 0x3FF0000
#define QH_EP_INFO_INTERRUPT_ON_SETUP_MASK 0x8000

typedef struct NPCM8xxUDCRegisters {
    uint32_t command;
    uint32_t status;
    uint32_t interrupt_enable;
    uint32_t endpoint_list_address;
    uint32_t port_control_status;
    uint32_t mode;
    uint32_t endpoint_setup_status;
    uint32_t endpoint_prime;
    uint32_t endpoint_flush;
    uint32_t endpoint_status;
    uint32_t endpoint_complete;
    uint32_t ep0_control;
    uint32_t ep1_control;
    uint32_t ep2_control;
} NPCM8xxUDCRegisters;

#define NPCM8XX_UDC_NUM_REGS (sizeof(NPCM8xxUDCRegisters) / 4)

typedef struct NPCM8xxUDC {
    SysBusDevice parent;
    MemoryRegion mr;
    qemu_irq irq;
    uint8_t device_index;

    const USBRedirectHostOps *usbredir_ops;

    /*
     * Registers are stored as array instead of NPCM8xxUDCRegisters because
     * I want to store registers value into the VM state description without
     * manually declaring and storing VM state description fields for each
     * register.
     */
    uint32_t registers[NPCM8XX_UDC_NUM_REGS];
    bool running;
    bool attached;
} NPCM8xxUDC;

#endif /* NPCM8XX_UDC_H */

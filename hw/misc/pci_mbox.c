/*
 * This is a generic chardev backend based PCI Mailbox memory mmio module.
 * Inspried by the Nuvoton PCI Mailbox (npc7xx_pci_mbox.c).
 *
 * Copyright 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */


#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/pci_mbox.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "trace.h"

enum PCIMBoxOperation {
    PCI_MBOX_OP_READ = 1,
    PCI_MBOX_OP_WRITE,
};

#define PCI_MBOX_OFFSET_BYTES 8

/* Response code */
#define PCI_MBOX_OK 0
#define PCI_MBOX_INVALID_OP 0xa0
#define PCI_MBOX_INVALID_SIZE 0xa1
#define PCI_MBOX_UNSPECIFIED_ERROR 0xff

#define PCI_MBOX_NR_CI 8
#define PCI_MBOX_CI_MASK MAKE_64BIT_MASK(0, PCI_MBOX_NR_CI)

static void pci_mbox_send_response(PCIMBoxState *s, uint8_t code)
{
    qemu_chr_fe_write(&s->chr, &code, 1);
    if (code == PCI_MBOX_OK && s->op == PCI_MBOX_OP_READ) {
        qemu_chr_fe_write(&s->chr, (uint8_t *)(&s->data), s->size);
    }
}

static void pci_mbox_handle_read(PCIMBoxState *s)
{
    uint8_t offset_bytes[4];
    MemTxResult r = memory_region_dispatch_read(
        &s->ram, s->offset, &s->data, MO_LE | size_memop(s->size),
        MEMTXATTRS_UNSPECIFIED);

    stl_le_p(offset_bytes, r);
    pci_mbox_send_response(s, offset_bytes[0]);
}

static void pci_mbox_handle_write(PCIMBoxState *s)
{
    uint8_t offset_bytes[4];
    MemTxResult r = memory_region_dispatch_write(
        &s->ram, s->offset, s->data, MO_LE | size_memop(s->size),
        MEMTXATTRS_UNSPECIFIED);

    stl_le_p(offset_bytes, r);
    pci_mbox_send_response(s, offset_bytes[0]);
}

/*
 * The device is using a Little Endian Protocol.
 * If running into errors, please check what protocol is being expected.
 */
static void pci_mbox_receive_char(PCIMBoxState *s, uint8_t byte)
{
    switch (s->state) {
    case PCI_MBOX_STATE_IDLE:
        switch (byte) {
        case PCI_MBOX_OP_READ:
        case PCI_MBOX_OP_WRITE:
            s->op = byte;
            s->state = PCI_MBOX_STATE_OFFSET;
            s->offset = 0;
            s->receive_count = 0;
            break;

        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                "received invalid op type: 0x%" PRIx8, byte);
            pci_mbox_send_response(s, PCI_MBOX_INVALID_OP);
            break;
        }
        break;

    case PCI_MBOX_STATE_OFFSET:
        s->offset += (uint64_t)byte << (s->receive_count * BITS_PER_BYTE);
        if (++s->receive_count >= PCI_MBOX_OFFSET_BYTES) {
            s->state = PCI_MBOX_STATE_SIZE;
        }
        break;

    case PCI_MBOX_STATE_SIZE:
        s->size = byte;
        if (s->size < 1 || s->size > sizeof(uint64_t)) {
            qemu_log_mask(LOG_GUEST_ERROR, "received invalid size: %u", byte);
            pci_mbox_send_response(s, PCI_MBOX_INVALID_SIZE);
            s->state = PCI_MBOX_STATE_IDLE;
            break;
        }
        if (s->op == PCI_MBOX_OP_READ) {
            pci_mbox_handle_read(s);
            s->state = PCI_MBOX_STATE_IDLE;
        } else {
            s->receive_count = 0;
            s->data = 0;
            s->state = PCI_MBOX_STATE_DATA;
        }
        break;

    case PCI_MBOX_STATE_DATA:
        g_assert(s->op == PCI_MBOX_OP_WRITE);
        s->data += (uint64_t)byte << (s->receive_count * BITS_PER_BYTE);
        if (++s->receive_count >= s->size) {
            pci_mbox_handle_write(s);
            s->state = PCI_MBOX_STATE_IDLE;
        }
        break;

    default:
        g_assert_not_reached();
    }
}

static void pci_mbox_enter_reset(Object *obj, ResetType type)
{
    PCIMBoxState *s = PCI_MBOX(obj);

    s->state = PCI_MBOX_STATE_IDLE;
    s->receive_count = 0;
}

static int can_receive(void *opaque)
{
    return 1;
}

static void receive(void *opaque, const uint8_t *buf, int size)
{
    PCIMBoxState *s = PCI_MBOX(opaque);
    int i;

    for (i = 0; i < size; ++i) {
        pci_mbox_receive_char(s, buf[i]);
    }
}

static void chr_event(void *opaque, QEMUChrEvent event)
{
    switch (event) {
    case CHR_EVENT_OPENED:
    case CHR_EVENT_CLOSED:
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;

    default:
        g_assert_not_reached();
    }
}

static void pci_mbox_realize(DeviceState *dev, Error **errp)
{
    PCIMBoxState *s = PCI_MBOX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* MMIO region for BMC */
    s->content = g_malloc(s->ram_size);
    memory_region_init_ram_device_ptr(&s->ram, OBJECT(dev),
                                      "pci-mbox-ram", s->ram_size, s->content);
    sysbus_init_mmio(sbd, &s->ram);

    /* Chardev backend for PcieEndpoint */
    qemu_chr_fe_set_handlers(&s->chr, can_receive, receive,
                             chr_event, NULL, OBJECT(dev), NULL, true);
}

static const Property pci_mbox_properties[] = {
    DEFINE_PROP_CHR("chardev", PCIMBoxState, chr),
    DEFINE_PROP_UINT32("ram-size", PCIMBoxState, ram_size, PCI_MBOX_RAM_SIZE),
};

static void pci_mbox_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "PCI Mailbox Controller";
    dc->realize = pci_mbox_realize;
    rc->phases.enter = pci_mbox_enter_reset;
    device_class_set_props(dc, pci_mbox_properties);
}

static const TypeInfo pci_mbox_info = {
    .name               = TYPE_PCI_MBOX,
    .parent             = TYPE_SYS_BUS_DEVICE,
    .instance_size      = sizeof(PCIMBoxState),
    .class_init         = pci_mbox_class_init,
};

static void pci_mbox_register_type(void)
{
    type_register_static(&pci_mbox_info);
}
type_init(pci_mbox_register_type);

/*
 * NXP's OSTIMER Module
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "hw/timer/ostimer.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"

#include "trace.h"

#define REG(s, reg) (s->regs[R_OSTIMER_##reg])
#define RF_RD(s, reg, field) \
    ARRAY_FIELD_EX32(s->regs, OSTIMER_##reg, field)
#define RF_WR(s, reg, field, val) \
    ARRAY_FIELD_DP32(s->regs, OSTIMER_##reg, field, val)

static const OSTIMER_REGISTER_ACCESS_INFO_ARRAY(reg_info);

static inline void capture(OsTimerState *s)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t count = (now - s->reset_time) * clock_get_hz(s->clk) /
        NANOSECONDS_PER_SECOND;

    REG(s, CAPTURE_L) = to_gray(count) & 0xffffffff;
    REG(s, CAPTURE_H) = to_gray(count) >> 32;
}

static MemTxResult ostimer_read(void *opaque, hwaddr addr,
                                uint64_t *data, unsigned size,
                                MemTxAttrs attrs)
{
    OsTimerState *s = opaque;
    const struct RegisterAccessInfo *rai = &reg_info[addr / 4];

    switch (addr) {
    case A_OSTIMER_EVTIMERL:
        capture(s);
        *data = REG(s, CAPTURE_L);
        break;
    case A_OSTIMER_EVTIMERH:
        capture(s);
        *data = REG(s, CAPTURE_H);
        break;
    default:
        *data = s->regs[addr / 4];
        break;
    }

    trace_ostimer_reg_read(rai->name, addr, *data);
    return MEMTX_OK;
}

static MemTxResult ostimer_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size,
                                 MemTxAttrs attrs)
{
    OsTimerState *s = opaque;
    const struct RegisterAccessInfo *rai = &reg_info[addr / 4];
    struct RegisterInfo ri = {
        .data = &s->regs[addr / 4],
        .data_size = 4,
        .access = rai,
    };

    trace_ostimer_reg_write(rai->name, addr, value);

    register_write(&ri, value, ~0, NULL, false);

    switch (addr) {
    case A_OSTIMER_OSEVENT_CTRL:
    {
        if (value & R_OSTIMER_OSEVENT_CTRL_OSTIMER_INTRFLAG_MASK) {
            RF_WR(s, OSEVENT_CTRL, OSTIMER_INTRFLAG, 0);
            qemu_set_irq(s->irq, 0);
        }
        break;
    }
    case A_OSTIMER_MATCH_H:
    {
        uint64_t match_gray;

        match_gray = REG(s, MATCH_L);
        match_gray |= (uint64_t)REG(s, MATCH_H) << 32;

        timer_mod(s->timer, s->reset_time +
                  from_gray(match_gray) * NANOSECONDS_PER_SECOND /
                  clock_get_hz(s->clk));
        break;
    }
    }

    return MEMTX_OK;
}

static void ostimer_event(void *opaque)
{
    OsTimerState *s = opaque;

    trace_ostimer_event();
    RF_WR(s, OSEVENT_CTRL, OSTIMER_INTRFLAG, 1);
    if (RF_RD(s, OSEVENT_CTRL, OSTIMER_INTENA)) {
        qemu_set_irq(s->irq, 1);
    }
}

static const MemoryRegionOps ostimer_ops = {
    .read_with_attrs = ostimer_read,
    .write_with_attrs = ostimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void ostimer_reset(Object *obj, ResetType type)
{
    OsTimerState *s = OSTIMER(obj);

    for (int i = 0; i < OSTIMER_REGS_NO; i++) {
        hwaddr addr = reg_info[i].addr;

    /* no soft-reset for CAPTURE_L and CAPTURE_H */
    if (addr != -1 && addr != A_OSTIMER_CAPTURE_L &&
        addr != A_OSTIMER_CAPTURE_H) {
            struct RegisterInfo ri = {
                .data = &s->regs[addr / 4],
                .data_size = 4,
                .access = &reg_info[i],
            };

            register_reset(&ri);
        }
    }

    s->reset_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* TODO(b/339725758): implement module reset via RT500 RSTCTL */
static void ostimer_module_reset(OsTimerState *s)
{
    /* Only the POR and module reset can reset the shared EVTimer */
    REG(s, EVTIMERL) = 0;
    REG(s, EVTIMERH) = 0;
}

static void ostimer_init(Object *obj)
{
    OsTimerState *s = OSTIMER(obj);

    memory_region_init_io(&s->mmio, obj, &ostimer_ops, s,
                          TYPE_OSTIMER, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void ostimer_realize(DeviceState *dev, Error **errp)
{
    OsTimerState *s = OSTIMER(dev);

    ostimer_module_reset(s);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ostimer_event, s);
}

static void ostimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = ostimer_reset;
    dc->realize = ostimer_realize;
}

static const TypeInfo ostimer_types[] = {
    {
        .name          = TYPE_OSTIMER,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(OsTimerState),
        .instance_init = ostimer_init,
        .class_init    = ostimer_class_init,
    }
};

DEFINE_TYPES(ostimer_types);

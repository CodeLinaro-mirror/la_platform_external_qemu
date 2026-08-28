/*
 * i.MX RT595 EVK
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "elf.h"
#include "system/address-spaces.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/boards.h"
#include "qemu/log.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "qapi/error.h"
#include "hw/arm/rt500.h"
#include "hw/core/qdev-clock.h"
#include "system/reset.h"

static void rt595_evk_reset(MachineState *ms, ResetType reason)
{
    /*
     * CPU reset is not done by default, we need to do it manually when the
     * machine is reset.
     */
    cpu_reset(first_cpu);

    qemu_devices_reset(reason);
}

static void rt595_evk_init(MachineState *ms)
{
    RT500State *s;
    Clock *sysclk;

    sysclk = clock_new(OBJECT(ms), "SYSCLK");
    clock_set_hz(sysclk, 192000000);

    s = RT500(object_new(TYPE_RT500));
    qdev_connect_clock_in(DEVICE(s), "sysclk", sysclk);
    object_property_add_child(OBJECT(ms), "soc", OBJECT(s));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    if (ms->kernel_filename) {
        armv7m_load_kernel(ARM_CPU(first_cpu), ms->kernel_filename, 0, 0);
    }
}

static void rt595_evk_machine_init(MachineClass *mc)
{
    mc->desc  = "RT595 EVK Machine (ARM Cortex-M33)";
    mc->init  = rt595_evk_init;
    mc->reset = rt595_evk_reset;

    mc->ignore_memory_transaction_failures = true;
}

DEFINE_MACHINE_ARM("rt595-evk", rt595_evk_machine_init);

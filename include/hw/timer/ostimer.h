/*
 * NXP's OSTIMER Module
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_OSTIMER_H
#define HW_TIMER_OSTIMER_H


#include "hw/arm/svd/ostimer.h"
#include "hw/core/sysbus.h"

#define TYPE_OSTIMER "ostimer"
#define OSTIMER(o) OBJECT_CHECK(OsTimerState, o, TYPE_OSTIMER)

typedef struct {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[OSTIMER_REGS_NO];
    QEMUTimer *timer;
    qemu_irq irq;
    uint64_t reset_time;
    Clock *clk;
} OsTimerState;

static inline uint64_t to_gray(uint64_t n)
{
    return n ^ (n >> 1);
}

static inline uint64_t from_gray(uint64_t n)
{
  n ^= n >> 32;
  n ^= n >> 16;
  n ^= n >> 8;
  n ^= n >> 4;
  n ^= n >> 2;
  n ^= n >> 1;

  return n;
}

#endif /* HW_TIMER_OSTIMER_H */

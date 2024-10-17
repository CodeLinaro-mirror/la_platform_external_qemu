/*
 * NXPS's OSTIMER tests.
 *
 * Copyright (c) 2024 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"

#include "hw/timer/ostimer.h"
#include "hw/misc/rt500_clkctl1.h"
#include "hw/arm/svd/rt500.h"
#include "reg-utils.h"

#define OSTIMER_BASE RT500_OSTIMER0_BASE
#define DEVICE_NAME "/machine/soc/ostimer"

/* get a minimum period in nanoseconds for a given frequency */
static long get_min_ns_period_for_freq(long freq)
{
    g_assert(freq <= 1000000);

    return 1000000000 / freq + 1;
}

static void clksel(void *fixture, gconstpointer data)
{
    long hz = (long)data;
    QDict *resp;

    resp = qmp("{\"execute\": \"system_reset\"}");
    qdict_unref(resp);

    switch (hz) {
    case 32000:
    {
        REG32_WRITE_FIELD(RT500_CLKCTL1, OSEVENTTFCLKSEL, SEL,
                          OSEVENTTFCLKSEL_32KHZRTC);
        break;
    }
    case 1000000:
    {
        REG32_WRITE_FIELD(RT500_CLKCTL1, OSEVENTTFCLKSEL, SEL,
                          OSEVENTTFCLKSEL_LPOSC);
        break;
    }
    }
}

static void evtimer_test(void *fixture, gconstpointer user_data)
{
    uint64_t timer1, timer2;
    long hz = (long)user_data;
    long period = get_min_ns_period_for_freq(hz);

    timer1 = REG32_READ(OSTIMER, EVTIMERL);
    timer1 |= (uint64_t)REG32_READ(OSTIMER, EVTIMERH) << 32;
    timer1 = from_gray(timer1);

    clock_step(period);

    timer2 = REG32_READ(OSTIMER, EVTIMERL);
    timer2 |= (uint64_t)REG32_READ(OSTIMER, EVTIMERH) << 32;
    timer2 = from_gray(timer2);

    g_assert_cmpuint(timer1, <, timer2);
}

static void capture_test(void *fixture, gconstpointer user_data)
{
    uint32_t capture;

    /* trigger a capture */
    REG32_READ(OSTIMER, EVTIMERL);

    /* read the captured value */
    capture = REG32_READ(OSTIMER, CAPTURE_L);

    clock_step(100);

    /* check that the capture value has not changed */
    g_assert_cmpuint(capture, ==, REG32_READ(OSTIMER, CAPTURE_L));
}

static void match_test(void *fixture, gconstpointer user_data)
{
    uint64_t now, match;
    long hz = (long)user_data;
    long period = get_min_ns_period_for_freq(hz);

    qtest_irq_intercept_out_named(global_qtest, DEVICE_NAME,
                                  SYSBUS_DEVICE_GPIO_IRQ);

    /* interrupt not asserted after reset */
    g_assert_cmpuint(REG32_READ_FIELD(OSTIMER, OSEVENT_CTRL, OSTIMER_INTRFLAG),
                     ==, 0);

    /* ... and not enabled after reset */
    g_assert_cmpuint(REG32_READ_FIELD(OSTIMER, OSEVENT_CTRL, OSTIMER_INTENA),
                     ==, 0);

    /* enable interrupts */
    REG32_WRITE_FIELD(OSTIMER, OSEVENT_CTRL, OSTIMER_INTENA, 1);
    g_assert_cmpuint(REG32_READ_FIELD(OSTIMER, OSEVENT_CTRL, OSTIMER_INTENA),
                     ==, 1);

    now = REG32_READ(OSTIMER, EVTIMERL);
    now |= (uint64_t)REG32_READ(OSTIMER, EVTIMERH) << 32;
    now = from_gray(now);

    match = now + 1;
    match = to_gray(match);

    /* setup timer */
    REG32_WRITE(OSTIMER, MATCH_L, (uint32_t)(match & 0xffffffff));
    REG32_WRITE(OSTIMER, MATCH_H, (uint32_t)(match >> 32));

    g_assert_false(get_irq(0));

    /* wait */
    clock_step(period);

    /* check that the intr flag has been set */
    g_assert_cmpuint(REG32_READ_FIELD(OSTIMER, OSEVENT_CTRL, OSTIMER_INTRFLAG),
                     ==, 1);

    /* check that the irq has been triggered */
    g_assert_true(get_irq(0));

    /* clear the interrupt */
    REG32_WRITE_FIELD(OSTIMER, OSEVENT_CTRL, OSTIMER_INTRFLAG, 1);

    /* and check that the interrupt has been cleared */
    g_assert_false(get_irq(0));
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    g_test_add("/rt500-ostimer/32khz-evtimer_test", void,
               (gconstpointer)32000, clksel, evtimer_test, NULL);
    g_test_add("/ostimer/32khz-capture_test", void,
               (gconstpointer)32000, clksel, capture_test, NULL);
    g_test_add("/ostimer/32khz-match_test", void,
               (gconstpointer)32000, clksel, match_test, NULL);
    g_test_add("/ostimer/1mhz-evtimer_test", void,
               (gconstpointer)1000000, clksel, evtimer_test, NULL);
    g_test_add("/ostimer/1mhz-capture_test", void,
               (gconstpointer)1000000, clksel, capture_test, NULL);
    g_test_add("/ostimer/1mhz-match_test", void,
               (gconstpointer)1000000, clksel, match_test, NULL);

    qtest_start("-M rt595-evk");
    ret = g_test_run();
    qtest_end();

    return ret;
}

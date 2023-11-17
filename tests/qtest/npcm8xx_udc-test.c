/*
 * QTests for Nuvoton NPCM8mnx USB Device Controller
 *
 * Copyright (C) 2023 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define NPCM8xxUDC_BASE_ADDR 0xf0830000

/* Register offsets */
#define R_DCCPARAMS 0x124
#define R_USBSTS 0x144
#define R_PORTSC1 0x184
#define R_ENDPTCTRL0 0x1C0

static void test_register_access(void)
{
    const uint32_t test_write_value = 0xffffffff;
    uint32_t init_value = 0;
    uint32_t new_value = 0;

    /* Test DCCPARAMS register */
    init_value = readl(NPCM8xxUDC_BASE_ADDR + R_DCCPARAMS);
    writel(NPCM8xxUDC_BASE_ADDR + R_DCCPARAMS, ~init_value);
    g_assert_cmphex(readl(NPCM8xxUDC_BASE_ADDR + R_DCCPARAMS), ==, init_value);

    /* Test USBSTS register */
    init_value = readl(NPCM8xxUDC_BASE_ADDR + R_USBSTS);
    writel(NPCM8xxUDC_BASE_ADDR + R_USBSTS, test_write_value);

    new_value = readl(NPCM8xxUDC_BASE_ADDR + R_USBSTS);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0x100);

    /* Test PORTSC1 register */
    init_value = readl(NPCM8xxUDC_BASE_ADDR + R_PORTSC1);
    writel(NPCM8xxUDC_BASE_ADDR + R_PORTSC1, test_write_value);

    new_value = readl(NPCM8xxUDC_BASE_ADDR + R_PORTSC1);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0xDBFFF27E);

    /* Test ENDPTCTRL0 register */
    init_value = readl(NPCM8xxUDC_BASE_ADDR + R_ENDPTCTRL0);
    writel(NPCM8xxUDC_BASE_ADDR + R_ENDPTCTRL0, test_write_value);

    new_value = readl(NPCM8xxUDC_BASE_ADDR + R_ENDPTCTRL0);
    g_assert_cmphex(new_value, !=, init_value);
    g_assert_cmphex(new_value, ==, 0xffffffff);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    g_test_set_nonfatal_assertions();

    qtest_add_func("/npcm8xx_udc/register_access", test_register_access);

    qtest_start("-machine npcm845-evb");
    ret = g_test_run();
    qtest_end();

    return ret;
}

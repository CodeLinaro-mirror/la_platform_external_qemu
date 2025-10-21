/*
 * QEMU Hypervisor.framework (HVF) support
 *
 * Copyright Google Inc., 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* header to be included in non-HVF-specific code */

#ifndef HVF_H
#define HVF_H

#include "qemu/accel.h"

#ifdef COMPILING_PER_TARGET
# ifdef CONFIG_HVF
#  define CONFIG_HVF_IS_POSSIBLE
# endif /* !CONFIG_HVF */
#else
# define CONFIG_HVF_IS_POSSIBLE
#endif /* COMPILING_PER_TARGET */

#ifdef CONFIG_HVF_IS_POSSIBLE
extern bool hvf_allowed;
#define hvf_enabled() (hvf_allowed)
#else /* !CONFIG_HVF_IS_POSSIBLE */
#define hvf_enabled() 0
#endif /* !CONFIG_HVF_IS_POSSIBLE */

#define TYPE_HVF_ACCEL ACCEL_CLASS_NAME("hvf")

typedef struct HVFState HVFState;
DECLARE_INSTANCE_CHECKER(HVFState, HVF_STATE,
                         TYPE_HVF_ACCEL)

#if defined(__APPLE__) && defined(__MACH__)
//todo: revert the CL intrducing the following function
//      after we don't support MacOS 12 or lower, as we
//      don't want to diverge from the upstream QEMU.
static int isMacOS13orAbove(void)
{
    size_t buf_len = 0;
    char *osVersion = NULL;
    int ret = 0;

    if (sysctlbyname("kern.osproductversion", NULL,
                     &buf_len, NULL, 0) != 0) {
        return 0;
    }

    osVersion = malloc(buf_len);
    if (!osVersion) {
        return 0;
    }

    if (sysctlbyname("kern.osproductversion", osVersion,
                      &buf_len, NULL, 0) != 0) {
        return 0;
    }

    ret = strcmp(osVersion, "13") >= 0;
    free(osVersion);
    return ret;
}
#endif

#endif

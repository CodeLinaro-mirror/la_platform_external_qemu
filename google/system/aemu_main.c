/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2020 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <limits.h>

#include <gmodule.h>

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "system/system.h"

#include "aemu_func_defs.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

 // Note this file is almost a duplicate of system/main.c
 // The main difference is that this will dynamically load the goldfish plugins
 // library.

int qemu_default_main(void) {
  int status;

  status = qemu_main_loop();
  qemu_cleanup(status);

  return status;
}

static const char *const LIB_PATH_ENV_VAR = "ANDROID_EMULATOR_LIBRARY_DIR";

#ifdef _WIN32
#define LIB_PREFIX ""
#else
#define LIB_PREFIX "lib"
#endif

#ifndef TARGET_NAME
#error CPU architecture not set
#endif

typedef void (*gf_register_types_func_t)(void);
typedef void (*gf_startup_func_t)(int argc, char **argv);
typedef void (*gf_shutdown_func_t)(void);

static bool module_load_gf(bool export_symbols, gf_startup_func_t *startup_func, gf_shutdown_func_t *shutdown_func) {
    char *module_dir = getenv(LIB_PATH_ENV_VAR);
    if (module_dir == NULL) {
        fprintf(stderr, "error: Cannot load goldfish module because environment variable %s is not set.\n", LIB_PATH_ENV_VAR);
        return false;
    }
    char full_path[PATH_MAX];
    if (snprintf(full_path, sizeof(full_path), "%s/" LIB_PREFIX "goldfish_" TARGET_NAME, module_dir) < 0) {
      fprintf(stderr, "error: Failed to generate goldfish module path using directory: %s\n", module_dir);
      return false;
    }

    GModule *g_module;
    int flags;

    flags = 0;
    if (!export_symbols) {
        flags |= G_MODULE_BIND_LOCAL;
    }
    g_module = g_module_open(full_path, (GModuleFlags)flags);
    if (!g_module) {
        fprintf(stderr, "error: Failed to load goldfish module from \"%s\": %s\n", full_path, g_module_error());
        return false;
    }

    gf_register_types_func_t gf_register_types_func = NULL;
    if (!g_module_symbol(g_module, TOSTRING(GF_REGISTER_TYPES_FUNC), (gpointer *)&gf_register_types_func)) {
        fprintf(stderr, "error: Failed to find required symbol \"%s\" in goldfish module.\n", TOSTRING(GF_REGISTER_TYPES_FUNC));
        return false;
    }
    if (!g_module_symbol(g_module, TOSTRING(GF_STARTUP_FUNC), (gpointer *)startup_func)) {
        fprintf(stderr, "error: Failed to find required symbol \"%s\" in goldfish module.\n", TOSTRING(GF_STARTUP_FUNC));
        return false;
    }
    if (!g_module_symbol(g_module, TOSTRING(GF_SHUTDOWN_FUNC), (gpointer *)shutdown_func)) {
        fprintf(stderr, "error: Failed to find required symbol \"%s\" in goldfish module.\n", TOSTRING(GF_SHUTDOWN_FUNC));
        return false;
    }

    register_module_init(gf_register_types_func, MODULE_INIT_QOM);

    return true;
}

int main(int argc, char **argv) {
  gf_startup_func_t gf_startup_func = NULL;
  gf_shutdown_func_t gf_shutdown_func = NULL;

  if (!module_load_gf(false, &gf_startup_func, &gf_shutdown_func)) {
      return 1;
  }

  gf_startup_func(argc, argv);

  qemu_init(argc, argv);
  int status = qemu_default_main();

  gf_shutdown_func();

  return status;
}

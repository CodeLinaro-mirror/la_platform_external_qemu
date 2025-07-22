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

#include <gmodule.h>
#include <cstdio>
#include <string>

extern "C" {
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "system/system.h"
}

 // Note this file is almost a duplicate of system/main.c
 // The main difference is that this will dynamically load the goldfish plugins
 // library and initialize the crashpad crash engine upon launch.
#include "android/crashreport/crash-initializer.h"

int qemu_default_main(void) {
  int status;

  status = qemu_main_loop();
  qemu_cleanup(status);

  return status;
}

int (*qemu_main)(void) = qemu_default_main;

#ifdef _WIN32
#define LIB_PREFIX ""
#else
#define LIB_PREFIX "lib"
#endif

#ifndef TARGET_NAME
#error CPU architecture not set
#endif

static bool module_load_gf(bool export_symbols)
{
    char *module_dir = getenv("QEMU_MODULE_DIR");
    if (module_dir == nullptr) {
        fprintf(stderr, "error, can't load goldfish module as QEMU_MODULE_DIR is not set");
        return false;
    }
    std::string full_path(module_dir);
    full_path += "/" LIB_PREFIX "goldfish_" TARGET_NAME;

    printf("loading %s\n", full_path.c_str());
    GModule *g_module;
    void (*register_types_func)(void);
    int flags;

    flags = 0;
    if (!export_symbols) {
        flags |= G_MODULE_BIND_LOCAL;
    }
    g_module = g_module_open(full_path.c_str(), (GModuleFlags)flags);
    if (!g_module) {
        fprintf(stderr, "error, failed to load goldfish module - %s\n", g_module_error());
        return false;
    }

    if (!g_module_symbol(g_module, "goldfish_register_types", (gpointer *)&register_types_func)) {
        fprintf(stderr, "error, failed to find goldfish_register_types function\n");
        g_module_close(g_module);
        return false;
    }

    register_module_init(register_types_func, MODULE_INIT_QOM);

    return true;
}

int main(int argc, char **argv) {
  if (!module_load_gf(true)) {
      exit(1);
  }

  // TODO(whollins): move these to the goldfish library.
  crashhandler_init(argc, argv);

  qemu_init(argc, argv);
  return qemu_main();
}

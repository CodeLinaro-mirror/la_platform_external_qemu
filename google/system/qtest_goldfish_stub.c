/*
 * Stub Goldfish plugin for QEMU qtest runs.
 *
 * Satisfies aemu_main.c's module_load_gf() contract without modifying
 * aemu_main.c, registers the runfiles pc-bios directory for ROM discovery,
 * and optionally chains to a higher-layer plugin specified via
 * QTEST_GOLDFISH_EXTRA_PLUGIN.
 */

#include "qemu/osdep.h"
#include <gmodule.h>
#include "qemu/datadir.h"
#include "aemu_func_defs.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

typedef void (*gf_register_types_func_t)(void);
typedef void (*gf_startup_func_t)(int argc, char **argv);
typedef void (*gf_shutdown_func_t)(void);

static GModule *extra_module;
static gf_register_types_func_t extra_register_types_func;
static gf_shutdown_func_t extra_shutdown_func;

G_MODULE_EXPORT void GF_REGISTER_TYPES_FUNC(void)
{
    if (extra_register_types_func) {
        extra_register_types_func();
    }
}

G_MODULE_EXPORT void GF_STARTUP_FUNC(int argc, char **argv)
{
    const char *extra_plugin;

    if (argc > 0 && argv[0] != NULL) {
        g_autofree char *exec_dir = g_path_get_dirname(argv[0]);
        qemu_add_data_dir(g_build_filename(exec_dir, "pc-bios", NULL));
    }

    extra_plugin = g_getenv("QTEST_GOLDFISH_EXTRA_PLUGIN");
    if (extra_plugin != NULL && extra_plugin[0] != '\0') {
        gf_startup_func_t extra_startup_func = NULL;

        extra_module = g_module_open(extra_plugin, G_MODULE_BIND_LOCAL);
        if (!extra_module) {
            fprintf(stderr,
                    "error: Failed to load extra qtest plugin from \"%s\": %s\n",
                    extra_plugin, g_module_error());
            exit(1);
        }

        g_module_symbol(extra_module, TOSTRING(GF_REGISTER_TYPES_FUNC),
                        (gpointer *)&extra_register_types_func);

        if (g_module_symbol(extra_module, TOSTRING(GF_STARTUP_FUNC),
                            (gpointer *)&extra_startup_func) &&
            extra_startup_func != NULL) {
            extra_startup_func(argc, argv);
        }

        g_module_symbol(extra_module, TOSTRING(GF_SHUTDOWN_FUNC),
                        (gpointer *)&extra_shutdown_func);
    }
}

G_MODULE_EXPORT void GF_SHUTDOWN_FUNC(void)
{
    if (extra_shutdown_func) {
        extra_shutdown_func();
    }
}

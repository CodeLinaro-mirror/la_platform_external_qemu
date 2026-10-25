// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Lazy-loading C trampolines for the rutabaga_gfx FFI.
//
// Why this exists
// ---------------
// librutabaga_ffi statically embeds a full copy of gfxstream (including the
// Objective-C classes in gfxstream's mac_native.m). The goldfish plugin's
// virtio-gpu-gfxstream device dlopen()s libgfxstream_backend, which is a
// second full copy of gfxstream, and the plugin's virtio_gpu_glue has to be
// able to talk to either device. If both images are present in one process
// the Objective-C runtime sees duplicate EmuGLContext / EmuGLView /
// EmuGLViewWithMetal classes, which it warns "may cause spurious casting
// failures and mysterious crashes".
//
// Linking qemu-system-* (and the goldfish plugin) against librutabaga_ffi at
// link time makes it load at process start, regardless of whether a
// virtio-gpu-rutabaga device is ever instantiated. This file replaces that
// link-time dependency with a lazy GModule load that happens on the first FFI
// call (i.e. from rutabaga_init() when the device is realized). When the
// launcher picks virtio-gpu-gfxstream instead, no rutabaga_* function is ever
// called, so librutabaga_ffi is never loaded and only one copy of gfxstream
// exists in the process.
//
// The library is looked up in $ANDROID_EMULATOR_LIBRARY_DIR (set by the
// launcher to <emulator>/lib/qemu). Only when that variable is unset is the
// bare name tried, so that DYLD_LIBRARY_PATH / LD_LIBRARY_PATH / rpath / PATH
// work when QEMU is launched directly (e.g. in tests) but a packaged emulator
// never silently picks up a foreign copy from the system search path.

#define RUTABAGA_GFX_FFI_UNSTABLE 1
#include "rutabaga_gfx/rutabaga_gfx_ffi.h"
#include "rutabaga_gfx/rutabaga_gfx_ffi_goldfish.h"

#include <errno.h>
#include <glib.h>
#include <gmodule.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define RUTABAGA_LAZY_LIBRARY_NAME "rutabaga_ffi.dll"
#define RUTABAGA_LAZY_VISIBILITY
#else
#define RUTABAGA_LAZY_VISIBILITY __attribute__((visibility("hidden")))
#if defined(__APPLE__)
#define RUTABAGA_LAZY_LIBRARY_NAME "librutabaga_ffi.dylib"
#else
#define RUTABAGA_LAZY_LIBRARY_NAME "librutabaga_ffi.so"
#endif
#endif

// X-macro listing every required core FFI entry point: (name, params, args)
#define RUTABAGA_LAZY_FOR_EACH_REQUIRED_FN(X)                                                     \
    X(rutabaga_calculate_capset_mask, (const char* capset_names, uint64_t* capset_mask),          \
      (capset_names, capset_mask))                                                                \
    X(rutabaga_init, (const struct rutabaga_builder* builder, struct rutabaga** ptr),             \
      (builder, ptr))                                                                             \
    X(rutabaga_finish, (struct rutabaga** ptr), (ptr))                                            \
    X(rutabaga_get_num_capsets, (struct rutabaga* ptr, uint32_t* num_capsets), (ptr, num_capsets)) \
    X(rutabaga_get_capset_info,                                                                   \
      (struct rutabaga* ptr, uint32_t capset_index, uint32_t* capset_id,                          \
       uint32_t* capset_version, uint32_t* capset_size),                                          \
      (ptr, capset_index, capset_id, capset_version, capset_size))                                \
    X(rutabaga_get_capset,                                                                        \
      (struct rutabaga* ptr, uint32_t capset_id, uint32_t version, uint8_t* capset,               \
       uint32_t capset_size),                                                                     \
      (ptr, capset_id, version, capset, capset_size))                                             \
    X(rutabaga_context_create,                                                                    \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t context_init, const char* context_name,    \
       uint32_t context_name_len),                                                                \
      (ptr, ctx_id, context_init, context_name, context_name_len))                                \
    X(rutabaga_context_destroy, (struct rutabaga* ptr, uint32_t ctx_id), (ptr, ctx_id))           \
    X(rutabaga_context_attach_resource,                                                           \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id), (ptr, ctx_id, resource_id))  \
    X(rutabaga_context_detach_resource,                                                           \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id), (ptr, ctx_id, resource_id))  \
    X(rutabaga_resource_create_3d,                                                                \
      (struct rutabaga* ptr, uint32_t resource_id, const struct rutabaga_create_3d* create_3d),   \
      (ptr, resource_id, create_3d))                                                              \
    X(rutabaga_resource_attach_backing,                                                           \
      (struct rutabaga* ptr, uint32_t resource_id, const struct rutabaga_iovecs* iovecs),         \
      (ptr, resource_id, iovecs))                                                                 \
    X(rutabaga_resource_detach_backing, (struct rutabaga* ptr, uint32_t resource_id),             \
      (ptr, resource_id))                                                                         \
    X(rutabaga_resource_transfer_read,                                                            \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id,                               \
       const struct rutabaga_transfer* transfer, const struct iovec* iovec),                      \
      (ptr, ctx_id, resource_id, transfer, iovec))                                                \
    X(rutabaga_resource_transfer_write,                                                           \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id,                               \
       const struct rutabaga_transfer* transfer),                                                 \
      (ptr, ctx_id, resource_id, transfer))                                                       \
    X(rutabaga_resource_create_blob,                                                              \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id,                               \
       const struct rutabaga_create_blob* create_blob, const struct rutabaga_iovecs* iovecs,      \
       const struct rutabaga_handle* handle),                                                     \
      (ptr, ctx_id, resource_id, create_blob, iovecs, handle))                                    \
    X(rutabaga_resource_unref, (struct rutabaga* ptr, uint32_t resource_id), (ptr, resource_id))  \
    X(rutabaga_resource_export_blob,                                                              \
      (struct rutabaga* ptr, uint32_t resource_id, struct rutabaga_handle* handle),               \
      (ptr, resource_id, handle))                                                                 \
    X(rutabaga_resource_map,                                                                      \
      (struct rutabaga* ptr, uint32_t resource_id, struct rutabaga_mapping* mapping),             \
      (ptr, resource_id, mapping))                                                                \
    X(rutabaga_resource_unmap, (struct rutabaga* ptr, uint32_t resource_id), (ptr, resource_id))  \
    X(rutabaga_resource_map_info,                                                                 \
      (struct rutabaga* ptr, uint32_t resource_id, uint32_t* map_info),                           \
      (ptr, resource_id, map_info))                                                               \
    X(rutabaga_submit_command, (struct rutabaga* ptr, struct rutabaga_command* cmd), (ptr, cmd))  \
    X(rutabaga_create_fence, (struct rutabaga* ptr, const struct rutabaga_fence* fence),          \
      (ptr, fence))                                                                               \
    X(rutabaga_snapshot, (struct rutabaga* ptr, const char* dir), (ptr, dir))                     \
    X(rutabaga_restore, (struct rutabaga* ptr, const char* dir), (ptr, dir))                      \
    X(rutabaga_resource_import,                                                                   \
      (struct rutabaga* ptr, uint32_t resource_id, const struct rutabaga_handle* import_handle,   \
       const struct rutabaga_import_data* import_data),                                           \
      (ptr, resource_id, import_handle, import_data))

// Goldfish-only extension (rutabaga_gfx_ffi_goldfish.h), used by the goldfish
// plugin's virtio_gpu_glue but never called by QEMU's virtio-gpu-rutabaga.c.
// Resolved non-fatally so an upstream librutabaga_ffi without --cfg=goldfish
// still initializes virtio-gpu-rutabaga cleanly.
#define RUTABAGA_LAZY_FOR_EACH_OPTIONAL_FN(X)                                                     \
    X(rutabaga_resource_transfer_write_goldfish,                                                  \
      (struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id,                               \
       const struct rutabaga_transfer* transfer, const struct iovec* iovec),                      \
      (ptr, ctx_id, resource_id, transfer, iovec))

#define RUTABAGA_LAZY_FOR_EACH_FN(X)      \
    RUTABAGA_LAZY_FOR_EACH_REQUIRED_FN(X) \
    RUTABAGA_LAZY_FOR_EACH_OPTIONAL_FN(X)

// Function-pointer table, one slot per entry point.
#define RUTABAGA_LAZY_DECLARE_SLOT(name, params, args) int32_t(*name) params;
static struct {
    RUTABAGA_LAZY_FOR_EACH_FN(RUTABAGA_LAZY_DECLARE_SLOT)
} s_fns;
#undef RUTABAGA_LAZY_DECLARE_SLOT

static GModule* s_module = NULL;
static int s_load_error = 0;  // positive errno once loading has been attempted and failed

static gpointer rutabaga_lazy_do_load(gpointer unused) {
    (void)unused;

    const char* lib_dir = g_getenv("ANDROID_EMULATOR_LIBRARY_DIR");
    const bool has_lib_dir = (lib_dir && *lib_dir);
    char* candidate = has_lib_dir ? g_build_filename(lib_dir, RUTABAGA_LAZY_LIBRARY_NAME, NULL)
                                  : g_strdup(RUTABAGA_LAZY_LIBRARY_NAME);
    const bool candidate_exists = has_lib_dir && g_file_test(candidate, G_FILE_TEST_EXISTS);

    // When ANDROID_EMULATOR_LIBRARY_DIR is set, only that packaged directory is
    // tried: if loading fails, report the broken packaged copy rather than
    // silently replacing it with whatever copy the system search path finds.
    // Only when ANDROID_EMULATOR_LIBRARY_DIR is unset/empty do we fall back to
    // the dynamic loader's search path (DYLD_LIBRARY_PATH, LD_LIBRARY_PATH,
    // rpath, PATH on Windows).
    s_module = g_module_open(candidate, G_MODULE_BIND_LOCAL);
    if (!s_module) {
        const char* reason = g_module_error();
        fprintf(stderr, "rutabaga: failed to load '%s' (ANDROID_EMULATOR_LIBRARY_DIR=%s): %s\n",
                candidate, has_lib_dir ? lib_dir : "<unset>",
                (reason && *reason) ? reason : "g_module_open failed");
        s_load_error = candidate_exists ? EIO : ENOENT;
        g_free(candidate);
        return NULL;
    }

#define RUTABAGA_LAZY_RESOLVE_REQUIRED_SLOT(name, params, args)                           \
    do {                                                                                  \
        gpointer sym = NULL;                                                              \
        if (!g_module_symbol(s_module, #name, &sym) || !sym) {                            \
            const char* reason = g_module_error();                                        \
            fprintf(stderr, "rutabaga: symbol " #name " missing from '%s': %s\n",         \
                    candidate, (reason && *reason) ? reason : "symbol not found");        \
            s_load_error = ENOSYS;                                                        \
        } else {                                                                          \
            memcpy(&s_fns.name, &sym, sizeof(s_fns.name));                                \
        }                                                                                 \
    } while (0);
    RUTABAGA_LAZY_FOR_EACH_REQUIRED_FN(RUTABAGA_LAZY_RESOLVE_REQUIRED_SLOT)
#undef RUTABAGA_LAZY_RESOLVE_REQUIRED_SLOT

#define RUTABAGA_LAZY_RESOLVE_OPTIONAL_SLOT(name, params, args)                           \
    do {                                                                                  \
        gpointer sym = NULL;                                                              \
        if (g_module_symbol(s_module, #name, &sym) && sym) {                              \
            memcpy(&s_fns.name, &sym, sizeof(s_fns.name));                                \
        }                                                                                 \
    } while (0);
    RUTABAGA_LAZY_FOR_EACH_OPTIONAL_FN(RUTABAGA_LAZY_RESOLVE_OPTIONAL_SLOT)
#undef RUTABAGA_LAZY_RESOLVE_OPTIONAL_SLOT

    g_free(candidate);
    return NULL;
}

static int rutabaga_lazy_ensure_loaded(void) {
    static GOnce s_once = G_ONCE_INIT;
    g_once(&s_once, rutabaga_lazy_do_load, NULL);
    return s_load_error;
}

// Trampolines. Every FFI function returns a negative errno on failure, so a
// missing or unloadable library surfaces to the caller as -ENOENT / -EIO /
// -ENOSYS from rutabaga_init() and the virtio-gpu-rutabaga device fails to
// realize cleanly. Hidden visibility on ELF/Mach-O prevents qemu-system-*
// (-rdynamic) from interposing rutabaga_* symbols into loaded plugins.
#define RUTABAGA_LAZY_DEFINE_TRAMPOLINE(name, params, args)       \
    RUTABAGA_LAZY_VISIBILITY int32_t name params {                \
        int err = rutabaga_lazy_ensure_loaded();                  \
        if (err) {                                                \
            return -err;                                          \
        }                                                         \
        if (!s_fns.name) {                                        \
            return -ENOSYS;                                       \
        }                                                         \
        return s_fns.name args;                                   \
    }
RUTABAGA_LAZY_FOR_EACH_FN(RUTABAGA_LAZY_DEFINE_TRAMPOLINE)
#undef RUTABAGA_LAZY_DEFINE_TRAMPOLINE

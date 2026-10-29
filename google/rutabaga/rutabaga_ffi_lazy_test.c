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

// Unit tests for rutabaga_ffi_lazy.c. Because GOnce caches the load outcome for
// the lifetime of the process, each scenario runs in its own cc_test binary:
//   - "fake":       loads a complete stand-in librutabaga_ffi from a non-ASCII
//                   UTF-8 directory and verifies FFI dispatch across slots.
//   - "missing":    ANDROID_EMULATOR_LIBRARY_DIR points to a nonexistent path;
//                   returns -ENOENT without falling back to system paths.
//   - "corrupt":    ANDROID_EMULATOR_LIBRARY_DIR points to a directory with a
//                   non-DSO file; returns -EIO (not -ENOENT).
//   - "incomplete":       stand-in DSO omits one required symbol; returns -ENOSYS.
//   - "optional_missing": stand-in DSO omits goldfish-only optional symbol;
//                         core FFI calls succeed and optional call returns -ENOSYS.

#define RUTABAGA_GFX_FFI_UNSTABLE 1
#include "rutabaga_gfx/rutabaga_gfx_ffi.h"
#include "rutabaga_gfx/rutabaga_gfx_ffi_goldfish.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#define TEST_LIB_NAME "librutabaga_ffi.dylib"
#else
#define TEST_LIB_NAME "librutabaga_ffi.so"
#endif

static char* make_utf8_test_dir(void) {
    const char* tmpdir = g_getenv("TEST_TMPDIR");
    g_assert_nonnull(tmpdir);
    char* dir = g_build_filename(tmpdir, "rutabaga_测试_🚀_é", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
    return dir;
}

static char* resolve_fake_dso_path(void) {
    const char* rlocation = g_getenv("RUTABAGA_FAKE_RLOCATION");
    const char* srcdir = g_getenv("TEST_SRCDIR");
    g_assert_nonnull(rlocation);
    g_assert_nonnull(srcdir);
    char* path = g_build_filename(srcdir, rlocation, NULL);
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    return path;
}

static void run_missing_test(void) {
    g_assert_true(g_setenv("ANDROID_EMULATOR_LIBRARY_DIR", "/nonexistent/测试_🚀_é", TRUE));
    uint64_t mask = 0;
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, -ENOENT);
    // Outcome is cached across subsequent calls.
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, -ENOENT);
}

static void run_corrupt_test(void) {
    char* dir = make_utf8_test_dir();
    char* bad_lib = g_build_filename(dir, TEST_LIB_NAME, NULL);
    GError* err = NULL;
    g_assert_true(g_file_set_contents(bad_lib, "not a shared library", -1, &err));
    g_assert_no_error(err);

    g_assert_true(g_setenv("ANDROID_EMULATOR_LIBRARY_DIR", dir, TRUE));
    uint64_t mask = 0;
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, -EIO);
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, -EIO);

    g_free(bad_lib);
    g_free(dir);
}

static void run_incomplete_test(void) {
    char* dso_path = resolve_fake_dso_path();
    char* dso_dir = g_path_get_dirname(dso_path);
    g_assert_true(g_setenv("ANDROID_EMULATOR_LIBRARY_DIR", dso_dir, TRUE));

    uint64_t mask = 0;
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, -ENOSYS);
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, -ENOSYS);

    g_free(dso_dir);
    g_free(dso_path);
}

static void run_optional_missing_test(void) {
    char* dso_path = resolve_fake_dso_path();
    char* dso_dir = g_path_get_dirname(dso_path);
    g_assert_true(g_setenv("ANDROID_EMULATOR_LIBRARY_DIR", dso_dir, TRUE));

    // Core rutabaga_gfx_ffi.h symbols still load and dispatch when the
    // goldfish-only extension symbol is absent.
    struct rutabaga* instance = NULL;
    struct rutabaga_builder builder;
    memset(&builder, 0, sizeof(builder));
    g_assert_cmpint(rutabaga_init(&builder, &instance), ==, 0);
    g_assert_nonnull(instance);

    // Calling the missing optional extension returns -ENOSYS without breaking
    // core entry points.
    g_assert_cmpint(
            rutabaga_resource_transfer_write_goldfish(instance, 1, 2, NULL, NULL), ==, -ENOSYS);
    g_assert_cmpint(rutabaga_finish(&instance), ==, 0);
    g_assert_null(instance);

    g_free(dso_dir);
    g_free(dso_path);
}

static void run_fake_test(void) {
    char* src_dso = resolve_fake_dso_path();
    char* utf8_dir = make_utf8_test_dir();
    char* dst_dso = g_build_filename(utf8_dir, TEST_LIB_NAME, NULL);

    char* contents = NULL;
    gsize length = 0;
    GError* err = NULL;
    g_assert_true(g_file_get_contents(src_dso, &contents, &length, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_set_contents(dst_dso, contents, (gssize)length, &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_chmod(dst_dso, 0755), ==, 0);
    g_free(contents);

    g_assert_true(g_setenv("ANDROID_EMULATOR_LIBRARY_DIR", utf8_dir, TRUE));

    uint64_t mask = 0;
    g_assert_cmpint(rutabaga_calculate_capset_mask("gfxstream-vulkan", &mask), ==, 0);
    g_assert_cmpuint(mask, ==, strlen("gfxstream-vulkan"));

    struct rutabaga* instance = NULL;
    struct rutabaga_builder builder;
    memset(&builder, 0, sizeof(builder));
    g_assert_cmpint(rutabaga_init(&builder, &instance), ==, 0);
    g_assert_nonnull(instance);

    uint32_t num_capsets = 0;
    g_assert_cmpint(rutabaga_get_num_capsets(instance, &num_capsets), ==, 0);
    g_assert_cmpuint(num_capsets, ==, 3);

    g_assert_cmpint(
            rutabaga_resource_transfer_write_goldfish(instance, 11, 31, NULL, NULL), ==, 42);

    g_assert_cmpint(rutabaga_finish(&instance), ==, 0);
    g_assert_null(instance);

    g_free(dst_dso);
    g_free(utf8_dir);
    g_free(src_dso);
}

int main(void) {
    const char* scenario = g_getenv("RUTABAGA_LAZY_TEST_SCENARIO");
    g_assert_nonnull(scenario);
    if (strcmp(scenario, "missing") == 0) {
        run_missing_test();
    } else if (strcmp(scenario, "corrupt") == 0) {
        run_corrupt_test();
    } else if (strcmp(scenario, "incomplete") == 0) {
        run_incomplete_test();
    } else if (strcmp(scenario, "optional_missing") == 0) {
        run_optional_missing_test();
    } else if (strcmp(scenario, "fake") == 0) {
        run_fake_test();
    } else {
        g_assert_not_reached();
    }
    return 0;
}

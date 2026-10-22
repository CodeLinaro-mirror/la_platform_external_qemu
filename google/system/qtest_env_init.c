/*
 * Prepares the environment of qtest binaries before libqtest spawns
 * qemu-system-<arch>: points TMPDIR at a private per-user directory and sets
 * ANDROID_EMULATOR_LIBRARY_DIR from QTEST_GOLDFISH_PLUGIN.
 *
 * The qtest targets are Linux/macOS only (see google/build/qtest.bzl).
 */

#include "qemu/osdep.h"

/* mkdtemp() fallback TMPDIR created by this process, removed at exit. */
static char *qtest_fallback_tmpdir;

static void qtest_remove_fallback_tmpdir(void)
{
    /* rmdir() fails harmlessly if a test left files behind. */
    rmdir(qtest_fallback_tmpdir);
}

/*
 * Returns true if `path` is a real directory (not a symlink) owned by the
 * effective user and inaccessible to group and others.
 */
static bool qtest_is_private_dir(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
}

/*
 * Returns a private directory to use as TMPDIR: "/tmp/qtest-<euid>" if it can
 * be created or safely reused, otherwise a fresh mkdtemp() directory that is
 * removed at exit. Returns NULL if neither is available.
 */
static char *qtest_private_tmpdir(void)
{
    unsigned int euid = (unsigned int)geteuid();
    g_autofree char *preferred = g_strdup_printf("/tmp/qtest-%u", euid);
    g_autofree char *fallback = NULL;

    if (g_mkdir_with_parents(preferred, 0700) == 0 &&
        qtest_is_private_dir(preferred)) {
        return g_steal_pointer(&preferred);
    }

    /*
     * Use an explicit template rather than g_dir_make_tmp(): it would call
     * g_get_tmp_dir(), which caches the current TMPDIR ("/tmp") for the rest
     * of the process, and libqtest derives its socket paths from it.
     */
    fallback = g_strdup("/tmp/qtest-XXXXXX");
    if (g_mkdtemp(fallback) == NULL) {
        int err = errno;

        fprintf(stderr,
                "warning: qtest: '%s' is not a private directory owned by uid "
                "%u and mkdtemp('/tmp/qtest-XXXXXX') failed: %s; TMPDIR is "
                "left unchanged. Remove '%s' or make /tmp writable.\n",
                preferred, euid, g_strerror(err), preferred);
        return NULL;
    }
    fprintf(stderr,
            "warning: qtest: '%s' is not a private directory owned by uid %u; "
            "using TMPDIR='%s' instead. Remove '%s' to reuse it.\n",
            preferred, euid, fallback, preferred);
    qtest_fallback_tmpdir = g_strdup(fallback);
    atexit(qtest_remove_fallback_tmpdir);
    return g_steal_pointer(&fallback);
}

static void __attribute__((constructor)) aemu_qtest_env_init(void)
{
    const char *tmpdir;
    const char *plugin_path;

    /*
     * Bazel sets TMPDIR=/tmp, and in block.c, get_tmp_filename() rewrites
     * "/tmp" to "/var/tmp", which is read-only inside Bazel's linux-sandbox.
     * A short private directory under /tmp avoids that rewrite, keeps UNIX
     * socket paths well under sun_path's 108-byte limit (Bazel's TEST_TMPDIR
     * is nested too deeply for that), and ensures test sockets never land in
     * a directory that other users can access.
     */
    tmpdir = g_getenv("TMPDIR");
    if (tmpdir == NULL || g_strcmp0(tmpdir, "/tmp") == 0) {
        g_autofree char *qtest_tmp = qtest_private_tmpdir();

        if (qtest_tmp != NULL) {
            g_setenv("TMPDIR", qtest_tmp, TRUE);
        }
    }

    plugin_path = g_getenv("QTEST_GOLDFISH_PLUGIN");
    if (plugin_path != NULL &&
        g_getenv("ANDROID_EMULATOR_LIBRARY_DIR") == NULL) {
        g_autofree char *plugin_dir = g_path_get_dirname(plugin_path);
        g_setenv("ANDROID_EMULATOR_LIBRARY_DIR", plugin_dir, FALSE);
    }
}

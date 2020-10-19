/* Code to mangle pathnames into those matching a given prefix.
   eg. open("/lib/foo.so") => open("/usr/gnemul/i386-linux/lib/foo.so");

   The assumption is that this area does not change.
*/
#include "qemu/osdep.h"
#include <sys/param.h>
#include <dirent.h>
#include <regex.h>
#include "qemu/cutils.h"
#include "qemu/path.h"
#include "qemu/thread.h"

static const char *base;
static GHashTable *hash;
static QemuMutex lock;
static regex_t shared_library_regex;

void init_paths(const char *prefix)
{
    int err;
    if (prefix[0] == '\0' || !strcmp(prefix, "/")) {
        return;
    }

    if (prefix[0] == '/') {
        base = g_strdup(prefix);
    } else {
        char *cwd = g_get_current_dir();
        base = g_build_filename(cwd, prefix, NULL);
        g_free(cwd);
    }

    hash = g_hash_table_new(g_str_hash, g_str_equal);
    qemu_mutex_init(&lock);

    err = regcomp(&shared_library_regex, "\\.so(\\.[[:digit:]]+)*$",
                  REG_EXTENDED | REG_NOSUB);
    if (err != 0) {
        fprintf(stderr, "regcomp returns error code %d\n", err);
        exit(EXIT_FAILURE);
    }
}

/*
 * Checks if a filename indicates that it's a shared object file by matching the
 * file name against /\.so(\.[0-9]+)*$/.
 */
static bool is_filename_shared_object(const char *name)
{
    return regexec(&shared_library_regex, name, 0, NULL, 0) == 0;
}

/*
 * Checks if a filename indicates that it may contain shared object files, and
 * so therefore should be wrapped by QEMU_LD_PREFIX.
 */
static bool is_filename_libdir(const char *name)
{
    return (
        g_str_has_prefix(name, "/lib")
        || g_str_has_prefix(name, "/usr/lib")
        || g_str_has_prefix(name, "/usr/local/lib")
        || g_str_has_prefix(name, "/lib64")
        || g_str_has_prefix(name, "/usr/lib64")
        || g_str_has_prefix(name, "/usr/local/lib64")
    );
}

/* Look for path in emulation dir, otherwise return name. */
const char *path(const char *name)
{
    gpointer key, value;
    const char *ret;

    /* Only do absolute paths: quick and dirty, but should mostly be OK.  */
    if (!base || !name || name[0] != '/') {
        return name;
    }

    qemu_mutex_lock(&lock);

    /* Have we looked up this file before?  */
    if (g_hash_table_lookup_extended(hash, name, &key, &value)) {
        ret = value ? value : name;
    } else if (is_filename_shared_object(name) || is_filename_libdir(name)) {
        /* GOOGLE CHANGE only prepend QEMU_LD_PREFIX for libraries */
        char *save = g_strdup(name);
        char *full = g_build_filename(base, name, NULL);

        /* Look for the path; record the result, pass or fail.  */
        if (access(full, F_OK) == 0) {
            /* Exists.  */
            g_hash_table_insert(hash, save, full);
            ret = full;
        } else {
            /* Does not exist.  */
            g_free(full);
            g_hash_table_insert(hash, save, NULL);
            ret = name;
        }
    } else {
        g_hash_table_insert(hash, g_strdup(name), NULL);
        ret = name;
    }

    qemu_mutex_unlock(&lock);
    return ret;
}

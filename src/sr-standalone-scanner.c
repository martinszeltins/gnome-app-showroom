/* sr-standalone-scanner.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-standalone"

#include "config.h"

#include <sys/stat.h>

#include "sr-appimage.h"
#include "sr-bundle-probe.h"
#include "sr-host.h"
#include "sr-standalone-scanner.h"
#include "sr-utils.h"

static GPtrArray *
list_sorted (const char *sandbox_dir)
{
    g_autoptr(GDir) dir = g_dir_open (sandbox_dir, 0, NULL);
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    const char *name;

    while (dir != NULL && (name = g_dir_read_name (dir)) != NULL) {
        if (name[0] != '.')
            g_ptr_array_add (names, g_strdup (name));
    }
    g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);

    return names;
}

static gboolean
add_unique (GPtrArray *apps, GHashTable *seen, const char *host_path, SrApp *app)
{
    g_autofree char *sandbox_path = sr_host_to_sandbox_path (host_path);
    g_autofree char *real = sr_realpath (sandbox_path);
    const char *key = real != NULL ? real : sandbox_path;

    if (app == NULL)
        return FALSE;

    if (g_hash_table_contains (seen, key)) {
        g_object_unref (app);
        return FALSE;
    }

    g_hash_table_add (seen, g_strdup (key));
    g_ptr_array_add (apps, app);
    return TRUE;
}

static void
scan_appimages_in (const char *host_dir, GPtrArray *apps, GHashTable *seen, GCancellable *cancellable)
{
    g_autofree char *sandbox_dir = sr_host_to_sandbox_path (host_dir);
    g_autoptr(GPtrArray) names = list_sorted (sandbox_dir);

    for (guint i = 0; i < names->len && !g_cancellable_is_cancelled (cancellable); i++) {
        g_autofree char *host_path = g_build_filename (host_dir, g_ptr_array_index (names, i), NULL);
        g_autofree char *sandbox_path = sr_host_to_sandbox_path (host_path);

        if (g_file_test (sandbox_path, G_FILE_TEST_IS_REGULAR) && sr_appimage_detect (sandbox_path, NULL))
            add_unique (apps, seen, host_path, sr_appimage_probe (host_path, cancellable));
    }
}

static void
scan_dir (const char *host_dir, SrDpkgIndex *dpkg, GPtrArray *apps, GHashTable *seen, GCancellable *cancellable)
{
    g_autofree char *sandbox_dir = sr_host_to_sandbox_path (host_dir);
    g_autoptr(GPtrArray) names = list_sorted (sandbox_dir);

    for (guint i = 0; i < names->len && !g_cancellable_is_cancelled (cancellable); i++) {
        g_autofree char *host_path = g_build_filename (host_dir, g_ptr_array_index (names, i), NULL);
        g_autofree char *sandbox_path = sr_host_to_sandbox_path (host_path);
        struct stat st;

        if (sr_dpkg_index_get_owner (dpkg, host_path) != NULL)
            continue;

        if (stat (sandbox_path, &st) != 0)
            continue;

        if (S_ISREG (st.st_mode)) {
            if (sr_appimage_detect (sandbox_path, NULL))
                add_unique (apps, seen, host_path, sr_appimage_probe (host_path, cancellable));
            else
                add_unique (apps, seen, host_path, sr_bundle_probe_binary (host_path));
        } else if (S_ISDIR (st.st_mode)) {
            SrApp *app = sr_bundle_probe_dir (host_path, cancellable);

            if (app != NULL)
                add_unique (apps, seen, host_path, app);
            else
                scan_appimages_in (host_path, apps, seen, cancellable);
        }
    }
}

GPtrArray *
sr_standalone_scanner_scan (const char *const *dirs, const char *const *appimage_only_dirs, SrDpkgIndex *dpkg,
                            GCancellable *cancellable)
{
    GPtrArray *apps = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; dirs != NULL && dirs[i] != NULL; i++)
        scan_dir (dirs[i], dpkg, apps, seen, cancellable);

    for (guint i = 0; appimage_only_dirs != NULL && appimage_only_dirs[i] != NULL; i++)
        scan_appimages_in (appimage_only_dirs[i], apps, seen, cancellable);

    return apps;
}

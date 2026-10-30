/* sr-catalog.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-catalog"

#include "config.h"

#include <string.h>

#include "sr-app-scanner.h"
#include "sr-catalog.h"
#include "sr-dpkg-index.h"
#include "sr-host.h"
#include "sr-package.h"
#include "sr-standalone-scanner.h"
#include "sr-utils.h"

typedef struct {
    GStrv standalone_dirs;
    GStrv appimage_only_dirs;
} LoadData;

static void
load_data_free (LoadData *data)
{
    g_strfreev (data->standalone_dirs);
    g_strfreev (data->appimage_only_dirs);
    g_free (data);
}

void
sr_catalog_free (SrCatalog *catalog)
{
    if (catalog == NULL)
        return;

    g_clear_pointer (&catalog->apps, g_ptr_array_unref);
    g_clear_pointer (&catalog->packages, g_ptr_array_unref);
    g_free (catalog);
}

static GStrv
expand_dirs (const char *const *dirs)
{
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();

    for (guint i = 0; dirs != NULL && dirs[i] != NULL; i++) {
        g_autofree char *expanded = sr_host_expand_home (dirs[i]);
        gsize len = strlen (expanded);

        while (len > 1 && expanded[len - 1] == '/')
            expanded[--len] = '\0';
        g_strv_builder_take (builder, g_steal_pointer (&expanded));
    }

    return g_strv_builder_end (builder);
}

/* Desktop entries that launch something inside a standalone folder (for
 * example an AppImageLauncher or Gear Lever integration) and the probed
 * standalone item describe the same app, so they become one card. */
static void
merge_standalone (GPtrArray *apps, GPtrArray *standalone)
{
    g_autoptr(GHashTable) by_root = g_hash_table_new (g_str_hash, g_str_equal);
    g_autoptr(GHashTable) consumed = g_hash_table_new (NULL, NULL);

    for (guint i = 0; i < standalone->len; i++) {
        SrApp *item = g_ptr_array_index (standalone, i);

        if (item->standalone_root != NULL)
            g_hash_table_insert (by_root, item->standalone_root, item);
    }

    for (guint i = 0; i < apps->len; i++) {
        SrApp *app = g_ptr_array_index (apps, i);
        SrApp *item;

        if (app->source != SR_SOURCE_STANDALONE || app->standalone_root == NULL)
            continue;

        item = g_hash_table_lookup (by_root, app->standalone_root);
        if (item == NULL)
            continue;

        g_hash_table_add (consumed, item);

        g_set_str (&app->kind, item->kind);
        g_set_str (&app->install_type, item->install_type);
        g_set_str (&app->location, item->location);
        if (app->version == NULL)
            app->version = g_strdup (item->version);
        if (app->icon == NULL && item->icon != NULL)
            app->icon = g_object_ref (item->icon);
        if (app->comment == NULL)
            app->comment = g_strdup (item->comment);

        for (guint j = 0; j < item->details->len; j++) {
            SrDetail *detail = g_ptr_array_index (item->details, j);
            sr_app_add_detail (app, detail->title, detail->value, detail->is_path);
        }
    }

    for (guint i = 0; i < standalone->len; i++) {
        SrApp *item = g_ptr_array_index (standalone, i);

        if (!g_hash_table_contains (consumed, item))
            g_ptr_array_add (apps, g_object_ref (item));
    }
}

static int
compare_apps (gconstpointer a, gconstpointer b)
{
    return sr_app_compare (*(SrApp **) a, *(SrApp **) b, NULL);
}

static void
load_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    LoadData *data = task_data;
    g_autoptr(SrDpkgIndex) dpkg = NULL;
    g_autoptr(GPtrArray) apps = NULL;
    g_autoptr(GPtrArray) standalone = NULL;
    g_autoptr(SrCatalog) catalog = g_new0 (SrCatalog, 1);
    GPtrArray *packages;

    dpkg = sr_dpkg_index_load (cancellable);
    if (g_task_return_error_if_cancelled (task))
        return;

    apps = sr_app_scanner_scan (dpkg, (const char *const *) data->standalone_dirs, cancellable);
    if (g_task_return_error_if_cancelled (task))
        return;

    standalone = sr_standalone_scanner_scan ((const char *const *) data->standalone_dirs,
                                             (const char *const *) data->appimage_only_dirs, dpkg, cancellable);
    if (g_task_return_error_if_cancelled (task))
        return;

    merge_standalone (apps, standalone);

    for (guint i = 0; i < apps->len; i++)
        sr_app_update_search_key (g_ptr_array_index (apps, i));
    g_ptr_array_sort (apps, compare_apps);

    sr_dpkg_index_finish_icons (dpkg);
    packages = sr_dpkg_index_get_packages (dpkg);
    for (guint i = 0; i < packages->len; i++)
        sr_package_update_search_key (g_ptr_array_index (packages, i));

    catalog->apps = g_steal_pointer (&apps);
    catalog->packages = g_ptr_array_ref (packages);

    g_task_return_pointer (task, g_steal_pointer (&catalog), (GDestroyNotify) sr_catalog_free);
}

void
sr_catalog_load_async (const char *const *standalone_dirs, const char *const *appimage_only_dirs,
                       GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new (NULL, cancellable, callback, user_data);
    LoadData *data = g_new0 (LoadData, 1);

    data->standalone_dirs = expand_dirs (standalone_dirs);
    data->appimage_only_dirs = expand_dirs (appimage_only_dirs);

    g_task_set_source_tag (task, sr_catalog_load_async);
    g_task_set_task_data (task, data, (GDestroyNotify) load_data_free);
    g_task_run_in_thread (task, load_thread);
}

SrCatalog *
sr_catalog_load_finish (GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer (G_TASK (result), error);
}

GPtrArray *
sr_catalog_get_watched_paths (const char *const *standalone_dirs, const char *const *appimage_only_dirs)
{
    g_auto(GStrv) dirs = sr_app_scanner_get_dir_paths ();
    g_auto(GStrv) standalone = expand_dirs (standalone_dirs);
    g_auto(GStrv) appimage_only = expand_dirs (appimage_only_dirs);
    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

    for (guint i = 0; dirs[i] != NULL; i++)
        g_ptr_array_add (paths, sr_host_to_sandbox_path (dirs[i]));

    for (guint i = 0; standalone[i] != NULL; i++)
        g_ptr_array_add (paths, sr_host_to_sandbox_path (standalone[i]));
    for (guint i = 0; appimage_only[i] != NULL; i++)
        g_ptr_array_add (paths, sr_host_to_sandbox_path (appimage_only[i]));

    g_ptr_array_add (paths, g_strdup ("/var/lib/dpkg/status"));

    return paths;
}

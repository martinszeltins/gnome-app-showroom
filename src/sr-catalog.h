/* sr-catalog.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
    /* SrApp, sorted by name */
    GPtrArray *apps;
    /* SrPackage, sorted by name */
    GPtrArray *packages;
} SrCatalog;

void sr_catalog_free (SrCatalog *catalog);

void sr_catalog_load_async (const char *const *standalone_dirs, const char *const *appimage_only_dirs,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);

SrCatalog *sr_catalog_load_finish (GAsyncResult *result, GError **error);

GPtrArray *sr_catalog_get_watched_paths (const char *const *standalone_dirs, const char *const *appimage_only_dirs);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SrCatalog, sr_catalog_free)

G_END_DECLS

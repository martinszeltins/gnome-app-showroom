/* sr-package.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SR_TYPE_PACKAGE (sr_package_get_type ())
G_DECLARE_FINAL_TYPE (SrPackage, sr_package, SR, PACKAGE, GObject)

struct _SrPackage {
    GObject parent_instance;

    char *name;
    char *arch;
    char *version;
    char *section;
    char *maintainer;
    char *homepage;
    char *summary;
    char *description;
    guint64 installed_size_kib;
    gboolean auto_installed;
    /* Sandbox path of /var/lib/dpkg/info/<name>.list */
    char *list_file;
    /* Host paths */
    char *location;
    GPtrArray *desktop_files;
    GPtrArray *app_names;
    GIcon *icon;
    char *icon_file;
    gboolean has_binaries;
    char *search_key;
};

SrPackage *sr_package_new (void);

void sr_package_update_search_key (SrPackage *self);

const char *sr_package_get_pill (SrPackage *self);

GPtrArray *sr_package_load_files (SrPackage *self, GCancellable *cancellable, GError **error);

G_END_DECLS

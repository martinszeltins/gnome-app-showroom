/* sr-package.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
    SR_PACKAGE_MANAGER_DEB,
    SR_PACKAGE_MANAGER_SNAP,
    SR_PACKAGE_MANAGER_FLATPAK,
    SR_PACKAGE_MANAGER_NODE,
    SR_PACKAGE_MANAGER_NPM,
    SR_PACKAGE_MANAGER_PNPM,
    SR_PACKAGE_MANAGER_YARN,
    SR_PACKAGE_MANAGER_BUN,
    SR_PACKAGE_MANAGER_DENO,
    SR_PACKAGE_MANAGER_RUST,
    SR_PACKAGE_MANAGER_CARGO,
    SR_PACKAGE_MANAGER_GO,
    SR_PACKAGE_MANAGER_PYTHON,
    SR_PACKAGE_MANAGER_COMPOSER,
    SR_PACKAGE_MANAGER_HOMEBREW,
    SR_PACKAGE_MANAGER_MANUAL,
} SrPackageManager;

/* What the source filter on the Installed Packages page offers */
typedef enum {
    SR_PACKAGE_GROUP_DEB,
    SR_PACKAGE_GROUP_SNAP,
    SR_PACKAGE_GROUP_FLATPAK,
    SR_PACKAGE_GROUP_NODE,
    SR_PACKAGE_GROUP_PYTHON,
    SR_PACKAGE_GROUP_RUST,
    SR_PACKAGE_GROUP_GO,
    SR_PACKAGE_GROUP_PHP,
    SR_PACKAGE_GROUP_HOMEBREW,
    SR_PACKAGE_GROUP_MANUAL,
    SR_N_PACKAGE_GROUPS,
} SrPackageGroup;

#define SR_TYPE_PACKAGE (sr_package_get_type ())
G_DECLARE_FINAL_TYPE (SrPackage, sr_package, SR, PACKAGE, GObject)

struct _SrPackage {
    GObject parent_instance;

    SrPackageManager manager;
    /* Shown on the pill instead of the manager name, e.g. "fnm" */
    char *manager_detail;
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
    gboolean is_default;
    /* Flatpak apps and app snaps, which the app categories may already show */
    gboolean is_app;
    /* Sandbox path of /var/lib/dpkg/info/<name>.list */
    char *list_file;
    /* Host paths */
    char *location;
    GPtrArray *desktop_files;
    GPtrArray *app_names;
    /* SrDetail */
    GPtrArray *details;
    GIcon *icon;
    char *icon_file;
    gboolean has_binaries;
    char *search_key;
};

SrPackage *sr_package_new (void);

SrPackage *sr_package_new_full (SrPackageManager manager, const char *manager_detail, const char *name,
                                const char *version, const char *summary, const char *location);

void sr_package_add_detail (SrPackage *self, const char *title, const char *value, gboolean is_path);

void sr_package_update_search_key (SrPackage *self);

const char *sr_package_get_pill (SrPackage *self);

const char *sr_package_get_css_class (SrPackage *self);

const char *sr_package_get_install_type (SrPackage *self);

SrPackageGroup sr_package_get_group (SrPackage *self);

SrPackageGroup sr_package_manager_get_group (SrPackageManager manager);

int sr_package_compare (gconstpointer a, gconstpointer b);

GPtrArray *sr_package_load_files (SrPackage *self, GCancellable *cancellable, GError **error);

const char *sr_package_group_get_label (SrPackageGroup group);

G_END_DECLS

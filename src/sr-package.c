/* sr-package.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "sr-app.h"
#include "sr-package.h"
#include "sr-utils.h"

G_DEFINE_FINAL_TYPE (SrPackage, sr_package, G_TYPE_OBJECT)

static void
sr_package_finalize (GObject *object)
{
    SrPackage *self = SR_PACKAGE (object);

    g_free (self->manager_detail);
    g_free (self->name);
    g_free (self->arch);
    g_free (self->version);
    g_free (self->section);
    g_free (self->maintainer);
    g_free (self->homepage);
    g_free (self->summary);
    g_free (self->description);
    g_free (self->list_file);
    g_free (self->location);
    g_ptr_array_unref (self->desktop_files);
    g_ptr_array_unref (self->app_names);
    g_ptr_array_unref (self->details);
    g_clear_object (&self->icon);
    g_free (self->icon_file);
    g_free (self->search_key);

    G_OBJECT_CLASS (sr_package_parent_class)->finalize (object);
}

static void
sr_package_class_init (SrPackageClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = sr_package_finalize;
}

static void
sr_package_init (SrPackage *self)
{
    self->manager = SR_PACKAGE_MANAGER_DEB;
    self->desktop_files = g_ptr_array_new_with_free_func (g_free);
    self->app_names = g_ptr_array_new_with_free_func (g_free);
    self->details = g_ptr_array_new_with_free_func ((GDestroyNotify) sr_detail_free);
}

SrPackage *
sr_package_new (void)
{
    return g_object_new (SR_TYPE_PACKAGE, NULL);
}

SrPackage *
sr_package_new_full (SrPackageManager manager, const char *manager_detail, const char *name, const char *version,
                     const char *summary, const char *location)
{
    SrPackage *self = sr_package_new ();

    self->manager = manager;
    self->manager_detail = g_strdup (manager_detail);
    self->name = g_strdup (name);
    self->version = g_strdup (version);
    self->summary = g_strdup (summary);
    self->location = g_strdup (location);

    return self;
}

void
sr_package_add_detail (SrPackage *self, const char *title, const char *value, gboolean is_path)
{
    sr_details_add (self->details, title, value, is_path);
}

void
sr_package_update_search_key (SrPackage *self)
{
    g_autofree char *apps = NULL;

    g_ptr_array_add (self->app_names, NULL);
    apps = g_strjoinv (" ", (char **) self->app_names->pdata);
    g_ptr_array_remove_index (self->app_names, self->app_names->len - 1);

    g_free (self->search_key);
    self->search_key = sr_build_search_key (self->name, self->summary ? self->summary : "", apps,
                                            sr_package_get_pill (self), NULL);
}

static const char *
manager_label (SrPackageManager manager)
{
    switch (manager) {
    case SR_PACKAGE_MANAGER_DEB:
        return _("Deb");
    case SR_PACKAGE_MANAGER_SNAP:
        return _("Snap");
    case SR_PACKAGE_MANAGER_FLATPAK:
        return _("Flatpak");
    case SR_PACKAGE_MANAGER_NODE:
        return _("Node.js");
    case SR_PACKAGE_MANAGER_NPM:
        return "npm";
    case SR_PACKAGE_MANAGER_PNPM:
        return "pnpm";
    case SR_PACKAGE_MANAGER_YARN:
        return "yarn";
    case SR_PACKAGE_MANAGER_BUN:
        return "Bun";
    case SR_PACKAGE_MANAGER_DENO:
        return "Deno";
    case SR_PACKAGE_MANAGER_RUST:
        return "rustup";
    case SR_PACKAGE_MANAGER_CARGO:
        return "cargo";
    case SR_PACKAGE_MANAGER_GO:
        return "Go";
    case SR_PACKAGE_MANAGER_PYTHON:
        return "pip";
    case SR_PACKAGE_MANAGER_COMPOSER:
        return "composer";
    case SR_PACKAGE_MANAGER_HOMEBREW:
        return "Homebrew";
    case SR_PACKAGE_MANAGER_MANUAL:
    default:
        return _("Manual");
    }
}

const char *
sr_package_get_pill (SrPackage *self)
{
    return self->manager_detail != NULL ? self->manager_detail : manager_label (self->manager);
}

SrPackageGroup
sr_package_get_group (SrPackage *self)
{
    return sr_package_manager_get_group (self->manager);
}

SrPackageGroup
sr_package_manager_get_group (SrPackageManager manager)
{
    switch (manager) {
    case SR_PACKAGE_MANAGER_DEB:
        return SR_PACKAGE_GROUP_DEB;
    case SR_PACKAGE_MANAGER_SNAP:
        return SR_PACKAGE_GROUP_SNAP;
    case SR_PACKAGE_MANAGER_FLATPAK:
        return SR_PACKAGE_GROUP_FLATPAK;
    case SR_PACKAGE_MANAGER_NODE:
    case SR_PACKAGE_MANAGER_NPM:
    case SR_PACKAGE_MANAGER_PNPM:
    case SR_PACKAGE_MANAGER_YARN:
    case SR_PACKAGE_MANAGER_BUN:
    case SR_PACKAGE_MANAGER_DENO:
        return SR_PACKAGE_GROUP_NODE;
    case SR_PACKAGE_MANAGER_RUST:
    case SR_PACKAGE_MANAGER_CARGO:
        return SR_PACKAGE_GROUP_RUST;
    case SR_PACKAGE_MANAGER_GO:
        return SR_PACKAGE_GROUP_GO;
    case SR_PACKAGE_MANAGER_PYTHON:
        return SR_PACKAGE_GROUP_PYTHON;
    case SR_PACKAGE_MANAGER_COMPOSER:
        return SR_PACKAGE_GROUP_PHP;
    case SR_PACKAGE_MANAGER_HOMEBREW:
        return SR_PACKAGE_GROUP_HOMEBREW;
    case SR_PACKAGE_MANAGER_MANUAL:
    default:
        return SR_PACKAGE_GROUP_MANUAL;
    }
}

const char *
sr_package_group_get_label (SrPackageGroup group)
{
    switch (group) {
    case SR_PACKAGE_GROUP_DEB:
        return _("Deb");
    case SR_PACKAGE_GROUP_SNAP:
        return _("Snap");
    case SR_PACKAGE_GROUP_FLATPAK:
        return _("Flatpak");
    case SR_PACKAGE_GROUP_NODE:
        return _("Node.js");
    case SR_PACKAGE_GROUP_PYTHON:
        return _("Python");
    case SR_PACKAGE_GROUP_RUST:
        return _("Rust");
    case SR_PACKAGE_GROUP_GO:
        return _("Go");
    case SR_PACKAGE_GROUP_PHP:
        return _("PHP");
    case SR_PACKAGE_GROUP_HOMEBREW:
        return _("Homebrew");
    case SR_PACKAGE_GROUP_MANUAL:
    case SR_N_PACKAGE_GROUPS:
    default:
        return _("Manual");
    }
}

const char *
sr_package_get_css_class (SrPackage *self)
{
    static const char *const classes[] = {
        [SR_PACKAGE_GROUP_DEB] = "pkg-deb",
        [SR_PACKAGE_GROUP_SNAP] = "pkg-snap",
        [SR_PACKAGE_GROUP_FLATPAK] = "pkg-flatpak",
        [SR_PACKAGE_GROUP_NODE] = "pkg-node",
        [SR_PACKAGE_GROUP_PYTHON] = "pkg-python",
        [SR_PACKAGE_GROUP_RUST] = "pkg-rust",
        [SR_PACKAGE_GROUP_GO] = "pkg-go",
        [SR_PACKAGE_GROUP_PHP] = "pkg-php",
        [SR_PACKAGE_GROUP_HOMEBREW] = "pkg-homebrew",
        [SR_PACKAGE_GROUP_MANUAL] = "pkg-manual",
    };

    return classes[sr_package_get_group (self)];
}

const char *
sr_package_get_install_type (SrPackage *self)
{
    switch (self->manager) {
    case SR_PACKAGE_MANAGER_DEB:
        return _("Debian package");
    case SR_PACKAGE_MANAGER_SNAP:
        return _("Snap");
    case SR_PACKAGE_MANAGER_FLATPAK:
        return self->is_app ? _("Flatpak app") : _("Flatpak runtime");
    case SR_PACKAGE_MANAGER_NODE:
        return _("Node.js version");
    case SR_PACKAGE_MANAGER_NPM:
        return _("npm global package");
    case SR_PACKAGE_MANAGER_PNPM:
        return _("pnpm");
    case SR_PACKAGE_MANAGER_YARN:
        return _("yarn global package");
    case SR_PACKAGE_MANAGER_BUN:
        return _("Bun");
    case SR_PACKAGE_MANAGER_DENO:
        return _("Deno");
    case SR_PACKAGE_MANAGER_RUST:
        return _("Rust toolchain");
    case SR_PACKAGE_MANAGER_CARGO:
        return _("Installed with cargo");
    case SR_PACKAGE_MANAGER_GO:
        return _("Go");
    case SR_PACKAGE_MANAGER_PYTHON:
        return _("Python package");
    case SR_PACKAGE_MANAGER_COMPOSER:
        return _("composer global package");
    case SR_PACKAGE_MANAGER_HOMEBREW:
        return _("Homebrew formula");
    case SR_PACKAGE_MANAGER_MANUAL:
    default:
        return _("Program you added by hand");
    }
}

int
sr_package_compare (gconstpointer a, gconstpointer b)
{
    SrPackage *pa = *(SrPackage **) a;
    SrPackage *pb = *(SrPackage **) b;
    int ret = g_ascii_strcasecmp (pa->name, pb->name);

    if (ret != 0)
        return ret;
    ret = (int) pa->manager - (int) pb->manager;
    if (ret != 0)
        return ret;
    return g_strcmp0 (pa->version, pb->version);
}

GPtrArray *
sr_package_load_files (SrPackage *self, GCancellable *cancellable, GError **error)
{
    g_autofree char *contents = NULL;
    g_autoptr(GPtrArray) files = NULL;
    g_auto(GStrv) lines = NULL;

    if (self->list_file == NULL)
        return g_ptr_array_new_with_free_func (g_free);

    if (!g_file_get_contents (self->list_file, &contents, NULL, error))
        return NULL;

    files = g_ptr_array_new_with_free_func (g_free);
    lines = g_strsplit (contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] != '/' || g_str_equal (lines[i], "/."))
            continue;
        g_ptr_array_add (files, g_strdup (lines[i]));
    }

    return g_steal_pointer (&files);
}

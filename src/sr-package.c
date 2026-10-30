/* sr-package.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "sr-package.h"
#include "sr-utils.h"

G_DEFINE_FINAL_TYPE (SrPackage, sr_package, G_TYPE_OBJECT)

static void
sr_package_finalize (GObject *object)
{
    SrPackage *self = SR_PACKAGE (object);

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
    self->desktop_files = g_ptr_array_new_with_free_func (g_free);
    self->app_names = g_ptr_array_new_with_free_func (g_free);
}

SrPackage *
sr_package_new (void)
{
    return g_object_new (SR_TYPE_PACKAGE, NULL);
}

void
sr_package_update_search_key (SrPackage *self)
{
    g_autofree char *apps = NULL;

    g_ptr_array_add (self->app_names, NULL);
    apps = g_strjoinv (" ", (char **) self->app_names->pdata);
    g_ptr_array_remove_index (self->app_names, self->app_names->len - 1);

    g_free (self->search_key);
    self->search_key = sr_build_search_key (self->name, self->summary ? self->summary : "", apps, NULL);
}

const char *
sr_package_get_pill (SrPackage *self)
{
    const char *slash;

    if (self->section == NULL)
        return NULL;

    /* "universe/libs" -> "libs" */
    slash = strrchr (self->section, '/');
    return slash != NULL ? slash + 1 : self->section;
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

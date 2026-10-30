/* sr-app.c
 *
 * Copyright 2026 Martins
 *
 * sr_app_compare() follows compare_rows() from gnome-control-center
 * (panels/applications/cc-applications-panel.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "sr-app.h"
#include "sr-utils.h"

G_DEFINE_FINAL_TYPE (SrApp, sr_app, G_TYPE_OBJECT)

static const char *sr_source_get_pill_label (SrSource source);

static void
sr_detail_free (SrDetail *detail)
{
    g_free (detail->title);
    g_free (detail->value);
    g_free (detail);
}

static void
sr_app_finalize (GObject *object)
{
    SrApp *self = SR_APP (object);

    g_free (self->id);
    g_free (self->name);
    g_free (self->comment);
    g_clear_object (&self->icon);
    g_free (self->kind);
    g_free (self->install_type);
    g_free (self->location);
    g_free (self->desktop_file);
    g_free (self->desktop_file_note);
    g_free (self->exec);
    g_free (self->categories);
    g_free (self->portal_app_id);
    g_free (self->package);
    g_free (self->version);
    g_free (self->standalone_root);
    g_ptr_array_unref (self->details);
    g_free (self->search_key);

    G_OBJECT_CLASS (sr_app_parent_class)->finalize (object);
}

static void
sr_app_class_init (SrAppClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = sr_app_finalize;
}

static void
sr_app_init (SrApp *self)
{
    self->source = SR_SOURCE_OTHER;
    self->details = g_ptr_array_new_with_free_func ((GDestroyNotify) sr_detail_free);
}

SrApp *
sr_app_new (void)
{
    return g_object_new (SR_TYPE_APP, NULL);
}

void
sr_app_add_detail (SrApp *self, const char *title, const char *value, gboolean is_path)
{
    SrDetail *detail;

    if (value == NULL || *value == '\0')
        return;

    detail = g_new0 (SrDetail, 1);
    detail->title = g_strdup (title);
    detail->value = g_strdup (value);
    detail->is_path = is_path;
    g_ptr_array_add (self->details, detail);
}

void
sr_app_update_search_key (SrApp *self)
{
    g_free (self->search_key);
    self->search_key = sr_build_search_key (self->name ? self->name : "", self->comment ? self->comment : "",
                                            self->id ? self->id : "", sr_app_get_pill (self), NULL);
}

const char *
sr_app_get_pill (SrApp *self)
{
    return self->kind != NULL ? self->kind : sr_source_get_pill_label (self->source);
}

int
sr_app_compare (gconstpointer a, gconstpointer b, gpointer user_data)
{
    SrApp *item1 = SR_APP ((gpointer) a);
    SrApp *item2 = SR_APP ((gpointer) b);
    g_autofree gchar *key1 = NULL;
    g_autofree gchar *key2 = NULL;

    key1 = g_utf8_casefold (item1->name, -1);
    key2 = g_utf8_casefold (item2->name, -1);

    return g_utf8_collate (key1, key2);
}

const char *
sr_source_get_id (SrSource source)
{
    switch (source) {
    case SR_SOURCE_FLATPAK:
        return "flatpak";
    case SR_SOURCE_DEB:
        return "deb";
    case SR_SOURCE_SNAP:
        return "snap";
    case SR_SOURCE_STANDALONE:
        return "standalone";
    case SR_SOURCE_OTHER:
    case SR_N_SOURCES:
    default:
        return "other";
    }
}

const char *
sr_source_get_label (SrSource source)
{
    switch (source) {
    case SR_SOURCE_FLATPAK:
        return _("Flatpak");
    case SR_SOURCE_DEB:
        return _("Deb");
    case SR_SOURCE_SNAP:
        return _("Snap");
    case SR_SOURCE_STANDALONE:
        return _("~/Applications");
    case SR_SOURCE_OTHER:
    case SR_N_SOURCES:
    default:
        return _("Launchers");
    }
}

static const char *
sr_source_get_pill_label (SrSource source)
{
    switch (source) {
    case SR_SOURCE_STANDALONE:
        return _("App Folder");
    case SR_SOURCE_OTHER:
        return _("Launcher");
    case SR_SOURCE_FLATPAK:
    case SR_SOURCE_DEB:
    case SR_SOURCE_SNAP:
    case SR_N_SOURCES:
    default:
        return sr_source_get_label (source);
    }
}

const char *
sr_source_get_description (SrSource source)
{
    switch (source) {
    case SR_SOURCE_FLATPAK:
        return _("Apps installed from Flatpak remotes such as Flathub");
    case SR_SOURCE_DEB:
        return _("Apps that come from Debian packages installed with apt");
    case SR_SOURCE_SNAP:
        return _("Apps installed from the Snap Store");
    case SR_SOURCE_STANDALONE:
        return _("AppImages, app folders and programs found in ~/Applications, ~/AppImages and /opt");
    case SR_SOURCE_OTHER:
    case SR_N_SOURCES:
    default:
        return _("Desktop entries that are not part of any package, such as web apps and custom shortcuts");
    }
}

const char *
sr_source_get_empty_title (SrSource source)
{
    switch (source) {
    case SR_SOURCE_FLATPAK:
        return _("No Flatpak Apps");
    case SR_SOURCE_DEB:
        return _("No Deb Apps");
    case SR_SOURCE_SNAP:
        return _("No Snap Apps");
    case SR_SOURCE_STANDALONE:
        return _("No Apps in ~/Applications");
    case SR_SOURCE_OTHER:
    case SR_N_SOURCES:
    default:
        return _("No Launchers");
    }
}

const char *
sr_source_get_icon_name (SrSource source)
{
    switch (source) {
    case SR_SOURCE_FLATPAK:
        return "sr-flatpak-symbolic";
    case SR_SOURCE_DEB:
        return "sr-debian-symbolic";
    case SR_SOURCE_SNAP:
        return "sr-snap-symbolic";
    case SR_SOURCE_STANDALONE:
        return "sr-standalone-symbolic";
    case SR_SOURCE_OTHER:
    case SR_N_SOURCES:
    default:
        return "preferences-desktop-apps-symbolic";
    }
}

const char *
sr_source_get_css_class (SrSource source)
{
    switch (source) {
    case SR_SOURCE_FLATPAK:
        return "source-flatpak";
    case SR_SOURCE_DEB:
        return "source-deb";
    case SR_SOURCE_SNAP:
        return "source-snap";
    case SR_SOURCE_STANDALONE:
        return "source-standalone";
    case SR_SOURCE_OTHER:
    case SR_N_SOURCES:
    default:
        return "source-other";
    }
}

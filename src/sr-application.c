/* sr-application.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "sr-app.h"
#include "sr-application.h"
#include "sr-catalog.h"
#include "sr-host.h"
#include "sr-package.h"
#include "sr-window.h"

struct _SrApplication {
    AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE (SrApplication, sr_application, ADW_TYPE_APPLICATION)

static void
about_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (user_data));
    AdwDialog *dialog;
    const char *developers[] = { "Martins", NULL };

    dialog = g_object_new (ADW_TYPE_ABOUT_DIALOG,
                           "application-name", _("App Showroom"),
                           "application-icon", APPLICATION_ID,
                           "developer-name", "Martins",
                           "version", PACKAGE_VERSION,
                           "developers", developers,
                           "license-type", GTK_LICENSE_GPL_3_0,
                           "comments", _("Browse installed apps and packages by where they came from"),
                           NULL);
    adw_about_dialog_add_credit_section (ADW_ABOUT_DIALOG (dialog), _("Based on the Apps panel of"),
                                         (const char *[]) { "GNOME Settings https://gitlab.gnome.org/GNOME/gnome-control-center", NULL });
    adw_dialog_present (dialog, GTK_WIDGET (window));
}

static void
shortcuts_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (user_data));
    AdwShortcutsDialog *dialog = ADW_SHORTCUTS_DIALOG (adw_shortcuts_dialog_new ());
    AdwShortcutsSection *section = adw_shortcuts_section_new (NULL);

    adw_shortcuts_section_add (section, adw_shortcuts_item_new (_("Search"), "<Control>f"));
    adw_shortcuts_section_add (section, adw_shortcuts_item_new (_("Reload"), "<Control>r"));
    adw_shortcuts_section_add (section, adw_shortcuts_item_new (_("Keyboard Shortcuts"), "<Control>question"));
    adw_shortcuts_section_add (section, adw_shortcuts_item_new (_("Quit"), "<Control>q"));
    adw_shortcuts_dialog_add (dialog, section);

    adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (window));
}

static void
quit_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    g_application_quit (G_APPLICATION (user_data));
}

static const GActionEntry app_actions[] = {
    { "about", about_action },
    { "shortcuts", shortcuts_action },
    { "quit", quit_action },
};

static void
sr_application_startup (GApplication *application)
{
    G_APPLICATION_CLASS (sr_application_parent_class)->startup (application);

    g_action_map_add_action_entries (G_ACTION_MAP (application), app_actions, G_N_ELEMENTS (app_actions),
                                     application);
    gtk_application_set_accels_for_action (GTK_APPLICATION (application), "app.quit",
                                           (const char *[]) { "<Control>q", NULL });
    gtk_application_set_accels_for_action (GTK_APPLICATION (application), "app.shortcuts",
                                           (const char *[]) { "<Control>question", NULL });

    sr_host_setup_icon_theme (gdk_display_get_default ());
}

static void
sr_application_activate (GApplication *application)
{
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (application));

    if (window == NULL)
        window = GTK_WINDOW (sr_window_new (ADW_APPLICATION (application)));

    gtk_window_present (window);
}

typedef struct {
    GMainLoop *loop;
    SrCatalog *catalog;
} ListData;

static void
list_loaded_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
    ListData *data = user_data;
    g_autoptr(GError) error = NULL;

    data->catalog = sr_catalog_load_finish (result, &error);
    if (data->catalog == NULL)
        g_printerr ("%s\n", error->message);
    g_main_loop_quit (data->loop);
}

static void
print_catalog (SrCatalog *catalog, gboolean with_packages)
{
    for (guint source = 0; source < SR_N_SOURCES; source++) {
        guint count = 0;

        for (guint i = 0; i < catalog->apps->len; i++)
            count += SR_APP (g_ptr_array_index (catalog->apps, i))->source == source;

        g_print ("== %s (%u)\n", sr_source_get_label (source), count);
        for (guint i = 0; i < catalog->apps->len; i++) {
            SrApp *app = g_ptr_array_index (catalog->apps, i);
            g_autofree char *icon = app->icon != NULL ? g_icon_to_string (app->icon) : NULL;

            if (app->source != source)
                continue;

            g_print ("  %-36s [%s] %s\n      location: %s\n      desktop:  %s\n      icon:     %s\n", app->name,
                     sr_app_get_pill (app), app->install_type ? app->install_type : "",
                     app->location ? app->location : "-", app->desktop_file ? app->desktop_file : "-",
                     icon ? icon : "-");
        }
    }

    g_print ("== %s (%u)\n", _("Installed Packages"), catalog->packages->len);
    for (guint i = 0; with_packages && i < catalog->packages->len; i++) {
        SrPackage *package = g_ptr_array_index (catalog->packages, i);
        g_autofree char *icon = package->icon != NULL ? g_icon_to_string (package->icon) : NULL;

        g_print ("  %-14s %-40s %-28s %s%s  %s\n", sr_package_get_pill (package), package->name,
                 package->version ? package->version : "", package->is_default ? "(default) " : "",
                 package->location ? package->location : "-", icon ? icon : "-");
    }
}

static int
sr_application_handle_local_options (GApplication *application, GVariantDict *options)
{
    gboolean list = FALSE;
    gboolean list_packages = FALSE;
    g_autoptr(GSettings) settings = NULL;
    g_auto(GStrv) standalone_dirs = NULL;
    g_auto(GStrv) appimage_only_dirs = NULL;
    ListData data = { 0 };

    g_variant_dict_lookup (options, "list", "b", &list);
    g_variant_dict_lookup (options, "list-packages", "b", &list_packages);
    if (!list && !list_packages)
        return -1;

    settings = g_settings_new (APPLICATION_ID);
    standalone_dirs = g_settings_get_strv (settings, "standalone-dirs");
    appimage_only_dirs = g_settings_get_strv (settings, "appimage-only-dirs");

    data.loop = g_main_loop_new (NULL, FALSE);
    sr_catalog_load_async ((const char *const *) standalone_dirs, (const char *const *) appimage_only_dirs, NULL,
                           list_loaded_cb, &data);
    g_main_loop_run (data.loop);
    g_main_loop_unref (data.loop);

    if (data.catalog == NULL)
        return 1;

    print_catalog (data.catalog, list_packages);
    sr_catalog_free (data.catalog);

    return 0;
}

static void
sr_application_class_init (SrApplicationClass *klass)
{
    GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

    app_class->startup = sr_application_startup;
    app_class->activate = sr_application_activate;
    app_class->handle_local_options = sr_application_handle_local_options;
}

static void
sr_application_init (SrApplication *self)
{
    const GOptionEntry entries[] = {
        { "list", 'l', 0, G_OPTION_ARG_NONE, NULL, N_("Print installed apps grouped by source and exit"), NULL },
        { "list-packages", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Also print every installed package and exit"), NULL },
        { NULL },
    };

    g_application_add_main_option_entries (G_APPLICATION (self), entries);
}

SrApplication *
sr_application_new (void)
{
    return g_object_new (SR_TYPE_APPLICATION, "application-id", APPLICATION_ID, "flags",
                         G_APPLICATION_DEFAULT_FLAGS, "resource-base-path", RESOURCE_BASE, NULL);
}

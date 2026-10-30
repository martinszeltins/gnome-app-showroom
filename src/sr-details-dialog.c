/* sr-details-dialog.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-details-dialog"

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "sr-app-card.h"
#include "sr-details-dialog.h"
#include "sr-host.h"

struct _SrDetailsDialog {
    AdwDialog parent_instance;

    AdwToastOverlay *toast_overlay;
    GtkImage *icon;
    GtkLabel *name_label;
    GtkLabel *comment_label;
    GtkLabel *pill;
    GtkButton *open_location_button;
    GtkButton *open_desktop_button;
    AdwPreferencesGroup *description_group;
    GtkLabel *description_label;
    AdwPreferencesGroup *info_group;
    AdwPreferencesGroup *files_group;
    AdwExpanderRow *files_row;

    char *location;
    char *desktop_file;
    GPtrArray *files;
    gboolean files_shown;
};

G_DEFINE_FINAL_TYPE (SrDetailsDialog, sr_details_dialog, ADW_TYPE_DIALOG)

static void
copy_clicked_cb (GtkButton *button, SrDetailsDialog *self)
{
    const char *value = g_object_get_data (G_OBJECT (button), "value");

    gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), value);
    adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (_("Copied to clipboard")));
}

static void
show_clicked_cb (GtkButton *button, SrDetailsDialog *self)
{
    sr_host_show_location (g_object_get_data (G_OBJECT (button), "value"));
}

static GtkWidget *
suffix_button (SrDetailsDialog *self, const char *icon_name, const char *tooltip, const char *value, GCallback cb)
{
    GtkWidget *button = gtk_button_new_from_icon_name (icon_name);

    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (button, tooltip);
    gtk_widget_add_css_class (button, "flat");
    gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
    g_object_set_data_full (G_OBJECT (button), "value", g_strdup (value), g_free);
    g_signal_connect_object (button, "clicked", cb, self, G_CONNECT_DEFAULT);

    return button;
}

static void
add_row_full (SrDetailsDialog *self, const char *title, const char *value, gboolean is_path, gboolean copyable)
{
    GtkWidget *row;

    if (value == NULL || *value == '\0')
        return;

    row = adw_action_row_new ();
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
    adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
    gtk_widget_add_css_class (row, "property");

    if (is_path && sr_host_path_exists (value))
        adw_action_row_add_suffix (ADW_ACTION_ROW (row), suffix_button (self, "folder-open-symbolic",
                                                                        _("Show in Files"), value,
                                                                        G_CALLBACK (show_clicked_cb)));
    if (copyable)
        adw_action_row_add_suffix (ADW_ACTION_ROW (row), suffix_button (self, "edit-copy-symbolic", _("Copy"), value,
                                                                        G_CALLBACK (copy_clicked_cb)));

    adw_preferences_group_add (self->info_group, row);
}

static void
add_row (SrDetailsDialog *self, const char *title, const char *value, gboolean is_path)
{
    add_row_full (self, title, value, is_path, TRUE);
}

static void
update_buttons (SrDetailsDialog *self)
{
    gtk_widget_set_visible (GTK_WIDGET (self->open_location_button), self->location != NULL);
    gtk_widget_set_visible (GTK_WIDGET (self->open_desktop_button), self->desktop_file != NULL);
}

static void
on_open_location_clicked (SrDetailsDialog *self)
{
    sr_host_show_location (self->location);
}

static void
on_open_desktop_clicked (SrDetailsDialog *self)
{
    sr_host_open_file (self->desktop_file);
}

static void
file_setup_cb (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    GtkWidget *label = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_list_item_set_child (item, label);
}

static void
file_bind_cb (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    GtkStringObject *string = gtk_list_item_get_item (item);
    GtkWidget *label = gtk_list_item_get_child (item);

    gtk_label_set_label (GTK_LABEL (label), gtk_string_object_get_string (string));
    gtk_widget_set_tooltip_text (label, gtk_string_object_get_string (string));
}

static void
file_activated_cb (GtkListView *view, guint position, SrDetailsDialog *self)
{
    GListModel *model = G_LIST_MODEL (gtk_list_view_get_model (view));
    g_autoptr(GtkStringObject) string = g_list_model_get_item (model, position);

    if (string != NULL)
        sr_host_show_location (gtk_string_object_get_string (string));
}

static void
on_files_expanded (SrDetailsDialog *self)
{
    g_autoptr(GtkStringList) strings = NULL;
    GtkListItemFactory *factory;
    GtkNoSelection *selection;
    GtkWidget *scrolled, *view;

    if (self->files_shown || self->files == NULL || !adw_expander_row_get_expanded (self->files_row))
        return;

    self->files_shown = TRUE;

    strings = gtk_string_list_new (NULL);
    for (guint i = 0; i < self->files->len; i++)
        gtk_string_list_append (strings, g_ptr_array_index (self->files, i));

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (file_setup_cb), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (file_bind_cb), NULL);

    selection = gtk_no_selection_new (G_LIST_MODEL (g_steal_pointer (&strings)));
    view = gtk_list_view_new (GTK_SELECTION_MODEL (selection), factory);
    gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (view), TRUE);
    gtk_widget_add_css_class (view, "file-list");
    g_signal_connect_object (view, "activate", G_CALLBACK (file_activated_cb), self, G_CONNECT_DEFAULT);

    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scrolled), MIN (self->files->len, 12) * 28);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 340);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), view);

    adw_expander_row_add_row (self->files_row, scrolled);
}

static void
sr_details_dialog_finalize (GObject *object)
{
    SrDetailsDialog *self = SR_DETAILS_DIALOG (object);

    g_free (self->location);
    g_free (self->desktop_file);
    g_clear_pointer (&self->files, g_ptr_array_unref);

    G_OBJECT_CLASS (sr_details_dialog_parent_class)->finalize (object);
}

static void
sr_details_dialog_class_init (SrDetailsDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->finalize = sr_details_dialog_finalize;

    gtk_widget_class_set_template_from_resource (widget_class, RESOURCE_BASE "/sr-details-dialog.ui");
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, toast_overlay);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, icon);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, name_label);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, comment_label);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, pill);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, open_location_button);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, open_desktop_button);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, description_group);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, description_label);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, info_group);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, files_group);
    gtk_widget_class_bind_template_child (widget_class, SrDetailsDialog, files_row);

    gtk_widget_class_bind_template_callback (widget_class, on_open_location_clicked);
    gtk_widget_class_bind_template_callback (widget_class, on_open_desktop_clicked);
    gtk_widget_class_bind_template_callback (widget_class, on_files_expanded);
}

static void
sr_details_dialog_init (SrDetailsDialog *self)
{
    gtk_widget_init_template (GTK_WIDGET (self));
}

static void
set_header (SrDetailsDialog *self, GIcon *icon, const char *fallback, const char *name, const char *comment)
{
    sr_image_set_icon (self->icon, icon, fallback);
    gtk_label_set_label (self->name_label, name);
    gtk_label_set_label (self->comment_label, comment);
    gtk_widget_set_visible (GTK_WIDGET (self->comment_label), comment != NULL && *comment != '\0');
    adw_dialog_set_title (ADW_DIALOG (self), name);
}

SrDetailsDialog *
sr_details_dialog_new_for_app (SrApp *app)
{
    SrDetailsDialog *self = g_object_new (SR_TYPE_DETAILS_DIALOG, NULL);
    g_autofree char *plain_id = NULL;

    set_header (self, app->icon, "application-x-executable", app->name, app->comment);
    sr_pill_set (self->pill, sr_app_get_pill (app), sr_source_get_css_class (app->source));

    self->location = g_strdup (app->location);
    self->desktop_file = g_strdup (app->desktop_file);
    update_buttons (self);

    add_row (self, _("Installation Type"), app->install_type, FALSE);
    add_row (self, _("Location"), app->location, TRUE);
    add_row (self, _("Desktop File"), app->desktop_file, TRUE);
    add_row_full (self, _("About the Desktop File"), app->desktop_file_note, FALSE, FALSE);
    add_row (self, _("Version"), app->version, FALSE);

    if (app->desktop_file != NULL && g_str_has_suffix (app->id, ".desktop")) {
        add_row (self, _("App ID"), app->id, FALSE);
        plain_id = g_strndup (app->id, strlen (app->id) - strlen (".desktop"));
    }
    if (app->portal_app_id != NULL && g_strcmp0 (app->portal_app_id, plain_id) != 0)
        add_row (self, _("Sandbox App ID"), app->portal_app_id, FALSE);

    add_row (self, _("Categories"), app->categories, FALSE);

    for (guint i = 0; i < app->details->len; i++) {
        SrDetail *detail = g_ptr_array_index (app->details, i);
        add_row (self, detail->title, detail->value, detail->is_path);
    }

    return self;
}

SrDetailsDialog *
sr_details_dialog_new_for_package (SrPackage *package)
{
    SrDetailsDialog *self = g_object_new (SR_TYPE_DETAILS_DIALOG, NULL);
    g_autoptr(GError) error = NULL;
    g_autofree char *size = NULL;
    g_autofree char *apps = NULL;
    g_autofree char *files_subtitle = NULL;

    set_header (self, package->icon, "package-x-generic", package->name, package->summary);
    sr_pill_set (self->pill, sr_package_get_pill (package), "source-package");

    if (package->description != NULL) {
        gtk_label_set_label (self->description_label, package->description);
        gtk_widget_set_visible (GTK_WIDGET (self->description_group), TRUE);
    }

    self->location = g_strdup (package->location);
    if (package->desktop_files->len > 0)
        self->desktop_file = g_strdup (g_ptr_array_index (package->desktop_files, 0));
    update_buttons (self);

    add_row (self, _("Installation Type"), _("Debian package"), FALSE);
    add_row (self, _("Package"), package->name, FALSE);
    add_row (self, _("Version"), package->version, FALSE);
    add_row (self, _("Architecture"), package->arch, FALSE);
    add_row (self, _("Section"), package->section, FALSE);
    if (package->installed_size_kib > 0) {
        size = g_format_size (package->installed_size_kib * 1024);
        add_row (self, _("Installed Size"), size, FALSE);
    }
    add_row_full (self, _("Installed"),
                  package->auto_installed ? _("Automatically, as a dependency") : _("Manually"), FALSE, FALSE);
    add_row (self, _("Location"), package->location, TRUE);

    if (package->app_names->len > 0) {
        g_ptr_array_add (package->app_names, NULL);
        apps = g_strjoinv (", ", (char **) package->app_names->pdata);
        g_ptr_array_remove_index (package->app_names, package->app_names->len - 1);
        add_row (self, _("Provides Apps"), apps, FALSE);
    }
    for (guint i = 0; i < package->desktop_files->len; i++)
        add_row (self, _("Desktop File"), g_ptr_array_index (package->desktop_files, i), TRUE);

    add_row (self, _("Maintainer"), package->maintainer, FALSE);
    add_row (self, _("Homepage"), package->homepage, FALSE);
    add_row (self, _("Package File List"), package->list_file, TRUE);

    self->files = sr_package_load_files (package, NULL, &error);
    if (self->files == NULL) {
        g_debug ("Could not read file list of %s: %s", package->name, error->message);
    } else if (self->files->len > 0) {
        files_subtitle = g_strdup_printf (ngettext ("%u file or folder", "%u files and folders", self->files->len),
                                          self->files->len);
        adw_expander_row_set_subtitle (self->files_row, files_subtitle);
        gtk_widget_set_visible (GTK_WIDGET (self->files_group), TRUE);
    }

    return self;
}

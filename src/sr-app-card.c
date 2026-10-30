/* sr-app-card.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "sr-app-card.h"

struct _SrAppCard {
    GtkBox parent_instance;

    GtkImage *icon;
    GtkLabel *title;
    GtkLabel *pill;
    GtkLabel *subtitle;
};

G_DEFINE_FINAL_TYPE (SrAppCard, sr_app_card, GTK_TYPE_BOX)

static const char *const pill_classes[] = {
    "source-flatpak", "source-deb", "source-snap", "source-standalone", "source-other",
    "pkg-deb", "pkg-snap", "pkg-flatpak", "pkg-node", "pkg-python", "pkg-rust", "pkg-go", "pkg-homebrew", "pkg-manual",
};

static void
sr_app_card_class_init (SrAppCardClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    gtk_widget_class_set_template_from_resource (widget_class, RESOURCE_BASE "/sr-app-card.ui");
    gtk_widget_class_bind_template_child (widget_class, SrAppCard, icon);
    gtk_widget_class_bind_template_child (widget_class, SrAppCard, title);
    gtk_widget_class_bind_template_child (widget_class, SrAppCard, pill);
    gtk_widget_class_bind_template_child (widget_class, SrAppCard, subtitle);
}

static void
sr_app_card_init (SrAppCard *self)
{
    gtk_widget_init_template (GTK_WIDGET (self));
}

GtkWidget *
sr_app_card_new (void)
{
    return g_object_new (SR_TYPE_APP_CARD, NULL);
}

void
sr_image_set_icon (GtkImage *image, GIcon *icon, const char *fallback)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (image)));

    if (icon != NULL && (!G_IS_THEMED_ICON (icon) || gtk_icon_theme_has_gicon (theme, icon)))
        gtk_image_set_from_gicon (image, icon);
    else
        gtk_image_set_from_icon_name (image, fallback);
}

void
sr_pill_set (GtkLabel *pill, const char *text, const char *css_class)
{
    for (guint i = 0; i < G_N_ELEMENTS (pill_classes); i++)
        gtk_widget_remove_css_class (GTK_WIDGET (pill), pill_classes[i]);

    gtk_label_set_label (pill, text);
    gtk_widget_set_visible (GTK_WIDGET (pill), text != NULL && *text != '\0');
    if (css_class != NULL)
        gtk_widget_add_css_class (GTK_WIDGET (pill), css_class);
}

void
sr_app_card_set_app (SrAppCard *self, SrApp *app)
{
    g_autofree char *tooltip = NULL;

    sr_image_set_icon (self->icon, app->icon, "application-x-executable");
    gtk_label_set_label (self->title, app->name);
    gtk_label_set_label (self->subtitle, app->comment != NULL ? app->comment : "");
    gtk_widget_set_visible (GTK_WIDGET (self->subtitle), app->comment != NULL);
    sr_pill_set (self->pill, sr_app_get_pill (app), sr_source_get_css_class (app->source));

    tooltip = app->comment != NULL ? g_strdup_printf ("%s\n%s", app->name, app->comment) : g_strdup (app->name);
    gtk_widget_set_tooltip_text (GTK_WIDGET (self), tooltip);
    gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, app->name,
                                    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, app->comment ? app->comment : "", -1);
}

void
sr_app_card_set_package (SrAppCard *self, SrPackage *package)
{
    g_autofree char *subtitle = NULL;

    if (package->manager != SR_PACKAGE_MANAGER_DEB && package->version != NULL)
        subtitle = package->summary != NULL ? g_strdup_printf ("%s · %s", package->version, package->summary)
                                            : g_strdup (package->version);
    else
        subtitle = g_strdup (package->summary);

    sr_image_set_icon (self->icon, package->icon, "package-x-generic");
    gtk_label_set_label (self->title, package->name);
    gtk_label_set_label (self->subtitle, subtitle != NULL ? subtitle : "");
    gtk_widget_set_visible (GTK_WIDGET (self->subtitle), subtitle != NULL);
    sr_pill_set (self->pill, sr_package_get_pill (package), sr_package_get_css_class (package));

    gtk_widget_set_tooltip_text (GTK_WIDGET (self), package->summary);
    gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, package->name,
                                    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, package->summary ? package->summary : "",
                                    -1);
}

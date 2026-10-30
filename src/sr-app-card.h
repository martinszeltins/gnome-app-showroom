/* sr-app-card.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "sr-app.h"
#include "sr-package.h"

G_BEGIN_DECLS

#define SR_TYPE_APP_CARD (sr_app_card_get_type ())
G_DECLARE_FINAL_TYPE (SrAppCard, sr_app_card, SR, APP_CARD, GtkBox)

GtkWidget *sr_app_card_new (void);

void sr_app_card_set_app (SrAppCard *self, SrApp *app);

void sr_app_card_set_package (SrAppCard *self, SrPackage *package);

void sr_image_set_icon (GtkImage *image, GIcon *icon, const char *fallback);

void sr_pill_set (GtkLabel *pill, const char *text, const char *css_class);

G_END_DECLS

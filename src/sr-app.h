/* sr-app.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
    SR_SOURCE_FLATPAK,
    SR_SOURCE_DEB,
    SR_SOURCE_SNAP,
    SR_SOURCE_STANDALONE,
    SR_SOURCE_OTHER,
    SR_N_SOURCES,
} SrSource;

typedef struct {
    char *title;
    char *value;
    gboolean is_path;
} SrDetail;

void sr_detail_free (SrDetail *detail);

void sr_details_add (GPtrArray *details, const char *title, const char *value, gboolean is_path);

#define SR_TYPE_APP (sr_app_get_type ())
G_DECLARE_FINAL_TYPE (SrApp, sr_app, SR, APP, GObject)

struct _SrApp {
    GObject parent_instance;

    char *id;
    char *name;
    char *comment;
    GIcon *icon;
    SrSource source;
    /* Overrides the source name on the card pill, e.g. "AppImage" */
    char *kind;
    char *install_type;
    /* All paths are host paths */
    char *location;
    char *desktop_file;
    char *desktop_file_note;
    char *exec;
    char *categories;
    char *portal_app_id;
    char *package;
    char *version;
    char *standalone_root;
    GPtrArray *details;
    char *search_key;
};

SrApp *sr_app_new (void);

void sr_app_add_detail (SrApp *self, const char *title, const char *value, gboolean is_path);

void sr_app_update_search_key (SrApp *self);

const char *sr_app_get_pill (SrApp *self);

int sr_app_compare (gconstpointer a, gconstpointer b, gpointer user_data);

const char *sr_source_get_id (SrSource source);

const char *sr_source_get_label (SrSource source);

const char *sr_source_get_description (SrSource source);

const char *sr_source_get_empty_title (SrSource source);

const char *sr_source_get_icon_name (SrSource source);

const char *sr_source_get_css_class (SrSource source);

G_END_DECLS

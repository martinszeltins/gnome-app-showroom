/* sr-utils.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

char *sr_normalize_casefold_and_unaccent (const char *str);

char *sr_build_search_key (const char *first, ...) G_GNUC_NULL_TERMINATED;

gboolean sr_search_key_matches (const char *key, const char *normalized_query);

char *sr_get_portal_app_id (GKeyFile *keyfile, const char *desktop_basename);

char *sr_clean_display_name (const char *file_name);

char *sr_read_symlink_basename (const char *path);

char *sr_realpath (const char *path);

gboolean sr_file_has_prefix_dir (const char *path, const char *dir);

char *sr_join_categories (const char *categories);

char *sr_yaml_get_field (const char *yaml, const char *field);

char *sr_read_first_line (const char *path);

G_END_DECLS

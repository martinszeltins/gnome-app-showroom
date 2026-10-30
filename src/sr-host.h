/* sr-host.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

gboolean sr_host_is_sandboxed (void);

char *sr_host_to_sandbox_path (const char *host_path);

char *sr_host_from_sandbox_path (const char *path);

char *sr_host_expand_home (const char *path);

gboolean sr_host_path_exists (const char *host_path);

gboolean sr_host_path_is_dir (const char *host_path);

char *sr_host_find_program (const char *program);

const char *const *sr_host_get_desktop_names (void);

void sr_host_setup_icon_theme (GdkDisplay *display);

void sr_host_open_file (const char *host_path);

void sr_host_show_location (const char *host_path);

G_END_DECLS

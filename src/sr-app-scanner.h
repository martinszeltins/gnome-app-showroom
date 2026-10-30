/* sr-app-scanner.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sr-app.h"
#include "sr-dpkg-index.h"

G_BEGIN_DECLS

GStrv sr_app_scanner_get_dir_paths (void);

GPtrArray *sr_app_scanner_scan (SrDpkgIndex *dpkg, const char *const *standalone_dirs, GCancellable *cancellable);

GIcon *sr_icon_from_desktop_value (const char *value);

char *sr_exec_get_program (const char *exec);

G_END_DECLS

/* sr-standalone-scanner.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sr-app.h"
#include "sr-dpkg-index.h"

G_BEGIN_DECLS

GPtrArray *sr_standalone_scanner_scan (const char *const *dirs, const char *const *appimage_only_dirs,
                                       SrDpkgIndex *dpkg, GCancellable *cancellable);

G_END_DECLS

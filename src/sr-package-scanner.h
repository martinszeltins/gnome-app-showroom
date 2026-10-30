/* sr-package-scanner.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sr-dpkg-index.h"
#include "sr-package.h"

G_BEGIN_DECLS

GPtrArray *sr_package_scanner_scan (SrDpkgIndex *dpkg, GCancellable *cancellable);

GPtrArray *sr_package_scanner_get_watched_paths (void);

G_END_DECLS

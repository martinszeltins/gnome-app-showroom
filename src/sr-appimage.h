/* sr-appimage.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sr-app.h"

G_BEGIN_DECLS

gboolean sr_appimage_detect (const char *path, int *type);

SrApp *sr_appimage_probe (const char *host_path, GCancellable *cancellable);

G_END_DECLS

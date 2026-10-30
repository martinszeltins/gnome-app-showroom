/* sr-bundle-probe.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sr-app.h"

G_BEGIN_DECLS

SrApp *sr_bundle_probe_dir (const char *host_dir, GCancellable *cancellable);

SrApp *sr_bundle_probe_binary (const char *host_path);

G_END_DECLS

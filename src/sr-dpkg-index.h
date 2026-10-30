/* sr-dpkg-index.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "sr-package.h"

G_BEGIN_DECLS

typedef struct _SrDpkgIndex SrDpkgIndex;

SrDpkgIndex *sr_dpkg_index_load (GCancellable *cancellable);

void sr_dpkg_index_free (SrDpkgIndex *self);

GPtrArray *sr_dpkg_index_get_packages (SrDpkgIndex *self);

SrPackage *sr_dpkg_index_lookup (SrDpkgIndex *self, const char *name);

SrPackage *sr_dpkg_index_get_owner (SrDpkgIndex *self, const char *host_path);

void sr_dpkg_index_finish_icons (SrDpkgIndex *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SrDpkgIndex, sr_dpkg_index_free)

G_END_DECLS

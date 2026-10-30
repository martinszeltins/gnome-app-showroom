/* sr-details-dialog.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "sr-app.h"
#include "sr-package.h"

G_BEGIN_DECLS

#define SR_TYPE_DETAILS_DIALOG (sr_details_dialog_get_type ())
G_DECLARE_FINAL_TYPE (SrDetailsDialog, sr_details_dialog, SR, DETAILS_DIALOG, AdwDialog)

SrDetailsDialog *sr_details_dialog_new_for_app (SrApp *app);

SrDetailsDialog *sr_details_dialog_new_for_package (SrPackage *package);

G_END_DECLS

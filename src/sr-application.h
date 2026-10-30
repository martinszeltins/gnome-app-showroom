/* sr-application.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SR_TYPE_APPLICATION (sr_application_get_type ())
G_DECLARE_FINAL_TYPE (SrApplication, sr_application, SR, APPLICATION, AdwApplication)

SrApplication *sr_application_new (void);

G_END_DECLS

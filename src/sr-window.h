/* sr-window.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SR_TYPE_WINDOW (sr_window_get_type ())
G_DECLARE_FINAL_TYPE (SrWindow, sr_window, SR, WINDOW, AdwApplicationWindow)

SrWindow *sr_window_new (AdwApplication *application);

void sr_window_reload (SrWindow *self);

G_END_DECLS

/* main.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "sr-application.h"

int
main (int argc, char *argv[])
{
    g_autoptr(SrApplication) app = NULL;

    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    app = sr_application_new ();

    return g_application_run (G_APPLICATION (app), argc, argv);
}

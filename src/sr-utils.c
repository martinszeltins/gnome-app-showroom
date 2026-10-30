/* sr-utils.c
 *
 * Copyright 2018 Matthias Clasen <matthias.clasen@gmail.com>
 * Copyright 2026 Martins
 *
 * sr_normalize_casefold_and_unaccent() comes from gnome-control-center
 * (panels/common/cc-util.c), which copied it from tracker and gnome-shell.
 * sr_get_portal_app_id() follows get_portal_app_id() from
 * panels/applications/cc-applications-panel.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sr-utils.h"

/* Combining diacritical mark?
 *  Basic range: [0x0300,0x036F]
 *  Supplement:  [0x1DC0,0x1DFF]
 *  For Symbols: [0x20D0,0x20FF]
 *  Half marks:  [0xFE20,0xFE2F]
 */
#define IS_CDM_UCS4(c)                                                                                                 \
    (((c) >= 0x0300 && (c) <= 0x036F) || ((c) >= 0x1DC0 && (c) <= 0x1DFF) || ((c) >= 0x20D0 && (c) <= 0x20FF)          \
     || ((c) >= 0xFE20 && (c) <= 0xFE2F))

#define IS_SOFT_HYPHEN(c) ((c) == 0x00AD)

char *
sr_normalize_casefold_and_unaccent (const char *str)
{
    g_autofree gchar *normalized = NULL;
    gchar *tmp;
    int i = 0, j = 0, ilen;

    if (str == NULL)
        return NULL;

    normalized = g_utf8_normalize (str, -1, G_NORMALIZE_NFKD);
    if (normalized == NULL)
        return g_strdup ("");
    tmp = g_utf8_casefold (normalized, -1);

    ilen = strlen (tmp);

    while (i < ilen) {
        gunichar unichar;
        gchar *next_utf8;
        gint utf8_len;

        unichar = g_utf8_get_char_validated (&tmp[i], -1);

        if (unichar == (gunichar) -1 || unichar == (gunichar) -2)
            break;

        next_utf8 = g_utf8_next_char (&tmp[i]);
        utf8_len = next_utf8 - &tmp[i];

        if (IS_CDM_UCS4 (unichar) || IS_SOFT_HYPHEN (unichar)) {
            i += utf8_len;
            continue;
        }

        if (i != j)
            memmove (&tmp[j], &tmp[i], utf8_len);

        i += utf8_len;
        j += utf8_len;
    }

    tmp[j] = '\0';

    return tmp;
}

char *
sr_build_search_key (const char *first, ...)
{
    g_autoptr(GString) str = g_string_new (NULL);
    const char *s;
    va_list args;

    va_start (args, first);
    for (s = first; s != NULL; s = va_arg (args, const char *)) {
        g_autofree char *norm = sr_normalize_casefold_and_unaccent (s);

        if (str->len > 0)
            g_string_append_c (str, '\n');
        g_string_append (str, norm);
    }
    va_end (args);

    return g_string_free (g_steal_pointer (&str), FALSE);
}

gboolean
sr_search_key_matches (const char *key, const char *normalized_query)
{
    if (normalized_query == NULL || *normalized_query == '\0')
        return TRUE;

    return key != NULL && strstr (key, normalized_query) != NULL;
}

char *
sr_get_portal_app_id (GKeyFile *keyfile, const char *desktop_basename)
{
    g_autofree char *flatpak_id = NULL;
    g_autofree char *snap_name = NULL;

    flatpak_id = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL);
    if (flatpak_id != NULL)
        return g_steal_pointer (&flatpak_id);

    snap_name = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-SnapInstanceName", NULL);
    if (snap_name != NULL)
        return g_strdup_printf ("snap.%s", snap_name);

    return NULL;
}

static char *
regex_replace (const char *pattern, const char *input, const char *replacement)
{
    g_autoptr(GRegex) regex = g_regex_new (pattern, G_REGEX_CASELESS, 0, NULL);

    return g_regex_replace (regex, input, -1, 0, replacement, 0, NULL);
}

char *
sr_clean_display_name (const char *file_name)
{
    g_autofree char *name = g_strdup (file_name);
    char *tmp;

    if (g_str_has_suffix (name, ".AppImage") || g_str_has_suffix (name, ".appimage"))
        name[strlen (name) - strlen (".AppImage")] = '\0';

    tmp = regex_replace ("[-_. ]?[0-9a-f]{32}", name, "");
    g_free (name);
    name = tmp;

    tmp = regex_replace ("[-_. ]?(x86[-_]64|amd64|x64|aarch64|arm64|armhf|i[36]86|linux)(?=$|[-_. ])", name, "");
    g_free (name);
    name = tmp;

    tmp = regex_replace ("[-_ ]v?[0-9]+(\\.[0-9]+)+[^-_ ]*", name, "");
    g_free (name);
    name = tmp;

    g_strdelimit (name, "-_", ' ');

    tmp = regex_replace ("\\s+", name, " ");
    g_free (name);
    name = tmp;
    g_strstrip (name);

    if (*name == '\0')
        return g_strdup (file_name);

    return g_steal_pointer (&name);
}

char *
sr_read_symlink_basename (const char *path)
{
    g_autofree char *target = g_file_read_link (path, NULL);

    if (target == NULL)
        return NULL;

    return g_path_get_basename (target);
}

char *
sr_realpath (const char *path)
{
    char *resolved = realpath (path, NULL);
    char *ret;

    if (resolved == NULL)
        return NULL;

    ret = g_strdup (resolved);
    free (resolved);
    return ret;
}

gboolean
sr_file_has_prefix_dir (const char *path, const char *dir)
{
    gsize len;

    if (path == NULL || dir == NULL)
        return FALSE;

    len = strlen (dir);
    while (len > 1 && dir[len - 1] == '/')
        len--;

    return strncmp (path, dir, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

/* Reads a top-level "key: value" line, which is all the snap.yaml fields
 * we need. Block scalars and nested keys are not supported. */
char *
sr_yaml_get_field (const char *yaml, const char *field)
{
    g_autofree char *prefix = g_strconcat (field, ":", NULL);
    g_auto(GStrv) lines = g_strsplit (yaml, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix (lines[i], prefix)) {
            char *value = g_strstrip (g_strdup (lines[i] + strlen (prefix)));
            gsize len = strlen (value);

            if (len >= 2 && (value[0] == '\'' || value[0] == '"') && value[len - 1] == value[0]) {
                memmove (value, value + 1, len - 2);
                value[len - 2] = '\0';
            }
            if (*value == '\0' || g_str_equal (value, "|") || g_str_equal (value, ">")) {
                g_free (value);
                return NULL;
            }
            return value;
        }
    }

    return NULL;
}

char *
sr_read_first_line (const char *path)
{
    g_autofree char *contents = NULL;
    char *newline;

    if (!g_file_get_contents (path, &contents, NULL, NULL))
        return NULL;

    newline = strchr (contents, '\n');
    if (newline != NULL)
        *newline = '\0';
    g_strstrip (contents);

    return *contents != '\0' ? g_steal_pointer (&contents) : NULL;
}

char *
sr_join_categories (const char *categories)
{
    g_auto(GStrv) parts = NULL;
    g_autoptr(GPtrArray) clean = NULL;

    if (categories == NULL)
        return NULL;

    parts = g_strsplit (categories, ";", -1);
    clean = g_ptr_array_new ();
    for (guint i = 0; parts[i] != NULL; i++) {
        if (*parts[i] != '\0')
            g_ptr_array_add (clean, parts[i]);
    }
    if (clean->len == 0)
        return NULL;
    g_ptr_array_add (clean, NULL);

    return g_strjoinv (", ", (char **) clean->pdata);
}

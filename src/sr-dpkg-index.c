/* sr-dpkg-index.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-dpkg"

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "sr-dpkg-index.h"
#include "sr-host.h"
#include "sr-utils.h"

#define DPKG_DIR "/var/lib/dpkg"
#define EXTENDED_STATES "/var/lib/apt/extended_states"

struct _SrDpkgIndex {
    GPtrArray *packages;
    /* "name" and "name:arch" -> SrPackage (borrowed) */
    GHashTable *by_name;
    /* host path -> SrPackage (borrowed). Only desktop files, binaries and
     * /opt/<dir> entries are indexed, which is all the scanners ask about. */
    GHashTable *owners;
};

typedef struct {
    char *bin;
    char *opt;
    char *so;
    char *own_dir;
    char *other;
    char *doc;
    int icon_score;
} ListScan;

static void
list_scan_clear (ListScan *scan)
{
    g_free (scan->bin);
    g_free (scan->opt);
    g_free (scan->so);
    g_free (scan->own_dir);
    g_free (scan->other);
    g_free (scan->doc);
}

static GHashTable *
load_auto_installed (void)
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) paragraphs = NULL;
    GHashTable *auto_installed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    if (!g_file_get_contents (EXTENDED_STATES, &contents, NULL, NULL))
        return auto_installed;

    paragraphs = g_strsplit (contents, "\n\n", -1);
    for (guint i = 0; paragraphs[i] != NULL; i++) {
        g_auto(GStrv) lines = g_strsplit (paragraphs[i], "\n", -1);
        const char *name = NULL, *arch = NULL;
        gboolean is_auto = FALSE;

        for (guint j = 0; lines[j] != NULL; j++) {
            if (g_str_has_prefix (lines[j], "Package: "))
                name = lines[j] + strlen ("Package: ");
            else if (g_str_has_prefix (lines[j], "Architecture: "))
                arch = lines[j] + strlen ("Architecture: ");
            else if (g_str_equal (lines[j], "Auto-Installed: 1"))
                is_auto = TRUE;
        }

        if (name != NULL && is_auto) {
            g_hash_table_add (auto_installed, g_strdup (name));
            if (arch != NULL)
                g_hash_table_add (auto_installed, g_strdup_printf ("%s:%s", name, arch));
        }
    }

    return auto_installed;
}

static int
icon_path_score (const char *path)
{
    const char *rest;
    char *end;
    long size;

    /* XPM is left out because the GNOME runtime's image loader can't read it */
    if (!(g_str_has_suffix (path, ".png") || g_str_has_suffix (path, ".svg")))
        return 0;

    if (g_str_has_prefix (path, "/usr/share/pixmaps/") && strchr (path + strlen ("/usr/share/pixmaps/"), '/') == NULL)
        return 48;

    if (!g_str_has_prefix (path, "/usr/share/icons/hicolor/") || strstr (path, "/apps/") == NULL)
        return 0;

    rest = path + strlen ("/usr/share/icons/hicolor/");
    if (g_str_has_prefix (rest, "scalable/"))
        return 1000;

    size = strtol (rest, &end, 10);
    if (end == rest || *end != 'x')
        return 0;

    return (int) size;
}

static gboolean
is_noise_path (const char *path)
{
    static const char *const prefixes[] = {
        "/usr/share/doc", "/usr/share/man", "/usr/share/lintian", "/usr/share/locale",
        "/usr/share/doc-base", "/usr/share/bug", "/usr/share/info", "/etc",
    };

    for (guint i = 0; i < G_N_ELEMENTS (prefixes); i++) {
        if (sr_file_has_prefix_dir (path, prefixes[i]))
            return TRUE;
    }

    return FALSE;
}

static gboolean
is_direct_child (const char *path, const char *dir)
{
    gsize len = strlen (dir);

    return strncmp (path, dir, len) == 0 && path[len] == '/' && path[len + 1] != '\0'
           && strchr (path + len + 1, '/') == NULL;
}

static void
scan_list_file (SrDpkgIndex *self, SrPackage *package, GCancellable *cancellable)
{
    g_autofree char *contents = NULL;
    g_autofree char *own_share = NULL;
    g_autofree char *own_lib = NULL;
    g_autofree char *own_doc = NULL;
    ListScan scan = { 0 };
    char *line, *next;

    if (package->list_file == NULL || !g_file_get_contents (package->list_file, &contents, NULL, NULL))
        return;

    own_share = g_strconcat ("/usr/share/", package->name, NULL);
    own_lib = g_strconcat ("/usr/lib/", package->name, NULL);
    own_doc = g_strconcat ("/usr/share/doc/", package->name, NULL);

    for (line = contents; line != NULL && *line != '\0'; line = next) {
        int score;

        next = strchr (line, '\n');
        if (next != NULL)
            *next++ = '\0';

        if (line[0] != '/' || line[1] == '.' || line[1] == '\0')
            continue;

        if (g_str_has_prefix (line, "/usr/share/applications/") && g_str_has_suffix (line, ".desktop")) {
            g_ptr_array_add (package->desktop_files, g_strdup (line));
            g_hash_table_insert (self->owners, g_strdup (line), package);
            continue;
        }

        if (is_direct_child (line, "/usr/bin") || is_direct_child (line, "/usr/sbin") || is_direct_child (line, "/bin")
            || is_direct_child (line, "/sbin") || is_direct_child (line, "/usr/games")) {
            package->has_binaries = TRUE;
            if (scan.bin == NULL)
                scan.bin = g_strdup (line);
            g_hash_table_insert (self->owners, g_strdup (line), package);
            continue;
        }

        if (is_direct_child (line, "/opt")) {
            if (scan.opt == NULL)
                scan.opt = g_strdup (line);
            g_hash_table_insert (self->owners, g_strdup (line), package);
            continue;
        }

        score = icon_path_score (line);
        if (score > scan.icon_score) {
            scan.icon_score = score;
            g_free (package->icon_file);
            package->icon_file = g_strdup (line);
        }

        if (scan.so == NULL && strstr (line, ".so") != NULL
            && (g_str_has_prefix (line, "/usr/lib/") || g_str_has_prefix (line, "/lib/"))) {
            scan.so = g_strdup (line);
        } else if (scan.own_dir == NULL && (g_str_equal (line, own_share) || g_str_equal (line, own_lib))) {
            scan.own_dir = g_strdup (line);
        } else if (scan.doc == NULL && g_str_equal (line, own_doc)) {
            scan.doc = g_strdup (line);
        } else if (scan.other == NULL && !is_noise_path (line) && strchr (line + 1, '/') != NULL
                   && strchr (strchr (line + 1, '/') + 1, '/') != NULL) {
            scan.other = g_strdup (line);
        }
    }

    if (scan.bin)
        package->location = g_steal_pointer (&scan.bin);
    else if (scan.opt)
        package->location = g_steal_pointer (&scan.opt);
    else if (scan.so)
        package->location = g_steal_pointer (&scan.so);
    else if (scan.own_dir)
        package->location = g_steal_pointer (&scan.own_dir);
    else if (scan.other)
        package->location = g_steal_pointer (&scan.other);
    else if (scan.doc)
        package->location = g_steal_pointer (&scan.doc);

    list_scan_clear (&scan);
}

static char *
find_list_file (const char *name, const char *arch)
{
    g_autofree char *with_arch = g_strdup_printf (DPKG_DIR "/info/%s:%s.list", name, arch ? arch : "");
    g_autofree char *plain = g_strdup_printf (DPKG_DIR "/info/%s.list", name);

    if (arch != NULL && g_file_test (with_arch, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&with_arch);
    if (g_file_test (plain, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&plain);

    return NULL;
}

static SrPackage *
parse_paragraph (const char *paragraph, GHashTable *auto_installed)
{
    g_autoptr(SrPackage) package = sr_package_new ();
    g_auto(GStrv) lines = g_strsplit (paragraph, "\n", -1);
    g_autoptr(GString) description = NULL;
    gboolean installed = FALSE;
    gboolean in_description = FALSE;
    g_autofree char *qualified = NULL;

    for (guint i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];

        if (line[0] == ' ' || line[0] == '\t') {
            if (in_description) {
                const char *text = line + 1;

                if (description == NULL)
                    description = g_string_new (NULL);
                if (g_str_equal (text, "."))
                    g_string_append (description, "\n");
                else {
                    if (description->len > 0 && description->str[description->len - 1] != '\n')
                        g_string_append_c (description, ' ');
                    g_string_append (description, text);
                }
            }
            continue;
        }

        in_description = FALSE;

#define FIELD(key) (g_str_has_prefix (line, key ": ") ? line + strlen (key ": ") : NULL)
        const char *value;
        if ((value = FIELD ("Package")))
            package->name = g_strdup (value);
        else if ((value = FIELD ("Status")))
            installed = g_str_has_suffix (value, " installed");
        else if ((value = FIELD ("Architecture")))
            package->arch = g_strdup (value);
        else if ((value = FIELD ("Version")))
            package->version = g_strdup (value);
        else if ((value = FIELD ("Section")))
            package->section = g_strdup (value);
        else if ((value = FIELD ("Maintainer")))
            package->maintainer = g_strdup (value);
        else if ((value = FIELD ("Homepage")))
            package->homepage = g_strdup (value);
        else if ((value = FIELD ("Installed-Size")))
            package->installed_size_kib = g_ascii_strtoull (value, NULL, 10);
        else if ((value = FIELD ("Description"))) {
            package->summary = g_strdup (value);
            in_description = TRUE;
        }
#undef FIELD
    }

    if (!installed || package->name == NULL)
        return NULL;

    if (description != NULL)
        package->description = g_strstrip (g_string_free (g_steal_pointer (&description), FALSE));

    qualified = g_strdup_printf ("%s:%s", package->name, package->arch ? package->arch : "");
    package->auto_installed
        = g_hash_table_contains (auto_installed, qualified) || g_hash_table_contains (auto_installed, package->name);
    package->list_file = find_list_file (package->name, package->arch);

    return g_steal_pointer (&package);
}

static int
compare_packages (gconstpointer a, gconstpointer b)
{
    SrPackage *pa = *(SrPackage **) a;
    SrPackage *pb = *(SrPackage **) b;
    int ret = strcmp (pa->name, pb->name);

    return ret != 0 ? ret : g_strcmp0 (pa->arch, pb->arch);
}

SrDpkgIndex *
sr_dpkg_index_load (GCancellable *cancellable)
{
    g_autoptr(SrDpkgIndex) self = g_new0 (SrDpkgIndex, 1);
    g_autoptr(GHashTable) auto_installed = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) paragraphs = NULL;

    self->packages = g_ptr_array_new_with_free_func (g_object_unref);
    self->by_name = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    self->owners = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    if (!g_file_get_contents (DPKG_DIR "/status", &contents, NULL, &error)) {
        g_debug ("No dpkg database: %s", error->message);
        return g_steal_pointer (&self);
    }

    auto_installed = load_auto_installed ();
    paragraphs = g_strsplit (contents, "\n\n", -1);
    for (guint i = 0; paragraphs[i] != NULL; i++) {
        SrPackage *package;

        if (g_cancellable_is_cancelled (cancellable))
            return NULL;

        package = parse_paragraph (paragraphs[i], auto_installed);
        if (package == NULL)
            continue;

        g_ptr_array_add (self->packages, package);
        if (!g_hash_table_contains (self->by_name, package->name))
            g_hash_table_insert (self->by_name, g_strdup (package->name), package);
        g_hash_table_insert (self->by_name, g_strdup_printf ("%s:%s", package->name, package->arch), package);
    }

    g_ptr_array_sort (self->packages, compare_packages);

    for (guint i = 0; i < self->packages->len; i++) {
        if (g_cancellable_is_cancelled (cancellable))
            return NULL;
        scan_list_file (self, g_ptr_array_index (self->packages, i), cancellable);
    }

    return g_steal_pointer (&self);
}

void
sr_dpkg_index_free (SrDpkgIndex *self)
{
    if (self == NULL)
        return;

    g_clear_pointer (&self->packages, g_ptr_array_unref);
    g_clear_pointer (&self->by_name, g_hash_table_unref);
    g_clear_pointer (&self->owners, g_hash_table_unref);
    g_free (self);
}

GPtrArray *
sr_dpkg_index_get_packages (SrDpkgIndex *self)
{
    return self->packages;
}

SrPackage *
sr_dpkg_index_lookup (SrDpkgIndex *self, const char *name)
{
    return g_hash_table_lookup (self->by_name, name);
}

SrPackage *
sr_dpkg_index_get_owner (SrDpkgIndex *self, const char *host_path)
{
    SrPackage *package;

    if (host_path == NULL)
        return NULL;

    package = g_hash_table_lookup (self->owners, host_path);
    if (package != NULL)
        return package;

    /* With merged /usr, dpkg may record /bin/foo for what is /usr/bin/foo */
    if (g_str_has_prefix (host_path, "/usr/bin/") || g_str_has_prefix (host_path, "/usr/sbin/"))
        return g_hash_table_lookup (self->owners, host_path + strlen ("/usr"));

    return NULL;
}

static const char *
heuristic_icon_name (SrPackage *package)
{
    const char *section = package->section != NULL ? package->section : "";
    const char *slash = strrchr (section, '/');

    /* "universe/libs" -> "libs" */
    if (slash != NULL)
        section = slash + 1;

    if (g_str_has_prefix (package->name, "fonts-") || g_str_equal (section, "fonts"))
        return "font-x-generic";
    if (g_str_has_suffix (package->name, "-keyring") || g_str_has_prefix (package->name, "ca-certificates"))
        return "application-certificate";
    if (g_str_has_prefix (package->name, "linux-firmware") || g_str_equal (section, "kernel")
        || g_str_has_suffix (package->name, "-firmware"))
        return "application-x-firmware";
    if (!package->has_binaries
        && (g_str_equal (section, "libs") || g_str_equal (section, "oldlibs") || g_str_equal (section, "libdevel")
            || g_str_has_prefix (package->name, "lib")))
        return "application-x-sharedlib";
    if (g_str_equal (section, "doc"))
        return "x-office-document";
    if (g_str_equal (section, "sound"))
        return "audio-x-generic";
    if (g_str_equal (section, "video"))
        return "video-x-generic";
    if (g_str_equal (section, "graphics"))
        return "image-x-generic";
    if (g_str_equal (section, "python") || g_str_equal (section, "perl") || g_str_equal (section, "ruby")
        || g_str_equal (section, "javascript") || g_str_equal (section, "interpreters"))
        return "text-x-script";
    if (g_str_equal (section, "localization"))
        return "x-office-document";
    if (package->has_binaries)
        return "application-x-executable";

    return "package-x-generic";
}

void
sr_dpkg_index_finish_icons (SrDpkgIndex *self)
{
    for (guint i = 0; i < self->packages->len; i++) {
        SrPackage *package = g_ptr_array_index (self->packages, i);

        if (package->icon != NULL)
            continue;

        if (package->icon_file != NULL) {
            g_autofree char *path = sr_host_to_sandbox_path (package->icon_file);
            g_autoptr(GFile) file = g_file_new_for_path (path);

            package->icon = g_file_icon_new (file);
        } else {
            package->icon = g_themed_icon_new (heuristic_icon_name (package));
        }
    }
}

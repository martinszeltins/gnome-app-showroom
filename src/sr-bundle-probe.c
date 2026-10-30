/* sr-bundle-probe.c
 *
 * Copyright 2026 Martins
 *
 * Folders in ~/Applications and /opt have no common layout, so a few
 * strategies are tried in order: a bundled desktop file, Mozilla's
 * application.ini, Electron's package.json, then a generic search for the
 * main executable and the best-looking icon. Folders that only contain
 * command-line programs become a single "CLI" item.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#define G_LOG_DOMAIN "sr-bundle-probe"

#include "config.h"

#include <fcntl.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sr-app-scanner.h"
#include "sr-bundle-probe.h"
#include "sr-host.h"
#include "sr-utils.h"

#define GUI_SCAN_BYTES (4 * 1024 * 1024)

typedef enum {
    EXE_NONE,
    EXE_ELF,
    EXE_SCRIPT,
} ExeType;

typedef struct {
    char *path;
    char *name;
    ExeType type;
    goffset size;
} Exe;

static void
exe_free (Exe *exe)
{
    g_free (exe->path);
    g_free (exe->name);
    g_free (exe);
}

static const char *const helper_names[] = {
    "chrome-sandbox", "chrome_crashpad_handler", "crashpad_handler", "crashreporter", "crashhelper",
    "updater", "pingsender", "gfxtest", "plugin-container", "minidump-analyzer", "glxtest", "vaapitest",
    "uninstall", "uninstaller", "update", "apprun", "AppRun", "xdg-open", "default-browser-agent",
};

static ExeType
get_exe_type (const char *path, goffset *size)
{
    guchar header[18];
    struct stat st;
    ssize_t n;
    int fd;

    if (stat (path, &st) != 0 || !S_ISREG (st.st_mode) || (st.st_mode & 0111) == 0)
        return EXE_NONE;

    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return EXE_NONE;
    n = read (fd, header, sizeof header);
    close (fd);

    if (size)
        *size = st.st_size;

    if (n >= 18 && memcmp (header, "\177ELF", 4) == 0) {
        guint16 e_type = header[5] == 1 ? (header[16] | header[17] << 8) : (header[16] << 8 | header[17]);

        /* ET_EXEC, or ET_DYN for position-independent executables */
        return e_type == 2 || e_type == 3 ? EXE_ELF : EXE_NONE;
    }

    if (n >= 2 && header[0] == '#' && header[1] == '!')
        return EXE_SCRIPT;

    return EXE_NONE;
}

static gboolean
is_helper (const char *name)
{
    g_autofree char *lower = g_ascii_strdown (name, -1);

    for (guint i = 0; i < G_N_ELEMENTS (helper_names); i++) {
        if (g_ascii_strcasecmp (name, helper_names[i]) == 0)
            return TRUE;
    }

    return strstr (lower, ".so") != NULL || strstr (lower, "crashpad") != NULL;
}

static void
add_executables (GPtrArray *exes, const char *dir)
{
    g_autoptr(GDir) gdir = g_dir_open (dir, 0, NULL);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
        g_autofree char *path = g_build_filename (dir, name, NULL);
        goffset size = 0;
        ExeType type;
        Exe *exe;

        if (name[0] == '.')
            continue;

        type = get_exe_type (path, &size);
        if (type == EXE_NONE)
            continue;

        exe = g_new0 (Exe, 1);
        exe->path = g_steal_pointer (&path);
        exe->name = g_strdup (name);
        exe->type = type;
        exe->size = size;
        g_ptr_array_add (exes, exe);
    }
}

static GPtrArray *
list_executables (const char *dir)
{
    GPtrArray *exes = g_ptr_array_new_with_free_func ((GDestroyNotify) exe_free);
    g_autofree char *bin = g_build_filename (dir, "bin", NULL);

    add_executables (exes, dir);
    add_executables (exes, bin);

    return exes;
}

static Exe *
pick_main_exe (GPtrArray *exes, const char *dir_name, const char *app_name)
{
    Exe *best = NULL;
    gint64 best_score = G_MININT64;

    for (guint i = 0; i < exes->len; i++) {
        Exe *exe = g_ptr_array_index (exes, i);
        gint64 score = 0;

        if (is_helper (exe->name))
            continue;

        if (g_ascii_strcasecmp (exe->name, dir_name) == 0)
            score += 10000;
        if (app_name != NULL && g_ascii_strcasecmp (exe->name, app_name) == 0)
            score += 10000;
        if (strcmp (exe->name, dir_name) == 0)
            score += 500;
        if (exe->type == EXE_ELF)
            score += 2000;
        if (g_str_has_suffix (exe->name, "-bin") || g_str_has_suffix (exe->name, ".sh"))
            score -= 1500;
        score += MIN (exe->size / (1024 * 1024), 999);

        if (score > best_score) {
            best_score = score;
            best = exe;
        }
    }

    return best;
}

static gboolean
file_mentions_any (const char *path, const char *const *needles)
{
    g_autofree char *data = NULL;
    gssize n;
    int fd;
    gboolean found = FALSE;

    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return FALSE;

    data = g_malloc (GUI_SCAN_BYTES);
    n = read (fd, data, GUI_SCAN_BYTES);
    close (fd);

    for (guint i = 0; n > 0 && needles[i] != NULL && !found; i++)
        found = memmem (data, n, needles[i], strlen (needles[i])) != NULL;

    return found;
}

static gboolean
dir_has_shared_libraries (const char *dir)
{
    g_autoptr(GDir) gdir = g_dir_open (dir, 0, NULL);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
        if (g_str_has_suffix (name, ".so") || strstr (name, ".so.") != NULL || g_str_has_suffix (name, ".pak"))
            return TRUE;
    }

    return FALSE;
}

static gboolean
has_gui_markers (const char *dir, Exe *main_exe)
{
    static const char *const gui_libs[] = {
        "libgtk", "libQt", "libX11.so", "libwayland-client", "libxul", "libSDL2", "libGL.so", "libEGL", "libvulkan",
        NULL,
    };
    g_autofree char *lib = g_build_filename (dir, "lib", NULL);
    g_autofree char *resources = g_build_filename (dir, "resources", NULL);

    if (dir_has_shared_libraries (dir) || dir_has_shared_libraries (lib))
        return TRUE;

    if (g_file_test (resources, G_FILE_TEST_IS_DIR))
        return TRUE;

    return main_exe != NULL && main_exe->type == EXE_ELF && file_mentions_any (main_exe->path, gui_libs);
}

static int
image_size (const char *path)
{
    guchar header[24];
    int fd;
    ssize_t n;

    if (g_str_has_suffix (path, ".svg"))
        return 128;
    if (g_str_has_suffix (path, ".xpm"))
        return 32;

    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    n = read (fd, header, sizeof header);
    close (fd);

    if (n < 24 || memcmp (header, "\x89PNG", 4) != 0)
        return 0;

    return (header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19];
}

typedef struct {
    char *path;
    int score;
} IconCandidate;

static void
collect_icons (const char *dir, int depth, int max_depth, int weight, const char *app_name, GHashTable *seen,
               IconCandidate *best)
{
    static const char *const skip_dirs[] = { "node_modules", "locales", "locale", ".git", "doc", "docs", "man" };
    static const char *const bad_words[] = { "updater", "uninstall", "crash", "splash", "tray", "symbolic",
                                             "background", "banner", "badge", "overlay" };
    g_autoptr(GDir) gdir = g_dir_open (dir, 0, NULL);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
        g_autofree char *path = g_build_filename (dir, name, NULL);
        g_autofree char *lower = NULL;
        gboolean skip = FALSE;
        int size, score;

        if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
            for (guint i = 0; i < G_N_ELEMENTS (skip_dirs); i++)
                skip |= g_str_equal (name, skip_dirs[i]);
            if (!skip && depth < max_depth)
                collect_icons (path, depth + 1, max_depth, weight, app_name, seen, best);
            continue;
        }

        if (!(g_str_has_suffix (name, ".png") || g_str_has_suffix (name, ".svg") || g_str_has_suffix (name, ".xpm")))
            continue;
        if (g_hash_table_contains (seen, path))
            continue;
        g_hash_table_add (seen, g_strdup (path));

        lower = g_ascii_strdown (name, -1);
        for (guint i = 0; i < G_N_ELEMENTS (bad_words); i++)
            skip |= strstr (lower, bad_words[i]) != NULL;
        if (skip)
            continue;

        size = image_size (path);
        if (size < 24)
            continue;

        score = MIN (size, 512) + weight;
        if (strstr (lower, "icon") != NULL || strstr (lower, "logo") != NULL)
            score += 150;
        if (app_name != NULL && strlen (app_name) >= 3 && strstr (lower, app_name) != NULL)
            score += 250;
        if (strstr (path, "/hicolor/") != NULL && strstr (path, "/apps/") != NULL)
            score += 100;

        if (score > best->score) {
            best->score = score;
            g_free (best->path);
            best->path = g_steal_pointer (&path);
        }
    }
}

static char *
find_best_icon (const char *dir, const char *app_name)
{
    static const struct {
        const char *subdir;
        int max_depth;
        int weight;
    } roots[] = {
        { "", 0, 150 },
        { "icons", 2, 200 },
        { "browser/chrome/icons/default", 0, 200 },
        { "share/icons", 5, 150 },
        { "share/pixmaps", 1, 150 },
        { "pixmaps", 1, 100 },
        { "assets", 2, 50 },
        { "images", 1, 50 },
        { "img", 1, 50 },
        { "data", 2, 0 },
        { "lib", 1, 0 },
        { "resources", 3, 0 },
    };
    g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *needle = app_name != NULL ? g_ascii_strdown (app_name, -1) : NULL;
    IconCandidate best = { NULL, -1 };

    for (guint i = 0; i < G_N_ELEMENTS (roots); i++) {
        g_autofree char *root = g_build_filename (dir, roots[i].subdir, NULL);

        if (g_file_test (root, G_FILE_TEST_IS_DIR))
            collect_icons (root, 0, roots[i].max_depth, roots[i].weight, needle, seen, &best);
    }

    return best.path;
}

static GIcon *
file_icon_new (const char *path)
{
    g_autoptr(GFile) file = NULL;

    if (path == NULL)
        return NULL;

    file = g_file_new_for_path (path);
    return g_file_icon_new (file);
}

static GIcon *
terminal_icon_new (void)
{
    const char *names[] = { "utilities-terminal", "org.gnome.Console", "org.gnome.Terminal",
                            "application-x-executable" };

    return g_themed_icon_new_from_names ((char **) names, G_N_ELEMENTS (names));
}

static char *
find_desktop_file (const char *dir)
{
    const char *subdirs[] = { "", "share/applications" };

    for (guint i = 0; i < G_N_ELEMENTS (subdirs); i++) {
        g_autofree char *path = g_build_filename (dir, subdirs[i], NULL);
        g_autoptr(GDir) gdir = g_dir_open (path, 0, NULL);
        g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
        const char *name;

        while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
            if (g_str_has_suffix (name, ".desktop"))
                g_ptr_array_add (names, g_strdup (name));
        }

        if (names->len > 0) {
            g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
            return g_build_filename (path, g_ptr_array_index (names, 0), NULL);
        }
    }

    return NULL;
}

static GIcon *
resolve_bundle_icon (const char *dir, const char *value, const char *app_name)
{
    g_autofree char *best = NULL;

    if (value != NULL && *value != '\0') {
        const char *exts[] = { "", ".png", ".svg", ".xpm" };
        g_autofree char *base = NULL;

        if (g_path_is_absolute (value))
            return sr_icon_from_desktop_value (value);

        if (strchr (value, '/') != NULL) {
            g_autofree char *path = g_build_filename (dir, value, NULL);

            if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
                return file_icon_new (path);
        }

        base = g_path_get_basename (value);
        for (guint i = 0; i < G_N_ELEMENTS (exts); i++) {
            g_autofree char *file_name = g_strconcat (base, exts[i], NULL);
            g_autofree char *path = g_build_filename (dir, file_name, NULL);
            g_autofree char *pixmap = g_build_filename (dir, "share", "pixmaps", file_name, NULL);

            if (*exts[i] != '\0' && g_file_test (path, G_FILE_TEST_IS_REGULAR))
                return file_icon_new (path);
            if (*exts[i] != '\0' && g_file_test (pixmap, G_FILE_TEST_IS_REGULAR))
                return file_icon_new (pixmap);
        }

        best = find_best_icon (dir, base);
        if (best != NULL)
            return file_icon_new (best);

        return sr_icon_from_desktop_value (value);
    }

    best = find_best_icon (dir, app_name);
    return best != NULL ? file_icon_new (best) : NULL;
}

static SrApp *
probe_desktop_file (const char *dir)
{
    g_autofree char *desktop_path = find_desktop_file (dir);
    g_autoptr(GKeyFile) keyfile = g_key_file_new ();
    g_autoptr(SrApp) app = NULL;
    g_autofree char *icon = NULL;
    g_autofree char *categories = NULL;
    g_autofree char *exec = NULL;
    g_autofree char *program = NULL;

    if (desktop_path == NULL || !g_key_file_load_from_file (keyfile, desktop_path, G_KEY_FILE_NONE, NULL))
        return NULL;

    app = sr_app_new ();
    app->name = g_key_file_get_locale_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
    if (app->name == NULL)
        return NULL;

    app->comment
        = g_key_file_get_locale_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL, NULL);
    icon = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
    app->icon = resolve_bundle_icon (dir, icon, app->name);
    categories = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_CATEGORIES, NULL);
    app->categories = sr_join_categories (categories);
    app->desktop_file = sr_host_from_sandbox_path (desktop_path);
    app->desktop_file_note = g_strdup (_("Shipped inside the app folder"));
    app->install_type = g_strdup (_("App folder with a desktop entry"));

    exec = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
    program = sr_exec_get_program (exec);
    if (program != NULL && !g_path_is_absolute (program)) {
        g_autofree char *local = g_build_filename (dir, program, NULL);

        if (g_file_test (local, G_FILE_TEST_EXISTS))
            app->exec = sr_host_from_sandbox_path (local);
    } else if (program != NULL) {
        app->exec = g_strdup (program);
    }

    return g_steal_pointer (&app);
}

static SrApp *
probe_mozilla (const char *dir, GPtrArray *exes)
{
    const char *candidates[] = { "application.ini", "browser/application.ini" };
    g_autoptr(GKeyFile) ini = g_key_file_new ();
    g_autoptr(SrApp) app = NULL;
    g_autofree char *code_name = NULL;
    g_autofree char *vendor = NULL;
    g_autofree char *exe_name = NULL;
    g_autofree char *exe_path = NULL;
    g_autofree char *icon = NULL;
    gboolean loaded = FALSE;

    for (guint i = 0; i < G_N_ELEMENTS (candidates) && !loaded; i++) {
        g_autofree char *path = g_build_filename (dir, candidates[i], NULL);
        g_autofree char *contents = NULL;
        g_autoptr(GString) cleaned = NULL;
        g_auto(GStrv) lines = NULL;

        if (!g_file_get_contents (path, &contents, NULL, NULL))
            continue;

        /* INI files use ';' comments, which GKeyFile rejects */
        cleaned = g_string_new (NULL);
        lines = g_strsplit (contents, "\n", -1);
        for (guint j = 0; lines[j] != NULL; j++) {
            if (lines[j][0] != ';')
                g_string_append_printf (cleaned, "%s\n", lines[j]);
        }

        loaded = g_key_file_load_from_data (ini, cleaned->str, cleaned->len, G_KEY_FILE_NONE, NULL);
    }

    if (!loaded)
        return NULL;

    app = sr_app_new ();
    app->name = g_key_file_get_string (ini, "App", "Name", NULL);
    if (app->name == NULL)
        return NULL;

    code_name = g_key_file_get_string (ini, "App", "CodeName", NULL);
    vendor = g_key_file_get_string (ini, "App", "Vendor", NULL);
    app->version = g_key_file_get_string (ini, "App", "Version", NULL);

    if (code_name != NULL)
        app->comment = g_steal_pointer (&code_name);
    else if (vendor != NULL)
        app->comment = g_strdup_printf ("%s %s", vendor, app->name);

    sr_app_add_detail (app, _("Vendor"), vendor, FALSE);

    exe_name = g_ascii_strdown (app->name, -1);
    exe_path = g_build_filename (dir, exe_name, NULL);
    if (get_exe_type (exe_path, NULL) != EXE_NONE) {
        app->exec = sr_host_from_sandbox_path (exe_path);
    } else {
        g_autofree char *base = g_path_get_basename (dir);
        Exe *main_exe = pick_main_exe (exes, base, exe_name);

        if (main_exe != NULL)
            app->exec = sr_host_from_sandbox_path (main_exe->path);
    }

    for (int size = 128; size >= 32 && icon == NULL; size /= 2) {
        g_autofree char *file_name = g_strdup_printf ("default%d.png", size);
        g_autofree char *path = g_build_filename (dir, "browser", "chrome", "icons", "default", file_name, NULL);

        if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
            icon = g_steal_pointer (&path);
    }
    if (icon == NULL)
        icon = find_best_icon (dir, exe_name);
    app->icon = file_icon_new (icon);

    app->install_type = g_strdup (_("Mozilla app folder"));

    return g_steal_pointer (&app);
}

static JsonObject *
parse_json_object (JsonParser *parser, const char *data, gssize len)
{
    JsonNode *root;

    if (!json_parser_load_from_data (parser, data, len, NULL))
        return NULL;

    root = json_parser_get_root (parser);
    return root != NULL && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
}

/* An asar archive starts with two Chromium pickles: the size of the
 * header, then the header itself, a JSON tree whose file entries hold an
 * offset relative to the end of the header. */
static char *
read_package_json_from_asar (const char *asar_path, gsize *out_len)
{
    g_autoptr(JsonParser) parser = json_parser_new ();
    guchar prefix[16];
    g_autofree char *header = NULL;
    JsonObject *object, *files, *package;
    guint32 header_size, json_len;
    gint64 size;
    guint64 offset;
    char *data = NULL;
    int fd;

    fd = g_open (asar_path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return NULL;

    if (pread (fd, prefix, sizeof prefix, 0) != sizeof prefix)
        goto out;

    header_size = prefix[4] | prefix[5] << 8 | prefix[6] << 16 | (guint32) prefix[7] << 24;
    json_len = prefix[12] | prefix[13] << 8 | prefix[14] << 16 | (guint32) prefix[15] << 24;
    if (json_len == 0 || json_len > 64 * 1024 * 1024)
        goto out;

    header = g_malloc (json_len);
    if (pread (fd, header, json_len, 16) != (ssize_t) json_len)
        goto out;

    object = parse_json_object (parser, header, json_len);
    if (object == NULL || !json_object_has_member (object, "files"))
        goto out;

    files = json_object_get_object_member (object, "files");
    if (files == NULL || !json_object_has_member (files, "package.json"))
        goto out;

    package = json_object_get_object_member (files, "package.json");
    if (package == NULL || !json_object_has_member (package, "offset"))
        goto out;

    size = json_object_get_int_member_with_default (package, "size", 0);
    offset = g_ascii_strtoull (json_object_get_string_member_with_default (package, "offset", "0"), NULL, 10);
    if (size <= 0 || size > 4 * 1024 * 1024)
        goto out;

    data = g_malloc (size + 1);
    if (pread (fd, data, size, 8 + header_size + offset) != size) {
        g_clear_pointer (&data, g_free);
        goto out;
    }
    data[size] = '\0';
    *out_len = size;

out:
    close (fd);
    return data;
}

static SrApp *
probe_electron (const char *dir, GPtrArray *exes)
{
    g_autofree char *package_json = g_build_filename (dir, "resources", "app", "package.json", NULL);
    g_autofree char *asar = g_build_filename (dir, "resources", "app.asar", NULL);
    g_autoptr(JsonParser) parser = json_parser_new ();
    g_autofree char *contents = NULL;
    g_autofree char *base = g_path_get_basename (dir);
    g_autofree char *icon = NULL;
    g_autofree char *lower = NULL;
    g_autoptr(SrApp) app = NULL;
    const char *product_name, *name, *description;
    JsonObject *object;
    Exe *main_exe;
    gsize len = 0;

    if (!g_file_get_contents (package_json, &contents, &len, NULL))
        contents = read_package_json_from_asar (asar, &len);
    if (contents == NULL)
        return NULL;

    object = parse_json_object (parser, contents, len);
    if (object == NULL)
        return NULL;

    product_name = json_object_get_string_member_with_default (object, "productName", NULL);
    name = json_object_get_string_member_with_default (object, "name", NULL);
    description = json_object_get_string_member_with_default (object, "description", NULL);

    app = sr_app_new ();
    if (product_name != NULL && *product_name != '\0') {
        app->name = g_strdup (product_name);
    } else if (name != NULL && *name != '\0') {
        app->name = g_strdup (name);
        app->name[0] = g_ascii_toupper (app->name[0]);
    } else {
        return NULL;
    }

    if (description != NULL && *description != '\0' && g_ascii_strcasecmp (description, app->name) != 0)
        app->comment = g_strdup (description);
    else
        app->comment = g_strdup (_("Electron app"));

    app->version = g_strdup (json_object_get_string_member_with_default (object, "version", NULL));

    lower = g_ascii_strdown (app->name, -1);
    main_exe = pick_main_exe (exes, base, lower);
    if (main_exe != NULL)
        app->exec = sr_host_from_sandbox_path (main_exe->path);

    icon = find_best_icon (dir, lower);
    app->icon = file_icon_new (icon);
    app->install_type = g_strdup (_("Electron app folder"));

    return g_steal_pointer (&app);
}

static char *
find_readme (const char *dir)
{
    g_autoptr(GDir) gdir = g_dir_open (dir, 0, NULL);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
        if (g_ascii_strncasecmp (name, "readme", 6) == 0)
            return g_build_filename (dir, name, NULL);
    }

    return NULL;
}

static SrApp *
probe_cli_folder (const char *dir, GPtrArray *exes)
{
    g_autoptr(GPtrArray) names = g_ptr_array_new ();
    g_autofree char *base = g_path_get_basename (dir);
    g_autofree char *joined = NULL;
    g_autofree char *readme = NULL;
    SrApp *app;

    for (guint i = 0; i < exes->len; i++) {
        Exe *exe = g_ptr_array_index (exes, i);

        if (!is_helper (exe->name))
            g_ptr_array_add (names, exe->name);
    }

    if (names->len == 0)
        return NULL;

    g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
    g_ptr_array_add (names, NULL);
    joined = g_strjoinv (", ", (char **) names->pdata);

    app = sr_app_new ();
    app->name = g_strdup (base);
    app->comment = g_strdup_printf (_("Command-line tools: %s"), joined);
    app->icon = terminal_icon_new ();
    app->kind = g_strdup (_("CLI"));
    app->install_type = g_strdup (_("Folder of command-line tools"));
    sr_app_add_detail (app, _("Programs"), joined, FALSE);

    readme = find_readme (dir);
    if (readme != NULL) {
        g_autofree char *host_readme = sr_host_from_sandbox_path (readme);
        g_autofree char *contents = NULL;

        sr_app_add_detail (app, _("README"), host_readme, TRUE);
        if (g_file_get_contents (readme, &contents, NULL, NULL)) {
            g_auto(GStrv) lines = g_strsplit (contents, "\n", 40);

            for (guint i = 0; lines[i] != NULL; i++) {
                char *line = g_strstrip (lines[i]);

                while (*line == '#')
                    line++;
                line = g_strstrip (line);
                if (*line != '\0' && g_utf8_validate (line, -1, NULL)) {
                    sr_app_add_detail (app, _("README Summary"), line, FALSE);
                    break;
                }
            }
        }
    }

    return app;
}

SrApp *
sr_bundle_probe_dir (const char *host_dir, GCancellable *cancellable)
{
    g_autofree char *dir = sr_host_to_sandbox_path (host_dir);
    g_autofree char *base = g_path_get_basename (host_dir);
    g_autoptr(GPtrArray) exes = list_executables (dir);
    g_autoptr(SrApp) app = NULL;
    Exe *main_exe;

    app = probe_desktop_file (dir);
    if (app == NULL)
        app = probe_mozilla (dir, exes);
    if (app == NULL)
        app = probe_electron (dir, exes);

    if (app == NULL) {
        main_exe = pick_main_exe (exes, base, NULL);
        if (main_exe != NULL && has_gui_markers (dir, main_exe)) {
            g_autofree char *clean = sr_clean_display_name (base);
            g_autofree char *needle = g_ascii_strdown (clean, -1);
            g_autofree char *icon = find_best_icon (dir, needle);

            app = sr_app_new ();
            app->name = g_steal_pointer (&clean);
            if (!g_ascii_isupper (app->name[0]))
                app->name[0] = g_ascii_toupper (app->name[0]);
            app->comment = g_strdup (_("App folder"));
            app->icon = file_icon_new (icon);
            app->exec = sr_host_from_sandbox_path (main_exe->path);
            app->install_type = g_strdup (_("App folder"));
        } else {
            app = probe_cli_folder (dir, exes);
            if (app == NULL)
                return NULL;
        }
    }

    if (app->kind == NULL)
        app->kind = g_strdup (_("Bundle"));
    if (app->icon == NULL)
        app->icon = g_themed_icon_new ("application-x-executable");

    app->id = g_strdup (host_dir);
    app->source = SR_SOURCE_STANDALONE;
    app->location = g_strdup (host_dir);
    app->standalone_root = g_strdup (host_dir);
    sr_app_add_detail (app, _("Main Program"), app->exec, TRUE);

    return g_steal_pointer (&app);
}

static char *
describe_script (const char *path)
{
    g_autofree char *contents = NULL;
    g_autofree char *interpreter = NULL;
    g_auto(GStrv) argv = NULL;
    char *newline;

    if (!g_file_get_contents (path, &contents, NULL, NULL))
        return g_strdup (_("Script"));

    newline = strchr (contents, '\n');
    if (newline != NULL)
        *newline = '\0';

    if (!g_shell_parse_argv (contents + 2, NULL, &argv, NULL) || argv[0] == NULL)
        return g_strdup (_("Script"));

    interpreter = g_path_get_basename (argv[0]);
    if (g_str_equal (interpreter, "env") && argv[1] != NULL) {
        g_free (interpreter);
        interpreter = g_path_get_basename (argv[1]);
    }

    if (g_str_has_prefix (interpreter, "python"))
        return g_strdup (_("Python script"));
    if (g_str_equal (interpreter, "sh") || g_str_equal (interpreter, "bash") || g_str_equal (interpreter, "zsh")
        || g_str_equal (interpreter, "dash"))
        return g_strdup (_("Shell script"));
    if (g_str_equal (interpreter, "node"))
        return g_strdup (_("Node.js script"));
    if (g_str_has_prefix (interpreter, "perl"))
        return g_strdup (_("Perl script"));
    if (g_str_has_prefix (interpreter, "ruby"))
        return g_strdup (_("Ruby script"));

    return g_strdup_printf (_("%s script"), interpreter);
}

SrApp *
sr_bundle_probe_binary (const char *host_path)
{
    static const char *const go_markers[] = { "Go build ID", "go.buildid", "runtime.goexit", NULL };
    static const char *const rust_markers[] = { "rust_panic", "/rustc/", NULL };
    g_autofree char *path = sr_host_to_sandbox_path (host_path);
    g_autofree char *base = g_path_get_basename (host_path);
    g_autofree char *size = NULL;
    SrApp *app;
    goffset file_size = 0;
    ExeType type;

    type = get_exe_type (path, &file_size);
    if (type == EXE_NONE)
        return NULL;

    app = sr_app_new ();
    app->id = g_strdup (host_path);
    app->name = sr_clean_display_name (base);
    app->source = SR_SOURCE_STANDALONE;
    app->location = g_strdup (host_path);
    app->exec = g_strdup (host_path);
    app->standalone_root = g_strdup (host_path);

    if (type == EXE_ELF) {
        app->kind = g_strdup (_("Binary"));
        app->install_type = g_strdup (_("Standalone binary"));
        app->icon = terminal_icon_new ();
        if (file_mentions_any (path, go_markers))
            app->comment = g_strdup (_("Go program"));
        else if (file_mentions_any (path, rust_markers))
            app->comment = g_strdup (_("Rust program"));
        else
            app->comment = g_strdup (_("Native program"));
    } else {
        app->kind = g_strdup (_("Script"));
        app->install_type = g_strdup (_("Standalone script"));
        app->icon = g_themed_icon_new ("text-x-script");
        app->comment = describe_script (path);
    }

    size = g_format_size (file_size);
    sr_app_add_detail (app, _("Size"), size, FALSE);

    return app;
}

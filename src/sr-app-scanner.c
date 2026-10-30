/* sr-app-scanner.c
 *
 * Copyright 2026 Martins
 *
 * The visibility rules follow g_app_info_should_show() and the
 * OnlyShowIn/NotShowIn handling in GLib's gdesktopappinfo.c, which is what
 * the Apps panel of gnome-control-center relies on. They are reimplemented
 * here because, inside the Flatpak sandbox, GLib only sees the runtime's
 * desktop files and resolves TryExec against the sandbox PATH.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-app-scanner"

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "sr-app-scanner.h"
#include "sr-host.h"
#include "sr-utils.h"

typedef enum {
    DIR_KIND_PLAIN,
    DIR_KIND_USER,
    DIR_KIND_FLATPAK_USER,
    DIR_KIND_FLATPAK_SYSTEM,
    DIR_KIND_LOCAL,
    DIR_KIND_SNAP,
} DirKind;

typedef struct {
    char *id;
    char *host_path;
    DirKind kind;
    GPtrArray *shadowed;
} Entry;

static void
entry_free (Entry *entry)
{
    g_free (entry->id);
    g_free (entry->host_path);
    g_clear_pointer (&entry->shadowed, g_ptr_array_unref);
    g_free (entry);
}

typedef struct {
    char *host_path;
    DirKind kind;
} AppDir;

static void
app_dir_free (AppDir *dir)
{
    g_free (dir->host_path);
    g_free (dir);
}

static void
add_dir (GPtrArray *dirs, char *host_path, DirKind kind)
{
    AppDir *dir = g_new0 (AppDir, 1);

    dir->host_path = host_path;
    dir->kind = kind;
    g_ptr_array_add (dirs, dir);
}

/* Host directories in XDG precedence order: an earlier directory wins
 * when two files have the same desktop ID. */
static GPtrArray *
get_dirs (void)
{
    GPtrArray *dirs = g_ptr_array_new_with_free_func ((GDestroyNotify) app_dir_free);
    const char *home = g_get_home_dir ();

    add_dir (dirs, g_build_filename (home, ".local", "share", "applications", NULL), DIR_KIND_USER);
    add_dir (dirs, g_build_filename (home, ".local", "share", "flatpak", "exports", "share", "applications", NULL),
             DIR_KIND_FLATPAK_USER);
    add_dir (dirs, g_strdup ("/var/lib/flatpak/exports/share/applications"), DIR_KIND_FLATPAK_SYSTEM);
    add_dir (dirs, g_strdup ("/usr/local/share/applications"), DIR_KIND_LOCAL);
    add_dir (dirs, g_strdup ("/usr/share/applications"), DIR_KIND_PLAIN);
    add_dir (dirs, g_strdup ("/var/lib/snapd/desktop/applications"), DIR_KIND_SNAP);

    return dirs;
}

GStrv
sr_app_scanner_get_dir_paths (void)
{
    g_autoptr(GPtrArray) dirs = get_dirs ();
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();

    for (guint i = 0; i < dirs->len; i++)
        g_strv_builder_add (builder, ((AppDir *) g_ptr_array_index (dirs, i))->host_path);

    return g_strv_builder_end (builder);
}

static void
collect_entries (const char *host_dir, const char *id_prefix, DirKind kind, GHashTable *seen, GPtrArray *entries,
                 int depth)
{
    g_autofree char *sandbox_dir = sr_host_to_sandbox_path (host_dir);
    g_autoptr(GDir) dir = NULL;
    const char *name;

    if (depth > 4)
        return;

    dir = g_dir_open (sandbox_dir, 0, NULL);
    if (dir == NULL)
        return;

    while ((name = g_dir_read_name (dir)) != NULL) {
        g_autofree char *host_path = g_build_filename (host_dir, name, NULL);
        g_autofree char *sandbox_path = g_build_filename (sandbox_dir, name, NULL);

        if (g_file_test (sandbox_path, G_FILE_TEST_IS_DIR)) {
            g_autofree char *prefix = g_strconcat (id_prefix, name, "-", NULL);

            collect_entries (host_path, prefix, kind, seen, entries, depth + 1);
            continue;
        }

        if (g_str_has_suffix (name, ".desktop")) {
            g_autofree char *id = g_strconcat (id_prefix, name, NULL);
            Entry *entry = g_hash_table_lookup (seen, id);

            if (entry != NULL) {
                if (entry->shadowed == NULL)
                    entry->shadowed = g_ptr_array_new_with_free_func (g_free);
                g_ptr_array_add (entry->shadowed, g_steal_pointer (&host_path));
                continue;
            }

            entry = g_new0 (Entry, 1);
            entry->id = g_steal_pointer (&id);
            entry->host_path = g_steal_pointer (&host_path);
            entry->kind = kind;
            g_hash_table_insert (seen, entry->id, entry);
            g_ptr_array_add (entries, entry);
        }
    }
}

static gboolean
strv_contains_ci (char **strv, const char *str)
{
    for (guint i = 0; strv != NULL && strv[i] != NULL; i++) {
        if (g_ascii_strcasecmp (strv[i], str) == 0)
            return TRUE;
    }

    return FALSE;
}

static gboolean
should_show (GKeyFile *keyfile)
{
    g_autofree char *type = NULL;
    g_autofree char *try_exec = NULL;
    g_auto(GStrv) only_show_in = NULL;
    g_auto(GStrv) not_show_in = NULL;
    const char *const *desktops = sr_host_get_desktop_names ();

    type = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
    if (g_strcmp0 (type, G_KEY_FILE_DESKTOP_TYPE_APPLICATION) != 0)
        return FALSE;

    if (g_key_file_get_boolean (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL))
        return FALSE;

    if (g_key_file_get_boolean (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL))
        return FALSE;

    only_show_in
        = g_key_file_get_string_list (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ONLY_SHOW_IN, NULL, NULL);
    not_show_in
        = g_key_file_get_string_list (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NOT_SHOW_IN, NULL, NULL);

    for (guint i = 0; desktops[i] != NULL; i++) {
        if (strv_contains_ci (only_show_in, desktops[i]))
            goto shown_in_desktop;
        if (strv_contains_ci (not_show_in, desktops[i]))
            return FALSE;
    }

    if (only_show_in != NULL)
        return FALSE;

shown_in_desktop:
    try_exec = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, NULL);
    if (try_exec != NULL && *try_exec != '\0') {
        g_autofree char *found = sr_host_find_program (try_exec);

        if (found == NULL)
            return FALSE;
    }

    return TRUE;
}

GIcon *
sr_icon_from_desktop_value (const char *value)
{
    g_autofree char *name = NULL;

    if (value == NULL || *value == '\0')
        return NULL;

    if (g_path_is_absolute (value)) {
        g_autofree char *path = sr_host_to_sandbox_path (value);
        g_autoptr(GFile) file = NULL;

        if (!g_file_test (path, G_FILE_TEST_EXISTS))
            return NULL;

        file = g_file_new_for_path (path);
        return g_file_icon_new (file);
    }

    name = g_strdup (value);
    if (g_str_has_suffix (name, ".png") || g_str_has_suffix (name, ".svg") || g_str_has_suffix (name, ".xpm"))
        name[strlen (name) - 4] = '\0';

    return g_themed_icon_new (name);
}

char *
sr_exec_get_program (const char *exec)
{
    g_auto(GStrv) argv = NULL;
    guint i = 0;

    if (exec == NULL || !g_shell_parse_argv (exec, NULL, &argv, NULL))
        return NULL;

    if (argv[0] != NULL && (g_str_equal (argv[0], "env") || g_str_equal (argv[0], "/usr/bin/env"))) {
        for (i = 1; argv[i] != NULL; i++) {
            if (argv[i][0] != '-' && strchr (argv[i], '=') == NULL)
                break;
        }
    }

    return g_strdup (argv[i]);
}

static char *
read_metainfo_version (const char *active_dir, const char *app_id)
{
    const char *names[] = { "metainfo", "appdata" };

    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        g_autofree char *file_name = g_strdup_printf ("%s.%s.xml", app_id, names[i]);
        g_autofree char *path = g_build_filename (active_dir, "files", "share", names[i], file_name, NULL);
        g_autofree char *contents = NULL;
        const char *release, *version, *end;

        if (!g_file_get_contents (path, &contents, NULL, NULL))
            continue;

        release = strstr (contents, "<release ");
        if (release == NULL)
            continue;
        version = strstr (release, "version=\"");
        if (version == NULL)
            continue;
        version += strlen ("version=\"");
        end = strchr (version, '"');
        if (end == NULL)
            continue;

        return g_strndup (version, end - version);
    }

    return NULL;
}

static void
classify_flatpak (SrApp *app, GKeyFile *keyfile, DirKind kind)
{
    g_autofree char *user_base = g_build_filename (g_get_home_dir (), ".local", "share", "flatpak", NULL);
    g_autofree char *app_id = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL);
    g_autofree char *app_dir = NULL;
    g_autofree char *current_link = NULL;
    g_autofree char *current = NULL;
    g_autofree char *active = NULL;
    g_autofree char *active_dir = NULL;
    g_autofree char *metadata_path = NULL;
    g_autoptr(GKeyFile) metadata = g_key_file_new ();
    gboolean user;

    if (app_id == NULL) {
        app_id = g_strdup (app->id);
        if (g_str_has_suffix (app_id, ".desktop"))
            app_id[strlen (app_id) - strlen (".desktop")] = '\0';
    }

    if (kind == DIR_KIND_FLATPAK_USER) {
        user = TRUE;
    } else if (kind == DIR_KIND_FLATPAK_SYSTEM) {
        user = FALSE;
    } else {
        g_autofree char *probe = g_build_filename (user_base, "app", app_id, NULL);
        user = g_file_test (probe, G_FILE_TEST_IS_DIR);
    }

    app_dir = g_build_filename (user ? user_base : "/var/lib/flatpak", "app", app_id, NULL);
    current_link = g_build_filename (app_dir, "current", NULL);
    current = g_file_read_link (current_link, NULL);
    active = g_build_filename (current_link, "active", NULL);
    active_dir = sr_realpath (active);

    app->source = SR_SOURCE_FLATPAK;
    app->install_type = g_strdup (user ? _("Flatpak (user installation)") : _("Flatpak (system installation)"));
    app->location = g_strdup (active_dir != NULL ? active_dir : app_dir);
    g_free (app->portal_app_id);
    app->portal_app_id = g_strdup (app_id);

    if (active_dir != NULL)
        app->version = read_metainfo_version (active_dir, app_id);

    if (current != NULL) {
        g_autofree char *ref = g_strdup_printf ("app/%s/%s", app_id, current);
        sr_app_add_detail (app, _("Ref"), ref, FALSE);
    }

    metadata_path = g_build_filename (active_dir != NULL ? active_dir : app_dir, "metadata", NULL);
    if (g_key_file_load_from_file (metadata, metadata_path, G_KEY_FILE_NONE, NULL)) {
        g_autofree char *runtime = g_key_file_get_string (metadata, "Application", "runtime", NULL);
        sr_app_add_detail (app, _("Runtime"), runtime, FALSE);
    }

    sr_app_add_detail (app, _("Installation"), user ? user_base : "/var/lib/flatpak", TRUE);
}

static void
classify_snap (SrApp *app, GKeyFile *keyfile)
{
    g_autofree char *snap_name = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-SnapInstanceName", NULL);
    g_autofree char *snap_dir = NULL;
    g_autofree char *current_link = NULL;
    g_autofree char *revision = NULL;
    g_autofree char *yaml_path = NULL;
    g_autofree char *yaml = NULL;

    if (snap_name == NULL) {
        const char *underscore = strchr (app->id, '_');
        snap_name = underscore != NULL ? g_strndup (app->id, underscore - app->id) : g_strdup (app->id);
    }

    snap_dir = g_build_filename ("/snap", snap_name, NULL);
    current_link = g_build_filename (snap_dir, "current", NULL);
    revision = g_file_read_link (current_link, NULL);

    app->source = SR_SOURCE_SNAP;
    app->install_type = g_strdup (_("Snap"));
    if (revision != NULL)
        app->location = g_build_filename (snap_dir, revision, NULL);
    else
        app->location = g_strdup (snap_dir);

    g_free (app->portal_app_id);
    app->portal_app_id = g_strdup_printf ("snap.%s", snap_name);

    sr_app_add_detail (app, _("Snap Name"), snap_name, FALSE);
    sr_app_add_detail (app, _("Revision"), revision, FALSE);

    yaml_path = g_build_filename (current_link, "meta", "snap.yaml", NULL);
    if (g_file_get_contents (yaml_path, &yaml, NULL, NULL)) {
        g_autofree char *confinement = sr_yaml_get_field (yaml, "confinement");
        g_autofree char *base = sr_yaml_get_field (yaml, "base");

        app->version = sr_yaml_get_field (yaml, "version");
        sr_app_add_detail (app, _("Confinement"), confinement, FALSE);
        sr_app_add_detail (app, _("Base"), base, FALSE);
    }
}

static void
classify_deb (SrApp *app, SrPackage *package, const char *program_path)
{
    g_autofree char *size = NULL;

    app->source = SR_SOURCE_DEB;
    app->install_type = g_strdup (_("Debian package"));
    app->package = g_strdup (package->name);
    app->version = g_strdup (package->version);
    app->location = g_strdup (program_path != NULL ? program_path
                                                   : (package->location != NULL ? package->location : app->desktop_file));

    sr_app_add_detail (app, _("Package"), package->name, FALSE);
    sr_app_add_detail (app, _("Architecture"), package->arch, FALSE);
    if (package->installed_size_kib > 0) {
        size = g_format_size (package->installed_size_kib * 1024);
        sr_app_add_detail (app, _("Installed Size"), size, FALSE);
    }
    sr_app_add_detail (app, _("Installed"),
                       package->auto_installed ? _("Automatically, as a dependency") : _("Manually"), FALSE);

    g_ptr_array_add (package->app_names, g_strdup (app->name));
    if (package->icon == NULL && app->icon != NULL)
        package->icon = g_object_ref (app->icon);
}

static char *
find_standalone_root (const char *path, const char *const *standalone_dirs)
{
    for (guint i = 0; path != NULL && standalone_dirs != NULL && standalone_dirs[i] != NULL; i++) {
        const char *dir = standalone_dirs[i];
        gsize len = strlen (dir);
        const char *rest, *slash;

        if (!sr_file_has_prefix_dir (path, dir) || path[len] != '/')
            continue;

        rest = path + len + 1;
        slash = strchr (rest, '/');
        return slash != NULL ? g_strndup (path, slash - path) : g_strdup (path);
    }

    return NULL;
}

static void
classify_other (SrApp *app, const char *program_path, DirKind kind)
{
    app->source = SR_SOURCE_OTHER;
    app->location = g_strdup (program_path != NULL ? program_path : app->desktop_file);

    if (app->exec != NULL && (strstr (app->exec, "--app-id=") != NULL || strstr (app->exec, "--app=") != NULL))
        app->kind = g_strdup (_("Web App"));
    else if (app->exec != NULL && strstr (app->exec, "steam://") != NULL)
        app->kind = g_strdup (_("Steam"));
    else if (app->exec != NULL && (strstr (app->exec, "WINEPREFIX") != NULL || strstr (app->exec, "wine ") != NULL))
        app->kind = g_strdup (_("Wine"));

    if (kind == DIR_KIND_USER)
        app->install_type = g_strdup (_("Desktop entry in your home folder"));
    else if (kind == DIR_KIND_LOCAL)
        app->install_type = g_strdup (_("Installed locally in /usr/local"));
    else
        app->install_type = g_strdup (_("Not owned by any package"));
}

static SrApp *
build_app (Entry *entry, SrDpkgIndex *dpkg, const char *const *standalone_dirs)
{
    g_autoptr(SrApp) app = NULL;
    g_autoptr(GKeyFile) keyfile = g_key_file_new ();
    g_autofree char *sandbox_path = sr_host_to_sandbox_path (entry->host_path);
    g_autofree char *icon = NULL;
    g_autofree char *categories = NULL;
    g_autofree char *program = NULL;
    g_autofree char *program_path = NULL;
    g_autofree char *standalone_root = NULL;
    SrPackage *package = NULL;

    if (!g_key_file_load_from_file (keyfile, sandbox_path, G_KEY_FILE_NONE, NULL))
        return NULL;

    if (!should_show (keyfile))
        return NULL;

    app = sr_app_new ();
    app->id = g_strdup (entry->id);
    app->name = g_key_file_get_locale_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
    if (app->name == NULL)
        return NULL;

    app->comment
        = g_key_file_get_locale_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL, NULL);
    if (app->comment == NULL)
        app->comment = g_key_file_get_locale_string (keyfile, G_KEY_FILE_DESKTOP_GROUP,
                                                     G_KEY_FILE_DESKTOP_KEY_GENERIC_NAME, NULL, NULL);

    icon = g_key_file_get_locale_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL, NULL);
    app->icon = sr_icon_from_desktop_value (icon);
    app->exec = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
    categories = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_CATEGORIES, NULL);
    app->categories = sr_join_categories (categories);
    app->desktop_file = g_strdup (entry->host_path);
    app->portal_app_id = sr_get_portal_app_id (keyfile, entry->id);

    program = sr_exec_get_program (app->exec);
    program_path = sr_host_find_program (program);

    if (entry->kind == DIR_KIND_FLATPAK_USER || entry->kind == DIR_KIND_FLATPAK_SYSTEM
        || g_key_file_has_key (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL)) {
        classify_flatpak (app, keyfile, entry->kind);
        return g_steal_pointer (&app);
    }

    if (entry->kind == DIR_KIND_SNAP
        || g_key_file_has_key (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-SnapInstanceName", NULL)) {
        classify_snap (app, keyfile);
        return g_steal_pointer (&app);
    }

    package = sr_dpkg_index_get_owner (dpkg, entry->host_path);
    for (guint i = 0; package == NULL && entry->shadowed != NULL && i < entry->shadowed->len; i++) {
        const char *shadowed = g_ptr_array_index (entry->shadowed, i);

        package = sr_dpkg_index_get_owner (dpkg, shadowed);
        if (package != NULL)
            app->desktop_file_note = g_strdup_printf (_("Your copy overrides %s"), shadowed);
    }
    if (package != NULL) {
        classify_deb (app, package, program_path);
        return g_steal_pointer (&app);
    }

    standalone_root = find_standalone_root (program_path, standalone_dirs);
    if (standalone_root == NULL && program != NULL && g_path_is_absolute (program))
        standalone_root = find_standalone_root (program, standalone_dirs);
    if (standalone_root == NULL) {
        g_autofree char *work_dir = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, "Path", NULL);
        standalone_root = find_standalone_root (work_dir, standalone_dirs);
    }
    if (standalone_root != NULL) {
        app->source = SR_SOURCE_STANDALONE;
        app->install_type = g_strdup (_("Standalone app"));
        app->location = g_strdup (standalone_root);
        app->standalone_root = g_steal_pointer (&standalone_root);
        return g_steal_pointer (&app);
    }

    classify_other (app, program_path, entry->kind);
    return g_steal_pointer (&app);
}

GPtrArray *
sr_app_scanner_scan (SrDpkgIndex *dpkg, const char *const *standalone_dirs, GCancellable *cancellable)
{
    g_autoptr(GPtrArray) dirs = get_dirs ();
    g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func ((GDestroyNotify) entry_free);
    g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
    GPtrArray *apps = g_ptr_array_new_with_free_func (g_object_unref);

    for (guint i = 0; i < dirs->len; i++) {
        AppDir *dir = g_ptr_array_index (dirs, i);
        collect_entries (dir->host_path, "", dir->kind, seen, entries, 0);
    }

    for (guint i = 0; i < entries->len; i++) {
        SrApp *app;

        if (g_cancellable_is_cancelled (cancellable))
            break;

        app = build_app (g_ptr_array_index (entries, i), dpkg, standalone_dirs);
        if (app == NULL)
            continue;

        if (app->exec != NULL)
            sr_app_add_detail (app, _("Command"), app->exec, FALSE);
        g_ptr_array_add (apps, app);
    }

    return apps;
}

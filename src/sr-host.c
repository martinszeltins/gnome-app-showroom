/* sr-host.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-host"

#include "config.h"

#include <string.h>
#include <sys/stat.h>

#include "sr-host.h"

#define HOST_PREFIX "/run/host"

/* Host directories that Flatpak exposes under /run/host with
 * --filesystem=host-os and --filesystem=host-etc. */
static const char *const host_os_dirs[] = {
    "/usr", "/etc", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32",
};

gboolean
sr_host_is_sandboxed (void)
{
    static gsize sandboxed = 0;

    if (g_once_init_enter (&sandboxed)) {
        gsize value = g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS) ? 2 : 1;
        g_once_init_leave (&sandboxed, value);
    }

    return sandboxed == 2;
}

static gboolean
is_host_os_path (const char *path)
{
    for (guint i = 0; i < G_N_ELEMENTS (host_os_dirs); i++) {
        gsize len = strlen (host_os_dirs[i]);

        if (strncmp (path, host_os_dirs[i], len) == 0 && (path[len] == '/' || path[len] == '\0'))
            return TRUE;
    }

    return FALSE;
}

char *
sr_host_to_sandbox_path (const char *host_path)
{
    if (host_path == NULL)
        return NULL;

    if (sr_host_is_sandboxed () && host_path[0] == '/' && is_host_os_path (host_path))
        return g_strconcat (HOST_PREFIX, host_path, NULL);

    return g_strdup (host_path);
}

char *
sr_host_from_sandbox_path (const char *path)
{
    if (path == NULL)
        return NULL;

    if (g_str_has_prefix (path, HOST_PREFIX "/"))
        return g_strdup (path + strlen (HOST_PREFIX));

    return g_strdup (path);
}

char *
sr_host_expand_home (const char *path)
{
    if (path == NULL)
        return NULL;

    if (g_str_equal (path, "~"))
        return g_strdup (g_get_home_dir ());

    if (g_str_has_prefix (path, "~/"))
        return g_build_filename (g_get_home_dir (), path + 2, NULL);

    return g_strdup (path);
}

gboolean
sr_host_path_exists (const char *host_path)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);
    struct stat st;

    /* lstat, because host symlinks such as /snap/bin/foo -> /usr/bin/snap
     * dangle inside the sandbox. */
    return path != NULL && lstat (path, &st) == 0;
}

gboolean
sr_host_path_is_dir (const char *host_path)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);

    return path != NULL && g_file_test (path, G_FILE_TEST_IS_DIR);
}

char *
sr_host_find_program (const char *program)
{
    g_autofree char *local_bin = NULL;
    g_autofree char *user_flatpak_bin = NULL;
    const char *dirs[] = {
        "/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin",
        "/usr/games", "/usr/local/games", "/snap/bin", "/var/lib/flatpak/exports/bin",
        NULL, NULL, NULL,
    };

    if (program == NULL || *program == '\0')
        return NULL;

    if (strchr (program, '/') != NULL) {
        if (program[0] == '/' && sr_host_path_exists (program))
            return g_strdup (program);
        return NULL;
    }

    local_bin = g_build_filename (g_get_home_dir (), ".local", "bin", NULL);
    user_flatpak_bin = g_build_filename (g_get_home_dir (), ".local", "share", "flatpak", "exports", "bin", NULL);
    dirs[G_N_ELEMENTS (dirs) - 3] = local_bin;
    dirs[G_N_ELEMENTS (dirs) - 2] = user_flatpak_bin;

    for (guint i = 0; dirs[i] != NULL; i++) {
        g_autofree char *candidate = g_build_filename (dirs[i], program, NULL);

        if (sr_host_path_exists (candidate))
            return g_steal_pointer (&candidate);
    }

    return NULL;
}

const char *const *
sr_host_get_desktop_names (void)
{
    static char **names = NULL;

    if (g_once_init_enter_pointer (&names)) {
        const char *env = g_getenv ("XDG_CURRENT_DESKTOP");
        char **value;

        if (env == NULL || *env == '\0')
            env = "GNOME";
        value = g_strsplit (env, ":", -1);
        g_once_init_leave_pointer (&names, value);
    }

    return (const char *const *) names;
}

void
sr_host_setup_icon_theme (GdkDisplay *display)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
    g_autofree char *user_icons = g_build_filename (g_get_home_dir (), ".local", "share", "icons", NULL);
    g_autofree char *user_flatpak_icons
        = g_build_filename (g_get_home_dir (), ".local", "share", "flatpak", "exports", "share", "icons", NULL);
    const char *paths[] = {
        user_icons,
        user_flatpak_icons,
        "/var/lib/flatpak/exports/share/icons",
        "/usr/local/share/icons",
        "/usr/share/icons",
        "/usr/share/pixmaps",
    };

    if (!sr_host_is_sandboxed ())
        return;

    for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
        g_autofree char *path = sr_host_to_sandbox_path (paths[i]);

        if (g_file_test (path, G_FILE_TEST_IS_DIR))
            gtk_icon_theme_add_search_path (theme, path);
    }
}

static void
subprocess_done_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error))
        g_warning ("Failed to open %s: %s", (const char *) user_data, error->message);

    g_free (user_data);
}

static void
launch_default_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    if (!g_app_info_launch_default_for_uri_finish (result, &error))
        g_warning ("Failed to open %s: %s", (const char *) user_data, error->message);

    g_free (user_data);
}

void
sr_host_open_file (const char *host_path)
{
    g_autoptr(GError) error = NULL;

    if (host_path == NULL)
        return;

    if (sr_host_is_sandboxed ()) {
        g_autoptr(GSubprocess) proc = NULL;

        proc = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, "flatpak-spawn", "--host", "xdg-open", host_path,
                                 NULL);
        if (proc == NULL) {
            g_warning ("Failed to run xdg-open on the host: %s", error->message);
            return;
        }
        g_subprocess_wait_check_async (proc, NULL, subprocess_done_cb, g_strdup (host_path));
    } else {
        g_autofree char *uri = g_filename_to_uri (host_path, NULL, NULL);

        if (uri != NULL)
            g_app_info_launch_default_for_uri_async (uri, NULL, NULL, launch_default_cb, g_strdup (host_path));
    }
}

typedef struct {
    char *host_path;
    gboolean is_dir;
} ShowData;

static void
show_data_free (ShowData *data)
{
    g_free (data->host_path);
    g_free (data);
}

static void
file_manager_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
    ShowData *data = user_data;
    g_autoptr(GVariant) ret = NULL;
    g_autoptr(GError) error = NULL;

    ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
    if (ret == NULL) {
        g_autofree char *folder = NULL;

        g_debug ("FileManager1 failed, falling back to xdg-open: %s", error->message);
        folder = data->is_dir ? g_strdup (data->host_path) : g_path_get_dirname (data->host_path);
        sr_host_open_file (folder);
    }

    show_data_free (data);
}

void
sr_host_show_location (const char *host_path)
{
    g_autoptr(GDBusConnection) bus = NULL;
    g_autofree char *uri = NULL;
    const char *uris[2] = { NULL, NULL };
    ShowData *data;

    if (host_path == NULL)
        return;

    data = g_new0 (ShowData, 1);
    data->host_path = g_strdup (host_path);
    data->is_dir = sr_host_path_is_dir (host_path);

    bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    uri = g_filename_to_uri (host_path, NULL, NULL);
    if (bus == NULL || uri == NULL) {
        g_autofree char *folder = data->is_dir ? g_strdup (host_path) : g_path_get_dirname (host_path);

        sr_host_open_file (folder);
        show_data_free (data);
        return;
    }

    uris[0] = uri;
    g_dbus_connection_call (bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                            "org.freedesktop.FileManager1", data->is_dir ? "ShowFolders" : "ShowItems",
                            g_variant_new ("(^ass)", uris, ""), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            file_manager_cb, data);
}

/* sr-appimage.c
 *
 * Copyright 2026 Martins
 *
 * Reads the embedded desktop file and icon of AppImages without running
 * them. Type 2 AppImages are an ELF runtime followed by a SquashFS image
 * that starts right after the ELF section headers; they are read with
 * unsquashfs. Type 1 AppImages are ISO 9660 images and are read with
 * libarchive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-appimage"

#include "config.h"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sr-appimage.h"
#include "sr-host.h"
#include "sr-utils.h"

#define MAX_ENTRY_SIZE (8 * 1024 * 1024)
#define MAX_LINK_HOPS 4
#define CACHE_GROUP "AppImage"

typedef struct {
    gboolean is_link;
    gboolean is_hardlink;
    char *target;
    GBytes *data;
} AiEntry;

static void
ai_entry_free (AiEntry *entry)
{
    g_free (entry->target);
    g_clear_pointer (&entry->data, g_bytes_unref);
    g_free (entry);
}

typedef struct {
    int type;
    char *path;
    guint64 offset;
    char *tmp_root;
    guint runs;
    GHashTable *entries;
} AiReader;

static guint64
read_uint (const guchar *p, guint size, gboolean little_endian)
{
    guint64 value = 0;

    for (guint i = 0; i < size; i++) {
        guint shift = little_endian ? i * 8 : (size - 1 - i) * 8;
        value |= ((guint64) p[i]) << shift;
    }

    return value;
}

static gboolean
read_at (int fd, guint64 offset, guchar *buf, gsize len)
{
    return pread (fd, buf, len, (off_t) offset) == (ssize_t) len;
}

static guint64
find_squashfs_offset (const char *path)
{
    guchar header[64];
    guchar magic[4];
    gboolean le;
    guint64 shoff, shentsize, shnum, offset;
    int fd;

    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return 0;

    if (!read_at (fd, 0, header, sizeof header) || memcmp (header, "\177ELF", 4) != 0) {
        close (fd);
        return 0;
    }

    le = header[5] == 1;
    if (header[4] == 2) {
        shoff = read_uint (header + 0x28, 8, le);
        shentsize = read_uint (header + 0x3A, 2, le);
        shnum = read_uint (header + 0x3C, 2, le);
    } else {
        shoff = read_uint (header + 0x20, 4, le);
        shentsize = read_uint (header + 0x2E, 2, le);
        shnum = read_uint (header + 0x30, 2, le);
    }

    offset = shoff + shentsize * shnum;
    if (!read_at (fd, offset, magic, sizeof magic) || memcmp (magic, "hsqs", 4) != 0)
        offset = 0;

    close (fd);
    return offset;
}

gboolean
sr_appimage_detect (const char *path, int *type)
{
    guchar header[16];
    g_autofree char *lower = NULL;
    int fd;
    gboolean is_elf;

    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return FALSE;

    is_elf = read_at (fd, 0, header, sizeof header) && memcmp (header, "\177ELF", 4) == 0;
    close (fd);

    if (!is_elf)
        return FALSE;

    if (header[8] == 'A' && header[9] == 'I' && (header[10] == 1 || header[10] == 2)) {
        if (type)
            *type = header[10];
        return TRUE;
    }

    lower = g_ascii_strdown (path, -1);
    if (g_str_has_suffix (lower, ".appimage")) {
        if (type)
            *type = 0;
        return TRUE;
    }

    return FALSE;
}

static void
rm_rf (const char *path)
{
    struct stat st;

    if (lstat (path, &st) != 0)
        return;

    if (S_ISDIR (st.st_mode)) {
        g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
        const char *name;

        while (dir != NULL && (name = g_dir_read_name (dir)) != NULL) {
            g_autofree char *child = g_build_filename (path, name, NULL);
            rm_rf (child);
        }
        g_rmdir (path);
    } else {
        g_unlink (path);
    }
}

static char *
normalize_rel (const char *path)
{
    g_auto(GStrv) parts = NULL;
    g_autoptr(GPtrArray) stack = g_ptr_array_new ();

    if (path == NULL)
        return NULL;

    parts = g_strsplit (path, "/", -1);
    for (guint i = 0; parts[i] != NULL; i++) {
        if (*parts[i] == '\0' || g_str_equal (parts[i], "."))
            continue;
        if (g_str_equal (parts[i], "..")) {
            if (stack->len == 0)
                return NULL;
            g_ptr_array_remove_index (stack, stack->len - 1);
            continue;
        }
        g_ptr_array_add (stack, parts[i]);
    }
    g_ptr_array_add (stack, NULL);

    return g_strjoinv ("/", (char **) stack->pdata);
}

static char *
resolve_link (const char *link_rel, const char *target)
{
    g_autofree char *dir = NULL;
    g_autofree char *joined = NULL;

    if (target[0] == '/')
        return normalize_rel (target);

    dir = g_path_get_dirname (link_rel);
    joined = g_strconcat (g_str_equal (dir, ".") ? "" : dir, "/", target, NULL);

    return normalize_rel (joined);
}

static gboolean
matches_any (const char *const *patterns, const char *rel)
{
    for (guint i = 0; patterns[i] != NULL; i++) {
        if (fnmatch (patterns[i], rel, FNM_PATHNAME) == 0)
            return TRUE;
    }

    return FALSE;
}

static void
collect_extracted (AiReader *reader, const char *dir, const char *prefix)
{
    g_autoptr(GDir) gdir = g_dir_open (dir, 0, NULL);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
        g_autofree char *full = g_build_filename (dir, name, NULL);
        g_autofree char *rel = *prefix ? g_strconcat (prefix, "/", name, NULL) : g_strdup (name);
        struct stat st;
        AiEntry *entry;

        if (lstat (full, &st) != 0)
            continue;

        if (S_ISDIR (st.st_mode)) {
            collect_extracted (reader, full, rel);
            continue;
        }

        entry = g_new0 (AiEntry, 1);
        if (S_ISLNK (st.st_mode)) {
            entry->is_link = TRUE;
            entry->target = g_file_read_link (full, NULL);
        } else if (S_ISREG (st.st_mode) && st.st_size <= MAX_ENTRY_SIZE) {
            char *contents = NULL;
            gsize len = 0;

            if (g_file_get_contents (full, &contents, &len, NULL))
                entry->data = g_bytes_new_take (contents, len);
        }

        if (entry->is_link && entry->target == NULL) {
            ai_entry_free (entry);
            continue;
        }

        g_hash_table_replace (reader->entries, g_steal_pointer (&rel), entry);
    }
}

static void
fetch_squashfs (AiReader *reader, const char *const *patterns, GCancellable *cancellable)
{
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    g_autofree char *offset = g_strdup_printf ("%" G_GUINT64_FORMAT, reader->offset);
    g_autofree char *dest = g_strdup_printf ("%s/run-%u", reader->tmp_root, reader->runs++);

    g_strv_builder_add_many (builder, "unsquashfs", "-o", offset, "-d", dest, "-no-xattrs", "-no-progress",
                             reader->path, NULL);
    g_strv_builder_addv (builder, (const char **) patterns);
    argv = g_strv_builder_end (builder);

    proc = g_subprocess_newv ((const char *const *) argv,
                              G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL) {
        g_warning ("Could not run unsquashfs: %s", error->message);
        return;
    }

    if (!g_subprocess_wait (proc, cancellable, &error)) {
        g_subprocess_force_exit (proc);
        return;
    }

    collect_extracted (reader, dest, "");
}

static void
fetch_iso (AiReader *reader, const char *const *patterns)
{
    struct archive *archive = archive_read_new ();
    struct archive_entry *ae;

    archive_read_support_format_iso9660 (archive);
    if (archive_read_open_filename (archive, reader->path, 64 * 1024) != ARCHIVE_OK) {
        g_debug ("libarchive could not open %s: %s", reader->path, archive_error_string (archive));
        archive_read_free (archive);
        return;
    }

    while (archive_read_next_header (archive, &ae) == ARCHIVE_OK) {
        g_autofree char *rel = normalize_rel (archive_entry_pathname (ae));
        const char *hardlink = archive_entry_hardlink (ae);
        AiEntry *entry;

        if (rel == NULL || *rel == '\0' || !matches_any (patterns, rel)) {
            archive_read_data_skip (archive);
            continue;
        }

        entry = g_new0 (AiEntry, 1);
        if (hardlink != NULL) {
            entry->is_link = TRUE;
            entry->is_hardlink = TRUE;
            entry->target = normalize_rel (hardlink);
        } else if (archive_entry_filetype (ae) == AE_IFLNK) {
            entry->is_link = TRUE;
            entry->target = g_strdup (archive_entry_symlink (ae));
        } else if (archive_entry_filetype (ae) == AE_IFREG && archive_entry_size (ae) <= MAX_ENTRY_SIZE) {
            gsize size = archive_entry_size (ae);
            guchar *buf = g_malloc (size + 1);
            gsize total = 0;

            while (total < size) {
                la_ssize_t n = archive_read_data (archive, buf + total, size - total);
                if (n <= 0)
                    break;
                total += n;
            }
            buf[total] = '\0';
            entry->data = g_bytes_new_take (buf, total);
        }

        if (entry->is_link && entry->target == NULL) {
            ai_entry_free (entry);
            continue;
        }

        g_hash_table_replace (reader->entries, g_steal_pointer (&rel), entry);
    }

    archive_read_free (archive);
}

static void
ai_fetch (AiReader *reader, const char *const *patterns, GCancellable *cancellable)
{
    if (reader->type == 2)
        fetch_squashfs (reader, patterns, cancellable);
    else
        fetch_iso (reader, patterns);
}

static AiEntry *
ai_get (AiReader *reader, const char *rel, GCancellable *cancellable)
{
    g_autofree char *current = g_strdup (rel);

    for (guint hops = 0; hops <= MAX_LINK_HOPS && current != NULL; hops++) {
        AiEntry *entry = g_hash_table_lookup (reader->entries, current);
        char *next;

        if (entry == NULL) {
            const char *patterns[] = { current, NULL };

            ai_fetch (reader, patterns, cancellable);
            entry = g_hash_table_lookup (reader->entries, current);
        }

        if (entry == NULL)
            return NULL;

        if (!entry->is_link)
            return entry->data != NULL ? entry : NULL;

        next = entry->is_hardlink ? g_strdup (entry->target) : resolve_link (current, entry->target);
        g_free (current);
        current = next;
    }

    return NULL;
}

static const char *
image_extension (GBytes *data)
{
    gsize len;
    const char *bytes = g_bytes_get_data (data, &len);
    g_autofree char *head = NULL;

    if (len >= 8 && memcmp (bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "png";

    head = g_strndup (bytes, MIN (len, 2048));
    if (strstr (head, "<svg") != NULL)
        return "svg";
    if (strstr (head, "/* XPM */") != NULL)
        return "xpm";

    return NULL;
}

static int
hicolor_score (const char *rel)
{
    const char *rest = rel + strlen ("usr/share/icons/hicolor/");

    if (g_str_has_prefix (rest, "scalable/"))
        return 1000;

    return atoi (rest);
}

static GBytes *
find_icon (AiReader *reader, const char *icon_name, GCancellable *cancellable)
{
    AiEntry *entry;

    if (icon_name != NULL && *icon_name != '\0') {
        g_autofree char *base = g_path_get_basename (icon_name);
        g_autofree char *png = g_strconcat (base, ".png", NULL);
        g_autofree char *svg = g_strconcat (base, ".svg", NULL);
        g_autofree char *xpm = g_strconcat (base, ".xpm", NULL);
        const char *candidates[] = { png, svg, xpm, base, NULL };

        ai_fetch (reader, candidates, cancellable);
        for (guint i = 0; candidates[i] != NULL; i++) {
            entry = ai_get (reader, candidates[i], cancellable);
            if (entry != NULL && image_extension (entry->data) != NULL)
                return g_bytes_ref (entry->data);
        }
    }

    entry = ai_get (reader, ".DirIcon", cancellable);
    if (entry != NULL && image_extension (entry->data) != NULL)
        return g_bytes_ref (entry->data);

    if (icon_name != NULL && *icon_name != '\0') {
        g_autofree char *base = g_path_get_basename (icon_name);
        g_autofree char *png = g_strdup_printf ("usr/share/icons/hicolor/*/apps/%s.png", base);
        g_autofree char *svg = g_strdup_printf ("usr/share/icons/hicolor/*/apps/%s.svg", base);
        const char *patterns[] = { png, svg, NULL };
        GHashTableIter iter;
        const char *rel;
        const char *best = NULL;
        int best_score = -1;

        ai_fetch (reader, patterns, cancellable);
        g_hash_table_iter_init (&iter, reader->entries);
        while (g_hash_table_iter_next (&iter, (gpointer *) &rel, NULL)) {
            int score;

            if (!matches_any (patterns, rel))
                continue;
            score = hicolor_score (rel);
            if (score > best_score) {
                best_score = score;
                best = rel;
            }
        }

        if (best != NULL) {
            g_autofree char *best_copy = g_strdup (best);

            entry = ai_get (reader, best_copy, cancellable);
            if (entry != NULL && image_extension (entry->data) != NULL)
                return g_bytes_ref (entry->data);
        }
    }

    return NULL;
}

static char *
find_root_desktop_file (AiReader *reader)
{
    g_autoptr(GPtrArray) names = g_ptr_array_new ();
    GHashTableIter iter;
    const char *rel;

    g_hash_table_iter_init (&iter, reader->entries);
    while (g_hash_table_iter_next (&iter, (gpointer *) &rel, NULL)) {
        if (strchr (rel, '/') == NULL && g_str_has_suffix (rel, ".desktop"))
            g_ptr_array_add (names, (gpointer) rel);
    }

    if (names->len == 0)
        return NULL;

    g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
    return g_strdup (g_ptr_array_index (names, 0));
}

static gboolean
write_bytes (const char *path, GBytes *bytes)
{
    gsize len;
    const char *data = g_bytes_get_data (bytes, &len);

    return g_file_set_contents (path, data, len, NULL);
}

static GKeyFile *
extract_metadata (const char *path, int type, const char *cache_dir, GCancellable *cancellable)
{
    g_autoptr(GKeyFile) meta = g_key_file_new ();
    g_autoptr(GBytes) icon = NULL;
    g_autofree char *desktop_rel = NULL;
    g_autofree char *icon_name = NULL;
    AiReader reader = { 0 };
    const char *first[] = { "*.desktop", ".DirIcon", NULL };
    guint64 offset = 0;

    if (type != 1)
        offset = find_squashfs_offset (path);

    if (offset > 0) {
        type = 2;
    } else if (type == 2) {
        g_debug ("%s says it is a type 2 AppImage but has no SquashFS image", path);
        return NULL;
    } else {
        type = 1;
    }

    if (type == 2) {
        g_autofree char *unsquashfs = g_find_program_in_path ("unsquashfs");

        if (unsquashfs == NULL) {
            g_warning ("unsquashfs is not installed, cannot read %s", path);
            return NULL;
        }
    }

    reader.type = type;
    reader.path = (char *) path;
    reader.offset = offset;
    reader.entries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) ai_entry_free);
    if (type == 2) {
        reader.tmp_root = g_dir_make_tmp ("sr-appimage-XXXXXX", NULL);
        if (reader.tmp_root == NULL) {
            g_hash_table_unref (reader.entries);
            return NULL;
        }
    }

    g_mkdir_with_parents (cache_dir, 0700);
    g_key_file_set_integer (meta, CACHE_GROUP, "Type", type);

    ai_fetch (&reader, first, cancellable);

    desktop_rel = find_root_desktop_file (&reader);
    if (desktop_rel != NULL) {
        AiEntry *entry = ai_get (&reader, desktop_rel, cancellable);
        g_autoptr(GKeyFile) desktop = g_key_file_new ();

        if (entry != NULL) {
            gsize len;
            const char *data = g_bytes_get_data (entry->data, &len);

            if (g_key_file_load_from_data (desktop, data, len, G_KEY_FILE_KEEP_TRANSLATIONS, NULL)) {
                g_autofree char *name = g_key_file_get_locale_string (desktop, G_KEY_FILE_DESKTOP_GROUP,
                                                                      G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
                g_autofree char *comment = g_key_file_get_locale_string (desktop, G_KEY_FILE_DESKTOP_GROUP,
                                                                         G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL, NULL);
                g_autofree char *categories = g_key_file_get_string (desktop, G_KEY_FILE_DESKTOP_GROUP,
                                                                     G_KEY_FILE_DESKTOP_KEY_CATEGORIES, NULL);
                g_autofree char *version
                    = g_key_file_get_string (desktop, G_KEY_FILE_DESKTOP_GROUP, "X-AppImage-Version", NULL);
                g_autofree char *base = g_path_get_basename (desktop_rel);
                g_autofree char *cached = g_build_filename (cache_dir, base, NULL);

                icon_name = g_key_file_get_string (desktop, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);

                if (name)
                    g_key_file_set_string (meta, CACHE_GROUP, "Name", name);
                if (comment)
                    g_key_file_set_string (meta, CACHE_GROUP, "Comment", comment);
                if (categories)
                    g_key_file_set_string (meta, CACHE_GROUP, "Categories", categories);
                if (version)
                    g_key_file_set_string (meta, CACHE_GROUP, "Version", version);

                if (write_bytes (cached, entry->data))
                    g_key_file_set_string (meta, CACHE_GROUP, "DesktopFile", base);
            }
        }
    }

    icon = find_icon (&reader, icon_name, cancellable);
    if (icon != NULL) {
        g_autofree char *icon_file = g_strconcat ("icon.", image_extension (icon), NULL);
        g_autofree char *icon_path = g_build_filename (cache_dir, icon_file, NULL);

        if (write_bytes (icon_path, icon))
            g_key_file_set_string (meta, CACHE_GROUP, "Icon", icon_file);
    }

    if (reader.tmp_root != NULL) {
        rm_rf (reader.tmp_root);
        g_free (reader.tmp_root);
    }
    g_hash_table_unref (reader.entries);

    if (g_cancellable_is_cancelled (cancellable))
        return NULL;

    return g_steal_pointer (&meta);
}

SrApp *
sr_appimage_probe (const char *host_path, GCancellable *cancellable)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);
    g_autofree char *basename = g_path_get_basename (host_path);
    g_autofree char *key = NULL;
    g_autofree char *key_source = NULL;
    g_autofree char *cache_dir = NULL;
    g_autofree char *meta_path = NULL;
    g_autofree char *icon_file = NULL;
    g_autofree char *desktop_file = NULL;
    g_autofree char *categories = NULL;
    g_autofree char *size = NULL;
    g_autoptr(GKeyFile) meta = g_key_file_new ();
    g_autoptr(SrApp) app = NULL;
    struct stat st;
    int type = 0;

    if (stat (path, &st) != 0 || !S_ISREG (st.st_mode) || !sr_appimage_detect (path, &type))
        return NULL;

    key_source = g_strdup_printf ("%s\n%" G_GINT64_FORMAT "\n%" G_GINT64_FORMAT, host_path, (gint64) st.st_size,
                                  (gint64) st.st_mtime);
    key = g_compute_checksum_for_string (G_CHECKSUM_SHA1, key_source, -1);
    cache_dir = g_build_filename (g_get_user_cache_dir (), "appimages", key, NULL);
    meta_path = g_build_filename (cache_dir, "meta.ini", NULL);

    if (!g_key_file_load_from_file (meta, meta_path, G_KEY_FILE_NONE, NULL)) {
        g_autoptr(GKeyFile) extracted = extract_metadata (path, type, cache_dir, cancellable);

        if (extracted != NULL) {
            g_key_file_save_to_file (extracted, meta_path, NULL);
            g_key_file_unref (meta);
            meta = g_steal_pointer (&extracted);
        }
    }

    type = g_key_file_get_integer (meta, CACHE_GROUP, "Type", NULL);

    app = sr_app_new ();
    app->id = g_strdup (host_path);
    app->name = g_key_file_get_string (meta, CACHE_GROUP, "Name", NULL);
    if (app->name == NULL)
        app->name = sr_clean_display_name (basename);
    app->comment = g_key_file_get_string (meta, CACHE_GROUP, "Comment", NULL);
    if (app->comment == NULL)
        app->comment = g_strdup (_("AppImage application"));

    icon_file = g_key_file_get_string (meta, CACHE_GROUP, "Icon", NULL);
    if (icon_file != NULL) {
        g_autofree char *icon_path = g_build_filename (cache_dir, icon_file, NULL);
        g_autoptr(GFile) file = g_file_new_for_path (icon_path);

        app->icon = g_file_icon_new (file);
    } else {
        app->icon = g_themed_icon_new ("sr-appimage");
    }

    app->source = SR_SOURCE_STANDALONE;
    app->kind = g_strdup (_("AppImage"));
    if (type == 1)
        app->install_type = g_strdup (_("AppImage (type 1, ISO 9660)"));
    else if (type == 2)
        app->install_type = g_strdup (_("AppImage (type 2, SquashFS)"));
    else
        app->install_type = g_strdup (_("AppImage"));
    app->location = g_strdup (host_path);
    app->exec = g_strdup (host_path);
    app->standalone_root = g_strdup (host_path);
    app->version = g_key_file_get_string (meta, CACHE_GROUP, "Version", NULL);

    desktop_file = g_key_file_get_string (meta, CACHE_GROUP, "DesktopFile", NULL);
    if (desktop_file != NULL) {
        g_autofree char *cached = g_build_filename (cache_dir, desktop_file, NULL);

        app->desktop_file = sr_host_from_sandbox_path (cached);
        app->desktop_file_note = g_strdup (_("Embedded in the AppImage. This is a cached copy."));
    }

    categories = g_key_file_get_string (meta, CACHE_GROUP, "Categories", NULL);
    app->categories = sr_join_categories (categories);

    size = g_format_size (st.st_size);
    sr_app_add_detail (app, _("Size"), size, FALSE);
    if ((st.st_mode & 0111) == 0)
        sr_app_add_detail (app, _("Executable"), _("No, it needs the executable permission to run"), FALSE);

    return g_steal_pointer (&app);
}

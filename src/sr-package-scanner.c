/* sr-package-scanner.c
 *
 * Copyright 2026 Martins
 *
 * Finds packages installed outside dpkg: snaps, Flatpak runtimes, Node.js
 * versions and the npm, pnpm and bun packages installed globally into them,
 * Rust, Go and Python tools, Homebrew formulae and programs dropped into
 * ~/.local/bin or /usr/local/bin. Everything is read from files on disk;
 * no program is run.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-package-scanner"

#include "config.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

#include "sr-bundle-probe.h"
#include "sr-host.h"
#include "sr-package-scanner.h"
#include "sr-utils.h"

static char *
home_path (const char *first, ...)
{
    g_autoptr(GPtrArray) parts = g_ptr_array_new ();
    const char *part;
    va_list args;

    g_ptr_array_add (parts, (gpointer) g_get_home_dir ());
    va_start (args, first);
    for (part = first; part != NULL; part = va_arg (args, const char *))
        g_ptr_array_add (parts, (gpointer) part);
    va_end (args);
    g_ptr_array_add (parts, NULL);

    return g_build_filenamev ((char **) parts->pdata);
}

/* Sorted entry names of a host directory, hidden entries skipped */
static GPtrArray *
list_dir (const char *host_dir)
{
    g_autofree char *dir = sr_host_to_sandbox_path (host_dir);
    g_autoptr(GDir) gdir = g_dir_open (dir, 0, NULL);
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    const char *name;

    while (gdir != NULL && (name = g_dir_read_name (gdir)) != NULL) {
        if (name[0] != '.')
            g_ptr_array_add (names, g_strdup (name));
    }
    g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);

    return names;
}

static gboolean
host_is_dir (const char *host_path)
{
    return sr_host_path_is_dir (host_path);
}

static gboolean
host_is_file (const char *host_path)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);

    return g_file_test (path, G_FILE_TEST_IS_REGULAR);
}

static gboolean
host_is_symlink (const char *host_path)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);

    return g_file_test (path, G_FILE_TEST_IS_SYMLINK);
}

static char *
host_read (const char *host_path)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);
    char *contents = NULL;

    if (!g_file_get_contents (path, &contents, NULL, NULL))
        return NULL;

    return contents;
}

static JsonObject *
host_read_json (const char *host_path, JsonParser *parser)
{
    g_autofree char *contents = host_read (host_path);
    JsonNode *root;

    if (contents == NULL || !json_parser_load_from_data (parser, contents, -1, NULL))
        return NULL;

    root = json_parser_get_root (parser);
    return root != NULL && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
}

static const char *
json_string (JsonObject *object, const char *member)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member (object, member))
        return NULL;

    node = json_object_get_member (object, member);
    if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}

static GIcon *
themed_icon (const char *first, ...)
{
    g_autoptr(GPtrArray) names = g_ptr_array_new ();
    const char *name;
    va_list args;

    va_start (args, first);
    for (name = first; name != NULL; name = va_arg (args, const char *))
        g_ptr_array_add (names, (gpointer) name);
    va_end (args);

    return g_themed_icon_new_from_names ((char **) names->pdata, names->len);
}

static GIcon *
file_icon_if_exists (const char *host_path)
{
    g_autofree char *path = sr_host_to_sandbox_path (host_path);
    g_autoptr(GFile) file = NULL;

    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return NULL;

    file = g_file_new_for_path (path);
    return g_file_icon_new (file);
}

static SrPackage *
add_package (GPtrArray *out, SrPackageManager manager, const char *detail, const char *name, const char *version,
             const char *summary, const char *location)
{
    SrPackage *package = sr_package_new_full (manager, detail, name, version, summary, location);

    g_ptr_array_add (out, package);
    return package;
}

/* ---- Snap ------------------------------------------------------------- */

static char *
yaml_block (const char *yaml, const char *field)
{
    g_autofree char *prefix = g_strconcat (field, ":", NULL);
    g_auto(GStrv) lines = g_strsplit (yaml, "\n", -1);
    g_autoptr(GString) text = NULL;

    for (guint i = 0; lines[i] != NULL; i++) {
        if (!g_str_has_prefix (lines[i], prefix))
            continue;

        text = g_string_new (NULL);
        for (guint j = i + 1; lines[j] != NULL && (lines[j][0] == ' ' || lines[j][0] == '\0'); j++) {
            g_autofree char *line = g_strstrip (g_strdup (lines[j]));

            if (*line == '\0') {
                if (text->len > 0)
                    g_string_append (text, "\n\n");
                continue;
            }
            if (text->len > 0 && text->str[text->len - 1] != '\n')
                g_string_append_c (text, ' ');
            g_string_append (text, line);
        }
        break;
    }

    if (text == NULL || text->len == 0)
        return NULL;

    return g_strstrip (g_string_free (g_steal_pointer (&text), FALSE));
}

static void
scan_snaps (GPtrArray *out)
{
    g_autoptr(GPtrArray) names = list_dir ("/snap");

    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index (names, i);
        g_autofree char *snap_dir = g_build_filename ("/snap", name, NULL);
        g_autofree char *yaml_path = g_build_filename (snap_dir, "current", "meta", "snap.yaml", NULL);
        g_autofree char *current_link = g_build_filename (snap_dir, "current", NULL);
        g_autofree char *revision = NULL;
        g_autofree char *yaml = NULL;
        g_autofree char *version = NULL;
        g_autofree char *summary = NULL;
        g_autofree char *type = NULL;
        g_autofree char *base = NULL;
        g_autofree char *confinement = NULL;
        g_autofree char *location = NULL;
        g_autofree char *icon_png = NULL;
        g_autofree char *icon_svg = NULL;
        SrPackage *package;

        yaml = host_read (yaml_path);
        if (yaml == NULL)
            continue;

        revision = g_file_read_link (current_link, NULL);
        version = sr_yaml_get_field (yaml, "version");
        summary = sr_yaml_get_field (yaml, "summary");
        type = sr_yaml_get_field (yaml, "type");
        base = sr_yaml_get_field (yaml, "base");
        confinement = sr_yaml_get_field (yaml, "confinement");
        location = revision != NULL ? g_build_filename (snap_dir, revision, NULL) : g_strdup (snap_dir);

        package = add_package (out, SR_PACKAGE_MANAGER_SNAP, NULL, name, version, summary, location);
        package->description = yaml_block (yaml, "description");
        if (package->description == NULL)
            package->description = sr_yaml_get_field (yaml, "description");
        package->is_app = type == NULL || g_str_equal (type, "app");
        package->auto_installed = !package->is_app;

        icon_png = g_build_filename (current_link, "meta", "gui", "icon.png", NULL);
        icon_svg = g_build_filename (current_link, "meta", "gui", "icon.svg", NULL);
        package->icon = file_icon_if_exists (icon_png);
        if (package->icon == NULL)
            package->icon = file_icon_if_exists (icon_svg);
        if (package->icon == NULL)
            package->icon = themed_icon ("package-x-generic", NULL);

        sr_package_add_detail (package, _("Revision"), revision, FALSE);
        sr_package_add_detail (package, _("Type"), type != NULL ? type : "app", FALSE);
        sr_package_add_detail (package, _("Base"), base, FALSE);
        sr_package_add_detail (package, _("Confinement"), confinement, FALSE);
    }
}

/* ---- Flatpak ---------------------------------------------------------- */

static char *
xml_first_text (const char *xml, const char *tag)
{
    g_autofree char *open = g_strdup_printf ("<%s>", tag);
    g_autofree char *close = g_strdup_printf ("</%s>", tag);
    const char *start = strstr (xml, open);
    const char *end;

    if (start == NULL)
        return NULL;
    start += strlen (open);
    end = strstr (start, close);
    if (end == NULL)
        return NULL;

    return g_strstrip (g_strndup (start, end - start));
}

static char *
xml_release_version (const char *xml)
{
    const char *release = strstr (xml, "<release ");
    const char *version, *end;

    if (release == NULL)
        return NULL;
    version = strstr (release, "version=\"");
    if (version == NULL)
        return NULL;
    version += strlen ("version=\"");
    end = strchr (version, '"');

    return end != NULL ? g_strndup (version, end - version) : NULL;
}

static char *
read_flatpak_metainfo (const char *active, const char *id)
{
    const char *dirs[] = { "metainfo", "appdata" };
    const char *exts[] = { "metainfo.xml", "appdata.xml" };

    for (guint i = 0; i < G_N_ELEMENTS (dirs); i++) {
        for (guint j = 0; j < G_N_ELEMENTS (exts); j++) {
            g_autofree char *file_name = g_strdup_printf ("%s.%s", id, exts[j]);
            g_autofree char *path = g_build_filename (active, "files", "share", dirs[i], file_name, NULL);
            char *contents = host_read (path);

            if (contents != NULL)
                return contents;
        }
    }

    return NULL;
}

static char *
flatpak_extension_summary (const char *id)
{
    if (g_str_has_suffix (id, ".Locale"))
        return g_strdup_printf (_("Translations for %.*s"), (int) (strlen (id) - strlen (".Locale")), id);
    if (g_str_has_suffix (id, ".Debug"))
        return g_strdup_printf (_("Debug symbols for %.*s"), (int) (strlen (id) - strlen (".Debug")), id);
    if (strstr (id, ".GL.") != NULL)
        return g_strdup (_("Graphics drivers for Flatpak apps"));
    if (strstr (id, ".Gtk3theme.") != NULL)
        return g_strdup (_("GTK 3 theme for Flatpak apps"));
    if (strstr (id, ".codecs") != NULL || strstr (id, ".openh264") != NULL || strstr (id, ".VAAPI.") != NULL)
        return g_strdup (_("Media codecs for Flatpak apps"));

    return NULL;
}

static void
scan_flatpak_installation (const char *base, gboolean user, GPtrArray *out)
{
    const char *kinds[] = { "app", "runtime" };

    for (guint k = 0; k < G_N_ELEMENTS (kinds); k++) {
        g_autofree char *kind_dir = g_build_filename (base, kinds[k], NULL);
        g_autoptr(GPtrArray) ids = list_dir (kind_dir);
        gboolean is_app = k == 0;

        for (guint i = 0; i < ids->len; i++) {
            const char *id = g_ptr_array_index (ids, i);
            g_autofree char *id_dir = g_build_filename (kind_dir, id, NULL);
            g_autoptr(GPtrArray) arches = list_dir (id_dir);

            for (guint a = 0; a < arches->len; a++) {
                const char *arch = g_ptr_array_index (arches, a);
                g_autofree char *arch_dir = g_build_filename (id_dir, arch, NULL);
                g_autoptr(GPtrArray) branches = NULL;

                if (g_str_equal (arch, "current") || host_is_symlink (arch_dir))
                    continue;

                branches = list_dir (arch_dir);
                for (guint b = 0; b < branches->len; b++) {
                    const char *branch = g_ptr_array_index (branches, b);
                    g_autofree char *active = g_build_filename (arch_dir, branch, "active", NULL);
                    g_autofree char *real = sr_realpath (active);
                    g_autofree char *metainfo = NULL;
                    g_autofree char *summary = NULL;
                    g_autofree char *version = NULL;
                    g_autofree char *ref = NULL;
                    SrPackage *package;

                    if (real == NULL)
                        continue;

                    metainfo = read_flatpak_metainfo (real, id);
                    if (metainfo != NULL) {
                        summary = xml_first_text (metainfo, "summary");
                        version = xml_release_version (metainfo);
                    }
                    if (summary == NULL)
                        summary = flatpak_extension_summary (id);

                    package = add_package (out, SR_PACKAGE_MANAGER_FLATPAK, is_app ? NULL : _("Runtime"), id,
                                           version != NULL ? version : branch, summary, real);
                    package->arch = g_strdup (arch);
                    package->is_app = is_app;
                    package->auto_installed = !is_app;
                    package->icon = is_app ? themed_icon (id, "application-x-executable", NULL)
                                           : themed_icon ("application-x-addon", NULL);

                    ref = g_strdup_printf ("%s/%s/%s/%s", kinds[k], id, arch, branch);
                    sr_package_add_detail (package, _("Ref"), ref, FALSE);
                    sr_package_add_detail (package, _("Branch"), branch, FALSE);
                    sr_package_add_detail (package, _("Installation"),
                                           user ? _("Your user installation") : _("System installation"), FALSE);
                }
            }
        }
    }
}

static void
scan_flatpak (GPtrArray *out)
{
    g_autofree char *user = home_path (".local", "share", "flatpak", NULL);

    scan_flatpak_installation ("/var/lib/flatpak", FALSE, out);
    scan_flatpak_installation (user, TRUE, out);
}

/* ---- JavaScript ------------------------------------------------------- */

typedef struct {
    char *prefix;
    char *context;
    gboolean skip_bundled;
} NodePrefix;

static void
node_prefix_free (NodePrefix *prefix)
{
    g_free (prefix->prefix);
    g_free (prefix->context);
    g_free (prefix);
}

static void
add_node_prefix (GPtrArray *prefixes, const char *prefix, char *context, gboolean skip_bundled)
{
    NodePrefix *entry = g_new0 (NodePrefix, 1);

    entry->prefix = g_strdup (prefix);
    entry->context = context;
    entry->skip_bundled = skip_bundled;
    g_ptr_array_add (prefixes, entry);
}

static char *
bundled_module_version (const char *prefix, const char *module)
{
    g_autofree char *path = g_build_filename (prefix, "lib", "node_modules", module, "package.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new ();
    JsonObject *object = host_read_json (path, parser);

    return g_strdup (json_string (object, "version"));
}

static void
add_node_version (GPtrArray *out, GPtrArray *prefixes, const char *prefix, const char *version, const char *manager,
                  gboolean is_default)
{
    g_autofree char *npm = bundled_module_version (prefix, "npm");
    g_autofree char *corepack = bundled_module_version (prefix, "corepack");
    g_autofree char *program = g_build_filename (prefix, "bin", "node", NULL);
    g_autofree char *display_version = g_str_has_prefix (version, "v") ? g_strdup (version)
                                                                        : g_strconcat ("v", version, NULL);
    g_autoptr(GString) includes = g_string_new (NULL);
    SrPackage *package;

    package = add_package (out, SR_PACKAGE_MANAGER_NODE, manager, "node", display_version,
                           is_default ? _("JavaScript runtime, your default Node.js version") : _("JavaScript runtime"),
                           prefix);
    package->is_default = is_default;
    package->icon = themed_icon ("nodejs", "text-x-script", NULL);
    package->homepage = g_strdup ("https://nodejs.org");

    if (npm != NULL)
        g_string_append_printf (includes, "npm %s", npm);
    if (corepack != NULL)
        g_string_append_printf (includes, "%scorepack %s", includes->len > 0 ? ", " : "", corepack);

    sr_package_add_detail (package, _("Version Manager"), manager, FALSE);
    sr_package_add_detail (package, _("Default Version"), is_default ? _("Yes") : _("No"), FALSE);
    sr_package_add_detail (package, _("Includes"), includes->str, FALSE);
    sr_package_add_detail (package, _("Program"), program, TRUE);

    add_node_prefix (prefixes, prefix, g_strdup_printf ("Node.js %s (%s)", display_version, manager), TRUE);
}

static void
scan_fnm (GPtrArray *out, GPtrArray *prefixes)
{
    g_autofree char *xdg = home_path (".local", "share", "fnm", NULL);
    g_autofree char *legacy = home_path (".fnm", NULL);
    const char *dirs[] = { g_getenv ("FNM_DIR"), xdg, legacy };

    for (guint d = 0; d < G_N_ELEMENTS (dirs); d++) {
        g_autofree char *versions_dir = NULL;
        g_autofree char *default_link = NULL;
        g_autofree char *default_real = NULL;
        g_autoptr(GPtrArray) versions = NULL;

        if (dirs[d] == NULL || (d > 0 && dirs[0] != NULL && g_str_equal (dirs[0], dirs[d])))
            continue;

        versions_dir = g_build_filename (dirs[d], "node-versions", NULL);
        default_link = g_build_filename (dirs[d], "aliases", "default", NULL);
        default_real = sr_realpath (default_link);
        versions = list_dir (versions_dir);

        for (guint i = 0; i < versions->len; i++) {
            const char *version = g_ptr_array_index (versions, i);
            g_autofree char *prefix = g_build_filename (versions_dir, version, "installation", NULL);
            g_autofree char *prefix_real = sr_realpath (prefix);

            if (prefix_real == NULL)
                continue;

            add_node_version (out, prefixes, prefix, version, "fnm",
                              default_real != NULL && g_str_equal (default_real, prefix_real));
        }
    }
}

static gboolean
version_matches_alias (const char *version, const char *alias)
{
    const char *v = version[0] == 'v' ? version + 1 : version;
    const char *a;
    gsize len;

    if (alias == NULL)
        return FALSE;

    a = alias[0] == 'v' ? alias + 1 : alias;
    len = strlen (a);

    return g_str_equal (v, a) || (len > 0 && g_ascii_isdigit (a[0]) && g_str_has_prefix (v, a) && v[len] == '.');
}

static void
scan_versions_dir (GPtrArray *out, GPtrArray *prefixes, const char *versions_dir, const char *manager,
                   const char *default_alias)
{
    g_autoptr(GPtrArray) versions = list_dir (versions_dir);

    for (guint i = 0; i < versions->len; i++) {
        const char *version = g_ptr_array_index (versions, i);
        g_autofree char *prefix = g_build_filename (versions_dir, version, NULL);

        /* mise and asdf keep aliases such as "lts" or "22" as symlinks */
        if (host_is_symlink (prefix) || !host_is_dir (prefix) || !g_ascii_isdigit (version[version[0] == 'v']))
            continue;

        add_node_version (out, prefixes, prefix, version, manager, version_matches_alias (version, default_alias));
    }
}

static void
scan_node_versions (GPtrArray *out, GPtrArray *prefixes)
{
    g_autofree char *nvm = home_path (".nvm", "versions", "node", NULL);
    g_autofree char *nvm_alias_path = home_path (".nvm", "alias", "default", NULL);
    g_autofree char *nvm_alias = sr_read_first_line (nvm_alias_path);
    g_autofree char *volta = home_path (".volta", "tools", "image", "node", NULL);
    g_autofree char *volta_platform_path = home_path (".volta", "tools", "user", "platform.json", NULL);
    g_autofree char *mise = home_path (".local", "share", "mise", "installs", "node", NULL);
    g_autofree char *asdf = home_path (".asdf", "installs", "nodejs", NULL);
    g_autoptr(JsonParser) parser = json_parser_new ();
    JsonObject *platform = host_read_json (volta_platform_path, parser);
    const char *volta_default = NULL;

    if (platform != NULL && json_object_has_member (platform, "node")) {
        JsonNode *node = json_object_get_member (platform, "node");

        if (JSON_NODE_HOLDS_OBJECT (node))
            volta_default = json_string (json_node_get_object (node), "runtime");
    }

    scan_fnm (out, prefixes);
    scan_versions_dir (out, prefixes, nvm, "nvm", nvm_alias);
    scan_versions_dir (out, prefixes, volta, "volta", volta_default);
    scan_versions_dir (out, prefixes, mise, "mise", NULL);
    scan_versions_dir (out, prefixes, asdf, "asdf", NULL);
}

static void
add_js_package (GPtrArray *out, SrPackageManager manager, const char *detail, const char *package_dir,
                const char *context)
{
    g_autofree char *package_json = g_build_filename (package_dir, "package.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new ();
    g_autoptr(GPtrArray) programs = g_ptr_array_new ();
    g_autofree char *joined = NULL;
    JsonObject *object = host_read_json (package_json, parser);
    const char *name = json_string (object, "name");
    SrPackage *package;

    if (object == NULL || name == NULL)
        return;

    package = add_package (out, manager, detail, name, json_string (object, "version"),
                           json_string (object, "description"), package_dir);
    package->homepage = g_strdup (json_string (object, "homepage"));
    package->icon = themed_icon ("text-x-script", NULL);

    if (json_object_has_member (object, "bin")) {
        JsonNode *bin = json_object_get_member (object, "bin");

        if (JSON_NODE_HOLDS_OBJECT (bin)) {
            g_autoptr(GList) members = json_object_get_members (json_node_get_object (bin));

            for (GList *l = members; l != NULL; l = l->next)
                g_ptr_array_add (programs, l->data);
        } else if (JSON_NODE_HOLDS_VALUE (bin)) {
            const char *base = strrchr (name, '/');
            g_ptr_array_add (programs, (gpointer) (base != NULL ? base + 1 : name));
        }
    }

    if (programs->len > 0) {
        g_ptr_array_add (programs, NULL);
        joined = g_strjoinv (", ", (char **) programs->pdata);
    }

    sr_package_add_detail (package, _("Installed For"), context, FALSE);
    sr_package_add_detail (package, _("Programs"), joined, FALSE);
}

static void
scan_node_modules (GPtrArray *out, SrPackageManager manager, const char *detail, const char *modules_dir,
                   const char *context, gboolean skip_bundled)
{
    g_autoptr(GPtrArray) names = list_dir (modules_dir);

    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index (names, i);
        g_autofree char *path = g_build_filename (modules_dir, name, NULL);

        if (skip_bundled && (g_str_equal (name, "npm") || g_str_equal (name, "corepack")))
            continue;

        if (name[0] == '@') {
            g_autoptr(GPtrArray) scoped = list_dir (path);

            for (guint j = 0; j < scoped->len; j++) {
                g_autofree char *scoped_path = g_build_filename (path, g_ptr_array_index (scoped, j), NULL);
                add_js_package (out, manager, detail, scoped_path, context);
            }
        } else {
            add_js_package (out, manager, detail, path, context);
        }
    }
}

static char *
npmrc_prefix (void)
{
    g_autofree char *npmrc = home_path (".npmrc", NULL);
    g_autofree char *contents = host_read (npmrc);
    g_auto(GStrv) lines = NULL;

    if (contents == NULL)
        return NULL;

    lines = g_strsplit (contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip (lines[i]);

        if (g_str_has_prefix (line, "prefix")) {
            char *eq = strchr (line, '=');

            if (eq != NULL)
                return sr_host_expand_home (g_strstrip (eq + 1));
        }
    }

    return NULL;
}

static void
scan_npm_globals (GPtrArray *out, GPtrArray *prefixes)
{
    g_autofree char *npm_global = home_path (".npm-global", NULL);
    g_autofree char *custom = npmrc_prefix ();
    g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    add_node_prefix (prefixes, "/usr/local", g_strdup (_("System npm in /usr/local")), FALSE);
    add_node_prefix (prefixes, npm_global, g_strdup ("~/.npm-global"), FALSE);
    if (custom != NULL)
        add_node_prefix (prefixes, custom, g_strdup_printf (_("npm prefix %s"), custom), FALSE);

    for (guint i = 0; i < prefixes->len; i++) {
        NodePrefix *prefix = g_ptr_array_index (prefixes, i);
        g_autofree char *modules = g_build_filename (prefix->prefix, "lib", "node_modules", NULL);

        if (g_hash_table_contains (seen, modules))
            continue;
        g_hash_table_add (seen, g_strdup (modules));

        scan_node_modules (out, SR_PACKAGE_MANAGER_NPM, NULL, modules, prefix->context, prefix->skip_bundled);
    }
}

static void
scan_dependency_dir (GPtrArray *out, SrPackageManager manager, const char *detail, const char *dir,
                     const char *context)
{
    g_autofree char *package_json = g_build_filename (dir, "package.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new ();
    JsonObject *object = host_read_json (package_json, parser);
    g_autoptr(GList) names = NULL;
    JsonNode *deps;

    if (object == NULL || !json_object_has_member (object, "dependencies"))
        return;

    deps = json_object_get_member (object, "dependencies");
    if (!JSON_NODE_HOLDS_OBJECT (deps))
        return;

    names = json_object_get_members (json_node_get_object (deps));
    for (GList *l = names; l != NULL; l = l->next) {
        g_autofree char *path = g_build_filename (dir, "node_modules", (const char *) l->data, NULL);
        add_js_package (out, manager, detail, path, context);
    }
}

static void
scan_pnpm (GPtrArray *out)
{
    g_autofree char *default_home = home_path (".local", "share", "pnpm", NULL);
    const char *pnpm_home = g_getenv ("PNPM_HOME") != NULL ? g_getenv ("PNPM_HOME") : default_home;
    const char *tool_dirs[] = { "pnpm-exe", "pnpm" };
    g_autofree char *global = g_build_filename (pnpm_home, "global", NULL);
    g_autoptr(GPtrArray) layouts = list_dir (global);

    for (guint t = 0; t < G_N_ELEMENTS (tool_dirs); t++) {
        g_autofree char *dir = g_build_filename (pnpm_home, ".tools", tool_dirs[t], NULL);
        g_autoptr(GPtrArray) versions = list_dir (dir);

        for (guint i = 0; i < versions->len; i++) {
            const char *version = g_ptr_array_index (versions, i);
            g_autofree char *location = g_build_filename (dir, version, NULL);
            SrPackage *package;

            package = add_package (out, SR_PACKAGE_MANAGER_PNPM, NULL, "pnpm", version,
                                   _("Fast, disk space efficient package manager"), location);
            package->icon = themed_icon ("pnpm", "text-x-script", NULL);
            package->homepage = g_strdup ("https://pnpm.io");
            sr_package_add_detail (package, _("Installed With"),
                                   _("The standalone pnpm installer"), FALSE);
        }
    }

    for (guint i = 0; i < layouts->len; i++) {
        g_autofree char *dir = g_build_filename (global, g_ptr_array_index (layouts, i), NULL);
        scan_dependency_dir (out, SR_PACKAGE_MANAGER_PNPM, _("pnpm global"), dir, _("pnpm global packages"));
    }
}

static void
scan_bun_deno (GPtrArray *out)
{
    g_autofree char *bun = home_path (".bun", "bin", "bun", NULL);
    g_autofree char *bun_global = home_path (".bun", "install", "global", NULL);
    g_autofree char *deno = home_path (".deno", "bin", "deno", NULL);

    if (host_is_file (bun)) {
        SrPackage *package = add_package (out, SR_PACKAGE_MANAGER_BUN, NULL, "bun", NULL,
                                          _("JavaScript runtime, bundler and package manager"), bun);

        package->icon = themed_icon ("bun", "text-x-script", NULL);
        package->homepage = g_strdup ("https://bun.sh");
    }

    scan_dependency_dir (out, SR_PACKAGE_MANAGER_BUN, _("bun global"), bun_global, _("bun global packages"));

    if (host_is_file (deno)) {
        SrPackage *package = add_package (out, SR_PACKAGE_MANAGER_DENO, NULL, "deno", NULL,
                                          _("JavaScript and TypeScript runtime"), deno);

        package->icon = themed_icon ("deno", "text-x-script", NULL);
        package->homepage = g_strdup ("https://deno.com");
    }
}

/* ---- Rust and Go ------------------------------------------------------ */

static char *
toml_string_after (const char *contents, const char *section, const char *key)
{
    const char *start = section != NULL ? strstr (contents, section) : contents;
    g_autofree char *needle = g_strdup_printf ("%s = \"", key);
    const char *value, *end;

    if (start == NULL)
        return NULL;
    value = strstr (start, needle);
    if (value == NULL)
        return NULL;
    value += strlen (needle);
    end = strchr (value, '"');

    return end != NULL ? g_strndup (value, end - value) : NULL;
}

static void
scan_rust (GPtrArray *out)
{
    g_autofree char *default_rustup = home_path (".rustup", NULL);
    g_autofree char *default_cargo = home_path (".cargo", NULL);
    const char *rustup_home = g_getenv ("RUSTUP_HOME") != NULL ? g_getenv ("RUSTUP_HOME") : default_rustup;
    const char *cargo_home = g_getenv ("CARGO_HOME") != NULL ? g_getenv ("CARGO_HOME") : default_cargo;
    g_autofree char *toolchains_dir = g_build_filename (rustup_home, "toolchains", NULL);
    g_autofree char *settings_path = g_build_filename (rustup_home, "settings.toml", NULL);
    g_autofree char *settings = host_read (settings_path);
    g_autofree char *default_toolchain = settings != NULL ? toml_string_after (settings, NULL, "default_toolchain") : NULL;
    g_autoptr(GPtrArray) toolchains = list_dir (toolchains_dir);
    g_autofree char *crates_path = g_build_filename (cargo_home, ".crates2.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new ();
    JsonObject *crates;

    for (guint i = 0; i < toolchains->len; i++) {
        const char *toolchain = g_ptr_array_index (toolchains, i);
        g_autofree char *dir = g_build_filename (toolchains_dir, toolchain, NULL);
        g_autofree char *manifest_path = g_build_filename (dir, "lib", "rustlib", "multirust-channel-manifest.toml", NULL);
        g_autofree char *components_path = g_build_filename (dir, "lib", "rustlib", "components", NULL);
        g_autofree char *manifest = host_read (manifest_path);
        g_autofree char *components = host_read (components_path);
        g_autofree char *version = manifest != NULL ? toml_string_after (manifest, "[pkg.rustc]", "version") : NULL;
        g_autofree char *summary = g_strdup_printf (_("Rust toolchain %s"), toolchain);
        gboolean is_default = g_strcmp0 (default_toolchain, toolchain) == 0;
        SrPackage *package;

        if (version != NULL && strchr (version, ' ') != NULL)
            *strchr (version, ' ') = '\0';

        package = add_package (out, SR_PACKAGE_MANAGER_RUST, NULL, "rust", version, summary, dir);
        package->is_default = is_default;
        package->icon = themed_icon ("rust", "application-x-executable", NULL);
        package->homepage = g_strdup ("https://www.rust-lang.org");
        sr_package_add_detail (package, _("Toolchain"), toolchain, FALSE);
        sr_package_add_detail (package, _("Default Toolchain"), is_default ? _("Yes") : _("No"), FALSE);
        if (components != NULL) {
            g_strstrip (components);
            g_strdelimit (components, "\n", ',');
            sr_package_add_detail (package, _("Components"), components, FALSE);
        }
    }

    crates = host_read_json (crates_path, parser);
    if (crates != NULL && json_object_has_member (crates, "installs")) {
        JsonNode *installs_node = json_object_get_member (crates, "installs");
        JsonObject *installs = JSON_NODE_HOLDS_OBJECT (installs_node) ? json_node_get_object (installs_node) : NULL;
        g_autoptr(GList) keys = installs != NULL ? json_object_get_members (installs) : NULL;

        for (GList *l = keys; l != NULL; l = l->next) {
            g_auto(GStrv) parts = g_strsplit (l->data, " ", 3);
            JsonNode *entry = json_object_get_member (installs, l->data);
            g_autofree char *location = NULL;
            g_autofree char *bins = NULL;
            SrPackage *package;

            if (parts[0] == NULL)
                continue;

            if (JSON_NODE_HOLDS_OBJECT (entry)) {
                JsonObject *info = json_node_get_object (entry);
                JsonNode *bins_node = json_object_has_member (info, "bins") ? json_object_get_member (info, "bins") : NULL;

                if (bins_node != NULL && JSON_NODE_HOLDS_ARRAY (bins_node)) {
                    JsonArray *array = json_node_get_array (bins_node);
                    g_autoptr(GPtrArray) names = g_ptr_array_new ();

                    for (guint j = 0; j < json_array_get_length (array); j++)
                        g_ptr_array_add (names, (gpointer) json_array_get_string_element (array, j));
                    if (names->len > 0)
                        location = g_build_filename (cargo_home, "bin", g_ptr_array_index (names, 0), NULL);
                    g_ptr_array_add (names, NULL);
                    bins = g_strjoinv (", ", (char **) names->pdata);
                }
            }

            package = add_package (out, SR_PACKAGE_MANAGER_CARGO, NULL, parts[0], parts[1],
                                   _("Installed with cargo install"), location != NULL ? location : cargo_home);
            package->icon = themed_icon ("application-x-executable", NULL);
            sr_package_add_detail (package, _("Programs"), bins, FALSE);
        }
    }
}

static void
add_go_toolchain (GPtrArray *out, const char *dir)
{
    g_autofree char *version_path = g_build_filename (dir, "VERSION", NULL);
    g_autofree char *version = sr_read_first_line (version_path);
    SrPackage *package;

    if (version == NULL)
        return;

    package = add_package (out, SR_PACKAGE_MANAGER_GO, NULL, "go",
                           g_str_has_prefix (version, "go") ? version + 2 : version,
                           _("Go programming language toolchain"), dir);
    package->icon = themed_icon ("golang", "application-x-executable", NULL);
    package->homepage = g_strdup ("https://go.dev");
}

static void
scan_go (GPtrArray *out)
{
    g_autofree char *sdk = home_path ("sdk", NULL);
    g_autofree char *gopath_bin = home_path ("go", "bin", NULL);
    g_autoptr(GPtrArray) sdks = list_dir (sdk);
    g_autoptr(GPtrArray) bins = list_dir (gopath_bin);

    add_go_toolchain (out, "/usr/local/go");
    for (guint i = 0; i < sdks->len; i++) {
        g_autofree char *dir = g_build_filename (sdk, g_ptr_array_index (sdks, i), NULL);
        add_go_toolchain (out, dir);
    }

    for (guint i = 0; i < bins->len; i++) {
        const char *name = g_ptr_array_index (bins, i);
        g_autofree char *path = g_build_filename (gopath_bin, name, NULL);
        SrPackage *package;

        if (!host_is_file (path))
            continue;

        package = add_package (out, SR_PACKAGE_MANAGER_GO, "go install", name, NULL, _("Installed with go install"),
                               path);
        package->icon = themed_icon ("application-x-executable", NULL);
    }
}

/* ---- Python ----------------------------------------------------------- */

static void
add_python_dist (GPtrArray *out, const char *detail, const char *dist_info, const char *context)
{
    g_autofree char *metadata_path = g_build_filename (dist_info, "METADATA", NULL);
    g_autofree char *metadata = host_read (metadata_path);
    g_auto(GStrv) lines = NULL;
    const char *name = NULL, *version = NULL, *summary = NULL, *homepage = NULL;
    SrPackage *package;

    if (metadata == NULL)
        return;

    lines = g_strsplit (metadata, "\n", -1);
    for (guint i = 0; lines[i] != NULL && *lines[i] != '\0'; i++) {
        if (g_str_has_prefix (lines[i], "Name: "))
            name = lines[i] + strlen ("Name: ");
        else if (g_str_has_prefix (lines[i], "Version: "))
            version = lines[i] + strlen ("Version: ");
        else if (g_str_has_prefix (lines[i], "Summary: "))
            summary = lines[i] + strlen ("Summary: ");
        else if (g_str_has_prefix (lines[i], "Home-page: "))
            homepage = lines[i] + strlen ("Home-page: ");
    }

    if (name == NULL)
        return;

    package = add_package (out, SR_PACKAGE_MANAGER_PYTHON, detail, name, version, summary, dist_info);
    package->homepage = g_strdup (homepage);
    package->icon = themed_icon ("text-x-script", NULL);
    sr_package_add_detail (package, _("Installed For"), context, FALSE);
}

static void
scan_site_packages (GPtrArray *out, const char *detail, const char *site_dir, const char *context)
{
    g_autoptr(GPtrArray) names = list_dir (site_dir);

    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index (names, i);

        if (g_str_has_suffix (name, ".dist-info")) {
            g_autofree char *dist_info = g_build_filename (site_dir, name, NULL);
            add_python_dist (out, detail, dist_info, context);
        }
    }
}

static void
scan_python_lib_dirs (GPtrArray *out, const char *lib_dir, const char *detail, const char *context)
{
    g_autoptr(GPtrArray) pythons = list_dir (lib_dir);

    for (guint i = 0; i < pythons->len; i++) {
        const char *python = g_ptr_array_index (pythons, i);
        const char *subdirs[] = { "site-packages", "dist-packages" };

        if (!g_str_has_prefix (python, "python"))
            continue;

        for (guint s = 0; s < G_N_ELEMENTS (subdirs); s++) {
            g_autofree char *site = g_build_filename (lib_dir, python, subdirs[s], NULL);
            scan_site_packages (out, detail, site, context);
        }
    }
}

static void
scan_python_tools (GPtrArray *out, const char *tools_dir, const char *detail)
{
    g_autoptr(GPtrArray) tools = list_dir (tools_dir);

    for (guint i = 0; i < tools->len; i++) {
        const char *tool = g_ptr_array_index (tools, i);
        g_autofree char *lib = g_build_filename (tools_dir, tool, "lib", NULL);
        g_autoptr(GPtrArray) pythons = list_dir (lib);
        g_autofree char *normalized = g_strdelimit (g_ascii_strdown (tool, -1), "-.", '_');
        gboolean found = FALSE;

        for (guint p = 0; p < pythons->len && !found; p++) {
            g_autofree char *site = g_build_filename (lib, g_ptr_array_index (pythons, p), "site-packages", NULL);
            g_autoptr(GPtrArray) names = list_dir (site);

            for (guint n = 0; n < names->len && !found; n++) {
                const char *name = g_ptr_array_index (names, n);
                g_autofree char *lower = g_ascii_strdown (name, -1);

                if (g_str_has_suffix (name, ".dist-info") && g_str_has_prefix (lower, normalized)
                    && lower[strlen (normalized)] == '-') {
                    g_autofree char *dist_info = g_build_filename (site, name, NULL);
                    g_autofree char *context = g_strdup_printf (_("Its own environment in %s"), tools_dir);

                    add_python_dist (out, detail, dist_info, context);
                    found = TRUE;
                }
            }
        }
    }
}

static void
scan_python (GPtrArray *out)
{
    g_autofree char *user_lib = home_path (".local", "lib", NULL);
    g_autofree char *pipx = home_path (".local", "share", "pipx", "venvs", NULL);
    g_autofree char *pipx_legacy = home_path (".local", "pipx", "venvs", NULL);
    g_autofree char *uv = home_path (".local", "share", "uv", "tools", NULL);

    scan_python_lib_dirs (out, user_lib, "pip --user", _("Your user site-packages"));
    scan_python_lib_dirs (out, "/usr/local/lib", "pip", _("System site-packages in /usr/local"));
    scan_python_tools (out, pipx, "pipx");
    scan_python_tools (out, pipx_legacy, "pipx");
    scan_python_tools (out, uv, "uv tool");
}

/* ---- Homebrew --------------------------------------------------------- */

static void
scan_homebrew (GPtrArray *out)
{
    g_autofree char *user = home_path (".linuxbrew", "Cellar", NULL);
    const char *cellars[] = { "/home/linuxbrew/.linuxbrew/Cellar", user };

    for (guint c = 0; c < G_N_ELEMENTS (cellars); c++) {
        g_autoptr(GPtrArray) formulae = list_dir (cellars[c]);

        for (guint i = 0; i < formulae->len; i++) {
            const char *name = g_ptr_array_index (formulae, i);
            g_autofree char *dir = g_build_filename (cellars[c], name, NULL);
            g_autoptr(GPtrArray) versions = list_dir (dir);

            for (guint v = 0; v < versions->len; v++) {
                g_autofree char *location = g_build_filename (dir, g_ptr_array_index (versions, v), NULL);
                SrPackage *package = add_package (out, SR_PACKAGE_MANAGER_HOMEBREW, NULL, name,
                                                  g_ptr_array_index (versions, v), _("Homebrew formula"), location);

                package->icon = themed_icon ("package-x-generic", NULL);
            }
        }
    }
}

/* ---- Loose programs --------------------------------------------------- */

static gboolean
is_claimed_target (const char *target, const char *const *claimed_prefixes)
{
    if (strstr (target, "/node_modules/") != NULL)
        return TRUE;

    for (guint i = 0; claimed_prefixes[i] != NULL; i++) {
        if (sr_file_has_prefix_dir (target, claimed_prefixes[i]))
            return TRUE;
    }

    return FALSE;
}

static void
scan_loose_programs (GPtrArray *out, SrDpkgIndex *dpkg)
{
    g_autofree char *local_bin = home_path (".local", "bin", NULL);
    g_autofree char *home_bin = home_path ("bin", NULL);
    g_autofree char *cargo = home_path (".cargo", NULL);
    g_autofree char *bun = home_path (".bun", NULL);
    g_autofree char *fnm = home_path (".local", "share", "fnm", NULL);
    g_autofree char *pnpm = home_path (".local", "share", "pnpm", NULL);
    g_autofree char *go = home_path ("go", NULL);
    g_autofree char *user_flatpak = home_path (".local", "share", "flatpak", NULL);
    const char *dirs[] = { local_bin, home_bin, "/usr/local/bin", "/usr/local/sbin" };
    const char *claimed[] = {
        "/usr/bin", "/usr/sbin", "/bin", "/sbin", "/snap", "/var/lib/flatpak", user_flatpak,
        cargo, bun, fnm, pnpm, go, NULL,
    };

    for (guint d = 0; d < G_N_ELEMENTS (dirs); d++) {
        g_autoptr(GPtrArray) names = list_dir (dirs[d]);

        for (guint i = 0; i < names->len; i++) {
            const char *name = g_ptr_array_index (names, i);
            g_autofree char *host_path = g_build_filename (dirs[d], name, NULL);
            g_autofree char *target = NULL;
            g_autoptr(SrApp) probe = NULL;
            SrPackage *package;

            if (sr_dpkg_index_get_owner (dpkg, host_path) != NULL)
                continue;

            if (host_is_symlink (host_path)) {
                g_autofree char *sandbox_path = sr_host_to_sandbox_path (host_path);
                g_autofree char *link = g_file_read_link (sandbox_path, NULL);

                if (link == NULL)
                    continue;
                target = g_path_is_absolute (link) ? g_canonicalize_filename (link, NULL)
                                                   : g_canonicalize_filename (link, dirs[d]);
                if (is_claimed_target (target, claimed))
                    continue;
            }

            probe = sr_bundle_probe_binary (host_path);
            if (probe == NULL)
                continue;

            package = add_package (out, SR_PACKAGE_MANAGER_MANUAL, NULL, name, NULL, probe->comment, host_path);
            package->icon = probe->icon != NULL ? g_object_ref (probe->icon) : NULL;
            sr_package_add_detail (package, _("Points To"), target, TRUE);
            for (guint j = 0; j < probe->details->len; j++) {
                SrDetail *detail = g_ptr_array_index (probe->details, j);
                sr_package_add_detail (package, detail->title, detail->value, detail->is_path);
            }
        }
    }
}

GPtrArray *
sr_package_scanner_scan (SrDpkgIndex *dpkg, GCancellable *cancellable)
{
    GPtrArray *out = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr(GPtrArray) prefixes = g_ptr_array_new_with_free_func ((GDestroyNotify) node_prefix_free);

    scan_snaps (out);
    scan_flatpak (out);
    if (g_cancellable_is_cancelled (cancellable))
        return out;

    scan_node_versions (out, prefixes);
    scan_npm_globals (out, prefixes);
    scan_pnpm (out);
    scan_bun_deno (out);
    scan_rust (out);
    scan_go (out);
    scan_python (out);
    scan_homebrew (out);
    scan_loose_programs (out, dpkg);

    return out;
}

GPtrArray *
sr_package_scanner_get_watched_paths (void)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

    g_ptr_array_add (paths, g_strdup ("/snap"));
    g_ptr_array_add (paths, g_strdup ("/var/lib/flatpak/runtime"));
    g_ptr_array_add (paths, home_path (".local", "share", "flatpak", "runtime", NULL));
    g_ptr_array_add (paths, home_path (".local", "share", "fnm", "node-versions", NULL));
    g_ptr_array_add (paths, home_path (".nvm", "versions", "node", NULL));
    g_ptr_array_add (paths, sr_host_to_sandbox_path ("/usr/local/lib/node_modules"));
    g_ptr_array_add (paths, home_path (".local", "share", "pnpm", NULL));
    g_ptr_array_add (paths, home_path (".local", "share", "pnpm", "global", NULL));
    g_ptr_array_add (paths, home_path (".bun", "install", "global", NULL));
    g_ptr_array_add (paths, home_path (".rustup", "toolchains", NULL));
    g_ptr_array_add (paths, home_path (".cargo", NULL));
    g_ptr_array_add (paths, home_path ("go", "bin", NULL));
    g_ptr_array_add (paths, home_path (".local", "share", "pipx", "venvs", NULL));
    g_ptr_array_add (paths, home_path (".local", "bin", NULL));
    g_ptr_array_add (paths, home_path ("bin", NULL));
    g_ptr_array_add (paths, sr_host_to_sandbox_path ("/usr/local/bin"));

    return paths;
}

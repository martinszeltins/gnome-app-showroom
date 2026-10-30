/* sr-window.c
 *
 * Copyright 2026 Martins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sr-window"

#include "config.h"

#include <glib/gi18n.h>
#include <string.h>

#include "sr-app-card.h"
#include "sr-app.h"
#include "sr-catalog.h"
#include "sr-details-dialog.h"
#include "sr-package.h"
#include "sr-utils.h"
#include "sr-window.h"

#define PAGE_ALL "all"
#define PAGE_PACKAGES "packages"
#define RELOAD_DELAY_SECONDS 2
#define PACKAGE_PREVIEW_LIMIT 12

typedef struct {
    SrSource source;
    GtkFilterListModel *source_model;
    GtkFilterListModel *view_model;
    GtkWidget *box;
    GtkWidget *header;
    GtkLabel *count;
    GtkFlowBox *flowbox;
    GtkLabel *sidebar_count;
} Section;

struct _SrWindow {
    AdwApplicationWindow parent_instance;

    AdwNavigationSplitView *split_view;
    AdwNavigationPage *content_page;
    GtkListBox *sidebar_list;
    GtkSearchBar *search_bar;
    GtkSearchEntry *search_entry;
    GtkStack *stack;
    GtkBox *sections_box;
    AdwToggleGroup *package_filter;
    GtkDropDown *package_source;
    GtkLabel *packages_count;
    GtkGridView *package_grid;
    AdwStatusPage *empty_page;

    GSettings *settings;
    GCancellable *cancellable;
    GListStore *apps;
    GListStore *packages;
    GtkFilter *search_filter;
    GtkFilter *package_search_filter;
    GtkFilter *manual_filter;
    /* SrPackageGroup for each dropdown entry, -1 for all sources */
    GArray *source_groups;
    int selected_group;
    gboolean updating_sources;
    GtkFilterListModel *package_view_model;
    GtkSortListModel *package_page_model;
    /* Packages matching the search, shown on the All Apps page */
    GtkFilterListModel *package_matches;
    GtkSliceListModel *package_preview;
    GtkSorter *package_relevance_sorter;
    GtkWidget *package_matches_box;
    GtkLabel *package_matches_count;
    GtkButton *package_matches_button;
    char *query;

    Section sections[SR_N_SOURCES];
    GtkLabel *all_sidebar_count;
    GtkLabel *packages_sidebar_count;

    char *page;
    GPtrArray *monitors;
    guint reload_id;
    gboolean loaded;
    gboolean reload_pending;
};

G_DEFINE_FINAL_TYPE (SrWindow, sr_window, ADW_TYPE_APPLICATION_WINDOW)

static void update_view (SrWindow *self);
static void load_catalog (SrWindow *self);
static void select_page (SrWindow *self, const char *page);
static void on_search_changed (SrWindow *self);

static gboolean
is_packages_page (SrWindow *self)
{
    return g_strcmp0 (self->page, PAGE_PACKAGES) == 0;
}

static gboolean
is_all_page (SrWindow *self)
{
    return g_strcmp0 (self->page, PAGE_ALL) == 0;
}

static gboolean
source_filter_func (gpointer item, gpointer user_data)
{
    return SR_APP (item)->source == (SrSource) GPOINTER_TO_INT (user_data);
}

static gboolean
search_filter_func (gpointer item, gpointer user_data)
{
    SrWindow *self = SR_WINDOW (user_data);

    return sr_search_key_matches (SR_APP (item)->search_key, self->query);
}

static gboolean
package_search_filter_func (gpointer item, gpointer user_data)
{
    SrWindow *self = SR_WINDOW (user_data);

    return sr_search_key_matches (SR_PACKAGE (item)->search_key, self->query);
}

static gboolean
manual_filter_func (gpointer item, gpointer user_data)
{
    SrWindow *self = SR_WINDOW (user_data);

    SrPackage *package = SR_PACKAGE (item);

    if (self->selected_group >= 0 && (int) sr_package_get_group (package) != self->selected_group)
        return FALSE;

    if (g_strcmp0 (adw_toggle_group_get_active_name (self->package_filter), "manual") != 0)
        return TRUE;

    return !package->auto_installed;
}

/* Rebuilds the source dropdown with per-source counts, hiding sources
 * with nothing installed and keeping the current choice when possible */
static void
update_package_sources (SrWindow *self)
{
    guint counts[SR_N_PACKAGE_GROUPS] = { 0 };
    guint n_packages = g_list_model_get_n_items (G_LIST_MODEL (self->packages));
    g_autoptr(GtkStringList) strings = gtk_string_list_new (NULL);
    g_autofree char *all = NULL;
    guint selected = 0;

    for (guint i = 0; i < n_packages; i++) {
        g_autoptr(SrPackage) package = g_list_model_get_item (G_LIST_MODEL (self->packages), i);
        counts[sr_package_get_group (package)]++;
    }

    g_array_set_size (self->source_groups, 0);

    all = g_strdup_printf (_("All Sources (%u)"), n_packages);
    gtk_string_list_append (strings, all);
    g_array_append_val (self->source_groups, (int) { -1 });

    for (int group = 0; group < SR_N_PACKAGE_GROUPS; group++) {
        g_autofree char *label = NULL;

        if (counts[group] == 0)
            continue;

        if (group == self->selected_group)
            selected = self->source_groups->len;

        label = g_strdup_printf ("%s (%u)", sr_package_group_get_label (group), counts[group]);
        gtk_string_list_append (strings, label);
        g_array_append_val (self->source_groups, group);
    }

    if (selected == 0)
        self->selected_group = -1;

    self->updating_sources = TRUE;
    gtk_drop_down_set_model (self->package_source, G_LIST_MODEL (strings));
    gtk_drop_down_set_selected (self->package_source, selected);
    self->updating_sources = FALSE;
}

static void
dialog_closed_cb (AdwDialog *dialog, SrWindow *self)
{
    gtk_search_bar_set_key_capture_widget (self->search_bar, GTK_WIDGET (self));
}

static void
present_dialog (SrWindow *self, AdwDialog *dialog)
{
    /* Typing inside the dialog must not open the search bar behind it */
    gtk_search_bar_set_key_capture_widget (self->search_bar, NULL);
    g_signal_connect_object (dialog, "closed", G_CALLBACK (dialog_closed_cb), self, 0);
    adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
show_app_details (SrWindow *self, SrApp *app)
{
    present_dialog (self, ADW_DIALOG (sr_details_dialog_new_for_app (app)));
}

static void
show_package_details (SrWindow *self, SrPackage *package)
{
    present_dialog (self, ADW_DIALOG (sr_details_dialog_new_for_package (package)));
}

static GtkWidget *
create_card_cb (gpointer item, gpointer user_data)
{
    GtkWidget *card = sr_app_card_new ();

    sr_app_card_set_app (SR_APP_CARD (card), SR_APP (item));
    return card;
}

static void
flowbox_child_activated_cb (GtkFlowBox *flowbox, GtkFlowBoxChild *child, SrWindow *self)
{
    Section *section = g_object_get_data (G_OBJECT (flowbox), "section");
    g_autoptr(SrApp) app = NULL;

    app = g_list_model_get_item (G_LIST_MODEL (section->view_model), gtk_flow_box_child_get_index (child));
    if (app != NULL)
        show_app_details (self, app);
}

static void
section_items_changed_cb (GListModel *model, guint position, guint removed, guint added, SrWindow *self)
{
    update_view (self);
}

static GtkWidget *
create_package_card_cb (gpointer item, gpointer user_data)
{
    GtkWidget *card = sr_app_card_new ();

    sr_app_card_set_package (SR_APP_CARD (card), SR_PACKAGE (item));
    return card;
}

static void
package_match_activated_cb (GtkFlowBox *flowbox, GtkFlowBoxChild *child, SrWindow *self)
{
    g_autoptr(SrPackage) package = NULL;

    package = g_list_model_get_item (G_LIST_MODEL (self->package_preview), gtk_flow_box_child_get_index (child));
    if (package != NULL)
        show_package_details (self, package);
}

static GtkWidget *
create_sidebar_row (const char *page, const char *icon_name, const char *label, GtkLabel **count_out)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
    GtkWidget *title = gtk_label_new (label);
    GtkWidget *count = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_widget_add_css_class (count, "sidebar-count");

    gtk_box_append (GTK_BOX (box), image);
    gtk_box_append (GTK_BOX (box), title);
    gtk_box_append (GTK_BOX (box), count);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);

    g_object_set_data_full (G_OBJECT (row), "page", g_strdup (page), g_free);
    *count_out = GTK_LABEL (count);

    return row;
}

static void
sidebar_header_func (GtkListBoxRow *row, GtkListBoxRow *before, gpointer user_data)
{
    const char *page = g_object_get_data (G_OBJECT (row), "page");

    if (g_strcmp0 (page, PAGE_PACKAGES) == 0 && gtk_list_box_row_get_header (row) == NULL) {
        GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

        gtk_widget_set_margin_top (separator, 6);
        gtk_widget_set_margin_bottom (separator, 6);
        gtk_widget_set_margin_start (separator, 6);
        gtk_widget_set_margin_end (separator, 6);
        gtk_list_box_row_set_header (row, separator);
    }
}

static void
build_sidebar (SrWindow *self)
{
    GtkWidget *row;

    row = create_sidebar_row (PAGE_ALL, "view-app-grid-symbolic", _("All Apps"), &self->all_sidebar_count);
    gtk_list_box_append (self->sidebar_list, row);

    for (guint i = 0; i < SR_N_SOURCES; i++) {
        row = create_sidebar_row (sr_source_get_id (i), sr_source_get_icon_name (i), sr_source_get_label (i),
                                  &self->sections[i].sidebar_count);
        gtk_widget_set_tooltip_text (row, sr_source_get_description (i));
        gtk_list_box_append (self->sidebar_list, row);
    }

    row = create_sidebar_row (PAGE_PACKAGES, "package-x-generic-symbolic", _("Installed Packages"),
                              &self->packages_sidebar_count);
    gtk_list_box_append (self->sidebar_list, row);

    gtk_list_box_set_header_func (self->sidebar_list, sidebar_header_func, NULL, NULL);
}

static void
build_sections (SrWindow *self)
{
    for (guint i = 0; i < SR_N_SOURCES; i++) {
        Section *section = &self->sections[i];
        GtkFilter *source_filter;
        GtkWidget *header, *title, *count, *description, *heading, *flowbox;

        section->source = i;

        source_filter = GTK_FILTER (gtk_custom_filter_new (source_filter_func, GINT_TO_POINTER (i), NULL));
        section->source_model = gtk_filter_list_model_new (g_object_ref (G_LIST_MODEL (self->apps)), source_filter);
        section->view_model = gtk_filter_list_model_new (g_object_ref (G_LIST_MODEL (section->source_model)),
                                                         g_object_ref (self->search_filter));
        g_signal_connect_object (section->view_model, "items-changed", G_CALLBACK (section_items_changed_cb), self,
                                 G_CONNECT_DEFAULT);

        section->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);

        header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        title = g_object_new (GTK_TYPE_LABEL, "label", sr_source_get_label (i), "accessible-role",
                              GTK_ACCESSIBLE_ROLE_HEADING, NULL);
        gtk_widget_add_css_class (title, "section-title");
        count = gtk_label_new (NULL);
        gtk_widget_add_css_class (count, "section-count");
        gtk_box_append (GTK_BOX (header), title);
        gtk_box_append (GTK_BOX (header), count);
        gtk_widget_set_margin_start (header, 2);

        description = gtk_label_new (sr_source_get_description (i));
        gtk_label_set_xalign (GTK_LABEL (description), 0);
        gtk_label_set_wrap (GTK_LABEL (description), TRUE);
        gtk_widget_set_margin_start (description, 2);
        gtk_widget_add_css_class (description, "section-description");

        heading = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append (GTK_BOX (heading), header);
        gtk_box_append (GTK_BOX (heading), description);

        flowbox = gtk_flow_box_new ();
        gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flowbox), TRUE);
        gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flowbox), GTK_SELECTION_NONE);
        gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flowbox), 1);
        gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flowbox), 4);
        gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flowbox), 12);
        gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flowbox), 12);
        gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flowbox), TRUE);
        gtk_widget_set_valign (flowbox, GTK_ALIGN_START);
        gtk_widget_add_css_class (flowbox, "app-grid");
        gtk_flow_box_bind_model (GTK_FLOW_BOX (flowbox), G_LIST_MODEL (section->view_model), create_card_cb, NULL,
                                 NULL);
        g_object_set_data (G_OBJECT (flowbox), "section", section);
        g_signal_connect_object (flowbox, "child-activated", G_CALLBACK (flowbox_child_activated_cb), self,
                                 G_CONNECT_DEFAULT);

        gtk_box_append (GTK_BOX (section->box), heading);
        gtk_box_append (GTK_BOX (section->box), flowbox);
        gtk_box_append (self->sections_box, section->box);

        section->header = header;
        section->count = GTK_LABEL (count);
        section->flowbox = GTK_FLOW_BOX (flowbox);
    }
}

static void
show_all_packages_clicked_cb (GtkButton *button, SrWindow *self)
{
    adw_toggle_group_set_active_name (self->package_filter, "all");
    gtk_drop_down_set_selected (self->package_source, 0);
    select_page (self, PAGE_PACKAGES);
}

static int
package_rank (SrPackage *package, const char *query)
{
    if (query == NULL || *query == '\0')
        return 0;
    if (g_str_equal (package->name, query))
        return 0;
    if (g_str_has_prefix (package->name, query))
        return 1;
    if (strstr (package->name, query) != NULL)
        return 2;
    return 3;
}

static int
package_relevance_func (gconstpointer a, gconstpointer b, gpointer user_data)
{
    SrWindow *self = SR_WINDOW (user_data);
    SrPackage *pa = SR_PACKAGE ((gpointer) a);
    SrPackage *pb = SR_PACKAGE ((gpointer) b);
    int ra = package_rank (pa, self->query);
    int rb = package_rank (pb, self->query);

    if (ra != rb)
        return ra < rb ? GTK_ORDERING_SMALLER : GTK_ORDERING_LARGER;
    if (self->query != NULL && *self->query != '\0' && pa->is_default != pb->is_default)
        return pa->is_default ? GTK_ORDERING_SMALLER : GTK_ORDERING_LARGER;

    return gtk_ordering_from_cmpfunc (sr_package_compare (&pa, &pb));
}

static void
build_package_matches (SrWindow *self)
{
    GtkWidget *header, *title, *count, *button, *flowbox;
    GtkSortListModel *sorted;

    self->package_matches = gtk_filter_list_model_new (g_object_ref (G_LIST_MODEL (self->packages)),
                                                       g_object_ref (self->package_search_filter));
    self->package_relevance_sorter = GTK_SORTER (gtk_custom_sorter_new (package_relevance_func, self, NULL));
    sorted = gtk_sort_list_model_new (g_object_ref (G_LIST_MODEL (self->package_matches)),
                                      g_object_ref (self->package_relevance_sorter));
    self->package_preview = gtk_slice_list_model_new (G_LIST_MODEL (sorted), 0, PACKAGE_PREVIEW_LIMIT);
    g_signal_connect_object (self->package_matches, "items-changed", G_CALLBACK (section_items_changed_cb), self,
                             G_CONNECT_DEFAULT);

    header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (header, 2);
    title = g_object_new (GTK_TYPE_LABEL, "label", _("Installed Packages"), "accessible-role",
                          GTK_ACCESSIBLE_ROLE_HEADING, NULL);
    gtk_widget_add_css_class (title, "section-title");
    count = gtk_label_new (NULL);
    gtk_widget_add_css_class (count, "section-count");
    gtk_widget_set_hexpand (count, TRUE);
    gtk_label_set_xalign (GTK_LABEL (count), 0);
    button = gtk_button_new ();
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    g_signal_connect_object (button, "clicked", G_CALLBACK (show_all_packages_clicked_cb), self, G_CONNECT_DEFAULT);
    gtk_box_append (GTK_BOX (header), title);
    gtk_box_append (GTK_BOX (header), count);
    gtk_box_append (GTK_BOX (header), button);

    flowbox = gtk_flow_box_new ();
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flowbox), TRUE);
    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flowbox), 1);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flowbox), 4);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flowbox), 12);
    gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flowbox), 12);
    gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flowbox), TRUE);
    gtk_widget_set_valign (flowbox, GTK_ALIGN_START);
    gtk_widget_add_css_class (flowbox, "app-grid");
    gtk_flow_box_bind_model (GTK_FLOW_BOX (flowbox), G_LIST_MODEL (self->package_preview), create_package_card_cb,
                             NULL, NULL);
    g_signal_connect_object (flowbox, "child-activated", G_CALLBACK (package_match_activated_cb), self,
                             G_CONNECT_DEFAULT);

    self->package_matches_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append (GTK_BOX (self->package_matches_box), header);
    gtk_box_append (GTK_BOX (self->package_matches_box), flowbox);
    gtk_box_append (self->sections_box, self->package_matches_box);

    self->package_matches_count = GTK_LABEL (count);
    self->package_matches_button = GTK_BUTTON (button);
}

static void
package_setup_cb (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    gtk_list_item_set_child (item, sr_app_card_new ());
}

static void
package_bind_cb (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
    sr_app_card_set_package (SR_APP_CARD (gtk_list_item_get_child (item)), gtk_list_item_get_item (item));
}

static void
build_packages (SrWindow *self)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    GtkFilterListModel *manual_model;
    GtkNoSelection *selection;

    manual_model = gtk_filter_list_model_new (g_object_ref (G_LIST_MODEL (self->packages)),
                                              g_object_ref (self->manual_filter));
    self->package_view_model
        = gtk_filter_list_model_new (G_LIST_MODEL (manual_model), g_object_ref (self->package_search_filter));
    g_signal_connect_object (self->package_view_model, "items-changed", G_CALLBACK (section_items_changed_cb), self,
                             G_CONNECT_DEFAULT);

    g_signal_connect (factory, "setup", G_CALLBACK (package_setup_cb), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (package_bind_cb), NULL);

    self->package_page_model = gtk_sort_list_model_new (g_object_ref (G_LIST_MODEL (self->package_view_model)),
                                                        g_object_ref (self->package_relevance_sorter));
    selection = gtk_no_selection_new (g_object_ref (G_LIST_MODEL (self->package_page_model)));
    gtk_grid_view_set_model (self->package_grid, GTK_SELECTION_MODEL (selection));
    gtk_grid_view_set_factory (self->package_grid, factory);
    g_object_unref (selection);
    g_object_unref (factory);
}

static void
set_count (GtkLabel *label, guint count)
{
    g_autofree char *text = g_strdup_printf ("%u", count);

    gtk_label_set_label (label, text);
}

static const char *
page_title (SrWindow *self)
{
    if (is_all_page (self))
        return _("All Apps");
    if (is_packages_page (self))
        return _("Installed Packages");

    for (guint i = 0; i < SR_N_SOURCES; i++) {
        if (g_strcmp0 (self->page, sr_source_get_id (i)) == 0)
            return sr_source_get_label (i);
    }

    return _("All Apps");
}

static void
show_empty (SrWindow *self, const char *icon_name, const char *title, const char *description)
{
    adw_status_page_set_icon_name (self->empty_page, icon_name);
    adw_status_page_set_title (self->empty_page, title);
    adw_status_page_set_description (self->empty_page, description);
    gtk_stack_set_visible_child_name (self->stack, "empty");
}

static void
update_view (SrWindow *self)
{
    gboolean searching = self->query != NULL && *self->query != '\0';
    guint visible_total = 0;

    adw_navigation_page_set_title (self->content_page, page_title (self));
    gtk_search_entry_set_placeholder_text (self->search_entry,
                                           is_packages_page (self) ? _("Search packages") : _("Search apps"));

    set_count (self->all_sidebar_count, g_list_model_get_n_items (G_LIST_MODEL (self->apps)));
    set_count (self->packages_sidebar_count, g_list_model_get_n_items (G_LIST_MODEL (self->packages)));

    for (guint i = 0; i < SR_N_SOURCES; i++) {
        Section *section = &self->sections[i];
        guint n_visible = g_list_model_get_n_items (G_LIST_MODEL (section->view_model));
        gboolean on_page = is_all_page (self) || g_strcmp0 (self->page, sr_source_get_id (i)) == 0;

        set_count (section->sidebar_count, g_list_model_get_n_items (G_LIST_MODEL (section->source_model)));
        set_count (section->count, n_visible);
        gtk_widget_set_visible (section->box, on_page && n_visible > 0);
        gtk_widget_set_visible (section->header, is_all_page (self));

        if (on_page)
            visible_total += n_visible;
    }

    if (self->package_matches != NULL) {
        guint n_matches = g_list_model_get_n_items (G_LIST_MODEL (self->package_matches));
        gboolean show_matches = is_all_page (self) && searching && n_matches > 0;

        gtk_widget_set_visible (self->package_matches_box, show_matches);
        if (show_matches) {
            g_autofree char *label = NULL;

            set_count (self->package_matches_count, n_matches);
            gtk_widget_set_visible (GTK_WIDGET (self->package_matches_button), n_matches > PACKAGE_PREVIEW_LIMIT);
            label = g_strdup_printf (ngettext ("Show All %u Package", "Show All %u Packages", n_matches), n_matches);
            gtk_button_set_label (self->package_matches_button, label);
            visible_total += n_matches;
        }
    }

    if (!self->loaded) {
        gtk_stack_set_visible_child_name (self->stack, "loading");
        return;
    }

    if (is_packages_page (self)) {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->package_view_model));
        g_autofree char *text = g_strdup_printf (ngettext ("%u package", "%u packages", n), n);

        gtk_label_set_label (self->packages_count, text);

        if (n > 0)
            gtk_stack_set_visible_child_name (self->stack, "packages");
        else if (searching)
            show_empty (self, "edit-find-symbolic", _("No Results Found"), _("Try a different search"));
        else if (g_list_model_get_n_items (G_LIST_MODEL (self->packages)) == 0)
            show_empty (self, "package-x-generic-symbolic", _("No Packages Found"),
                        _("This system has no dpkg package database"));
        else
            gtk_stack_set_visible_child_name (self->stack, "packages");
        return;
    }

    if (visible_total > 0) {
        gtk_stack_set_visible_child_name (self->stack, "apps");
    } else if (searching) {
        show_empty (self, "edit-find-symbolic", _("No Results Found"), _("Try a different search"));
    } else {
        const char *title = _("No Apps Found");
        const char *description = NULL;

        for (guint i = 0; i < SR_N_SOURCES; i++) {
            if (g_strcmp0 (self->page, sr_source_get_id (i)) == 0) {
                title = sr_source_get_empty_title (i);
                description = sr_source_get_description (i);
            }
        }

        show_empty (self, "preferences-desktop-apps-symbolic", title, description);
    }
}

static void
select_page (SrWindow *self, const char *page)
{
    for (int i = 0;; i++) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index (self->sidebar_list, i);

        if (row == NULL)
            break;

        if (g_strcmp0 (g_object_get_data (G_OBJECT (row), "page"), page) == 0) {
            gtk_list_box_select_row (self->sidebar_list, row);
            g_set_str (&self->page, page);
            break;
        }
    }

    if (self->page == NULL)
        self->page = g_strdup (PAGE_ALL);

    update_view (self);
}

static void
on_sidebar_row_activated (SrWindow *self, GtkListBoxRow *row)
{
    const char *page = g_object_get_data (G_OBJECT (row), "page");

    g_set_str (&self->page, page);

    /* search-changed is delayed, so apply the cleared query right away */
    gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
    gtk_search_bar_set_search_mode (self->search_bar, FALSE);
    on_search_changed (self);

    adw_navigation_split_view_set_show_content (self->split_view, TRUE);
}

static void
on_search_changed (SrWindow *self)
{
    const char *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));

    g_free (self->query);
    self->query = sr_normalize_casefold_and_unaccent (g_strstrip (g_strdup (text)));

    gtk_filter_changed (self->search_filter, GTK_FILTER_CHANGE_DIFFERENT);
    gtk_filter_changed (self->package_search_filter, GTK_FILTER_CHANGE_DIFFERENT);
    gtk_sorter_changed (self->package_relevance_sorter, GTK_SORTER_CHANGE_DIFFERENT);
    if (g_list_model_get_n_items (G_LIST_MODEL (self->package_page_model)) > 0)
        gtk_grid_view_scroll_to (self->package_grid, 0, GTK_LIST_SCROLL_NONE, NULL);
    update_view (self);
}

static void
on_search_stopped (SrWindow *self)
{
    gtk_search_bar_set_search_mode (self->search_bar, FALSE);
}

static void
on_search_mode_changed (SrWindow *self)
{
    if (!gtk_search_bar_get_search_mode (self->search_bar))
        gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
}

static void
on_search_activated (SrWindow *self)
{
    if (is_packages_page (self)) {
        g_autoptr(SrPackage) package = g_list_model_get_item (G_LIST_MODEL (self->package_page_model), 0);

        if (package != NULL)
            show_package_details (self, package);
        return;
    }

    for (guint i = 0; i < SR_N_SOURCES; i++) {
        Section *section = &self->sections[i];
        g_autoptr(SrApp) app = NULL;

        if (!gtk_widget_get_visible (section->box))
            continue;

        app = g_list_model_get_item (G_LIST_MODEL (section->view_model), 0);
        if (app != NULL) {
            show_app_details (self, app);
            return;
        }
    }

    if (gtk_widget_get_visible (self->package_matches_box)) {
        g_autoptr(SrPackage) package = g_list_model_get_item (G_LIST_MODEL (self->package_preview), 0);

        if (package != NULL)
            show_package_details (self, package);
    }
}

static void
on_package_filter_changed (SrWindow *self)
{
    guint selected;

    if (self->updating_sources || self->manual_filter == NULL)
        return;

    selected = gtk_drop_down_get_selected (self->package_source);
    if (self->source_groups != NULL && selected < self->source_groups->len)
        self->selected_group = g_array_index (self->source_groups, int, selected);
    else
        self->selected_group = -1;

    gtk_filter_changed (self->manual_filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_package_activated (SrWindow *self, guint position)
{
    g_autoptr(SrPackage) package = g_list_model_get_item (G_LIST_MODEL (self->package_page_model), position);

    if (package != NULL)
        show_package_details (self, package);
}

static void
splice_store (GListStore *store, GPtrArray *items)
{
    g_list_store_splice (store, 0, g_list_model_get_n_items (G_LIST_MODEL (store)), items->pdata, items->len);
}

static void
catalog_loaded_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
    SrWindow *self = user_data;
    g_autoptr(SrCatalog) catalog = NULL;
    g_autoptr(GError) error = NULL;

    catalog = sr_catalog_load_finish (result, &error);
    if (catalog == NULL) {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning ("Failed to load apps: %s", error->message);
            g_clear_object (&self->cancellable);
            self->loaded = TRUE;
            update_view (self);
        }
        g_object_unref (self);
        return;
    }

    splice_store (self->apps, catalog->apps);
    splice_store (self->packages, catalog->packages);
    update_package_sources (self);
    gtk_filter_changed (self->manual_filter, GTK_FILTER_CHANGE_DIFFERENT);
    self->loaded = TRUE;
    update_view (self);

    g_clear_object (&self->cancellable);
    if (self->reload_pending) {
        self->reload_pending = FALSE;
        load_catalog (self);
    }

    g_object_unref (self);
}

static void
load_catalog (SrWindow *self)
{
    g_auto(GStrv) standalone_dirs = NULL;
    g_auto(GStrv) appimage_only_dirs = NULL;

    if (self->cancellable != NULL) {
        self->reload_pending = TRUE;
        return;
    }

    standalone_dirs = g_settings_get_strv (self->settings, "standalone-dirs");
    appimage_only_dirs = g_settings_get_strv (self->settings, "appimage-only-dirs");

    self->cancellable = g_cancellable_new ();
    sr_catalog_load_async ((const char *const *) standalone_dirs, (const char *const *) appimage_only_dirs,
                           self->cancellable, catalog_loaded_cb, g_object_ref (self));
}

static gboolean
reload_timeout_cb (gpointer user_data)
{
    SrWindow *self = user_data;

    self->reload_id = 0;
    load_catalog (self);

    return G_SOURCE_REMOVE;
}

static void
monitor_changed_cb (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, SrWindow *self)
{
    if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
        return;

    g_clear_handle_id (&self->reload_id, g_source_remove);
    self->reload_id = g_timeout_add_seconds (RELOAD_DELAY_SECONDS, reload_timeout_cb, self);
}

static void
setup_monitors (SrWindow *self)
{
    g_auto(GStrv) standalone_dirs = g_settings_get_strv (self->settings, "standalone-dirs");
    g_auto(GStrv) appimage_only_dirs = g_settings_get_strv (self->settings, "appimage-only-dirs");
    g_autoptr(GPtrArray) paths = NULL;

    g_clear_pointer (&self->monitors, g_ptr_array_unref);
    self->monitors = g_ptr_array_new_with_free_func (g_object_unref);

    paths = sr_catalog_get_watched_paths ((const char *const *) standalone_dirs,
                                          (const char *const *) appimage_only_dirs);
    for (guint i = 0; i < paths->len; i++) {
        g_autoptr(GFile) file = g_file_new_for_path (g_ptr_array_index (paths, i));
        g_autoptr(GError) error = NULL;
        GFileMonitor *monitor;

        monitor = g_file_monitor (file, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
        if (monitor == NULL) {
            g_debug ("Not watching %s: %s", (const char *) g_ptr_array_index (paths, i), error->message);
            continue;
        }

        g_signal_connect_object (monitor, "changed", G_CALLBACK (monitor_changed_cb), self, G_CONNECT_DEFAULT);
        g_ptr_array_add (self->monitors, monitor);
    }
}

static void
settings_dirs_changed_cb (GSettings *settings, const char *key, SrWindow *self)
{
    setup_monitors (self);
    sr_window_reload (self);
}

void
sr_window_reload (SrWindow *self)
{
    g_return_if_fail (SR_IS_WINDOW (self));

    g_clear_handle_id (&self->reload_id, g_source_remove);
    load_catalog (self);
}

static void
reload_action (GtkWidget *widget, const char *action_name, GVariant *parameter)
{
    sr_window_reload (SR_WINDOW (widget));
}

static void
search_action (GtkWidget *widget, const char *action_name, GVariant *parameter)
{
    SrWindow *self = SR_WINDOW (widget);

    gtk_search_bar_set_search_mode (self->search_bar, TRUE);
    gtk_widget_grab_focus (GTK_WIDGET (self->search_entry));
}

static void
sr_window_dispose (GObject *object)
{
    SrWindow *self = SR_WINDOW (object);

    g_cancellable_cancel (self->cancellable);
    g_clear_handle_id (&self->reload_id, g_source_remove);
    g_clear_pointer (&self->monitors, g_ptr_array_unref);

    for (guint i = 0; i < SR_N_SOURCES; i++) {
        g_clear_object (&self->sections[i].source_model);
        g_clear_object (&self->sections[i].view_model);
    }
    g_clear_object (&self->package_page_model);
    g_clear_object (&self->package_view_model);
    g_clear_object (&self->package_preview);
    g_clear_object (&self->package_matches);
    g_clear_object (&self->package_relevance_sorter);

    gtk_widget_dispose_template (GTK_WIDGET (self), SR_TYPE_WINDOW);

    G_OBJECT_CLASS (sr_window_parent_class)->dispose (object);
}

static void
sr_window_finalize (GObject *object)
{
    SrWindow *self = SR_WINDOW (object);

    g_clear_object (&self->settings);
    g_clear_object (&self->cancellable);
    g_clear_object (&self->apps);
    g_clear_object (&self->packages);
    g_clear_object (&self->search_filter);
    g_clear_object (&self->package_search_filter);
    g_clear_object (&self->manual_filter);
    g_clear_pointer (&self->source_groups, g_array_unref);
    g_free (self->query);
    g_free (self->page);

    G_OBJECT_CLASS (sr_window_parent_class)->finalize (object);
}

static void
sr_window_class_init (SrWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose = sr_window_dispose;
    object_class->finalize = sr_window_finalize;

    gtk_widget_class_set_template_from_resource (widget_class, RESOURCE_BASE "/sr-window.ui");
    gtk_widget_class_bind_template_child (widget_class, SrWindow, split_view);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, content_page);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, sidebar_list);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, search_bar);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, search_entry);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, stack);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, sections_box);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, package_filter);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, package_source);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, packages_count);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, package_grid);
    gtk_widget_class_bind_template_child (widget_class, SrWindow, empty_page);

    gtk_widget_class_bind_template_callback (widget_class, on_sidebar_row_activated);
    gtk_widget_class_bind_template_callback (widget_class, on_search_changed);
    gtk_widget_class_bind_template_callback (widget_class, on_search_stopped);
    gtk_widget_class_bind_template_callback (widget_class, on_search_mode_changed);
    gtk_widget_class_bind_template_callback (widget_class, on_search_activated);
    gtk_widget_class_bind_template_callback (widget_class, on_package_filter_changed);
    gtk_widget_class_bind_template_callback (widget_class, on_package_activated);

    gtk_widget_class_install_action (widget_class, "win.reload", NULL, reload_action);
    gtk_widget_class_install_action (widget_class, "win.search", NULL, search_action);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_r, GDK_CONTROL_MASK, "win.reload", NULL);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_F5, 0, "win.reload", NULL);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_f, GDK_CONTROL_MASK, "win.search", NULL);
}

static void
sr_window_init (SrWindow *self)
{
    g_type_ensure (SR_TYPE_APP_CARD);

    self->apps = g_list_store_new (SR_TYPE_APP);
    self->packages = g_list_store_new (SR_TYPE_PACKAGE);
    self->search_filter = GTK_FILTER (gtk_custom_filter_new (search_filter_func, self, NULL));
    self->package_search_filter = GTK_FILTER (gtk_custom_filter_new (package_search_filter_func, self, NULL));
    self->source_groups = g_array_new (FALSE, FALSE, sizeof (int));
    self->selected_group = -1;

    gtk_widget_init_template (GTK_WIDGET (self));

    self->manual_filter = GTK_FILTER (gtk_custom_filter_new (manual_filter_func, self, NULL));

    self->settings = g_settings_new (APPLICATION_ID);
    g_settings_bind (self->settings, "window-width", self, "default-width", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind (self->settings, "window-height", self, "default-height", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind (self->settings, "window-maximized", self, "maximized", G_SETTINGS_BIND_DEFAULT);

    build_sidebar (self);
    build_sections (self);
    build_package_matches (self);
    build_packages (self);
    update_package_sources (self);

    /* Typing anywhere in the window starts a search */
    gtk_search_bar_set_key_capture_widget (self->search_bar, GTK_WIDGET (self));
    gtk_search_bar_connect_entry (self->search_bar, GTK_EDITABLE (self->search_entry));

    select_page (self, PAGE_ALL);

    g_signal_connect_object (self->settings, "changed::standalone-dirs", G_CALLBACK (settings_dirs_changed_cb), self,
                             G_CONNECT_DEFAULT);
    g_signal_connect_object (self->settings, "changed::appimage-only-dirs", G_CALLBACK (settings_dirs_changed_cb),
                             self, G_CONNECT_DEFAULT);

    setup_monitors (self);
    load_catalog (self);
}

SrWindow *
sr_window_new (AdwApplication *application)
{
    return g_object_new (SR_TYPE_WINDOW, "application", application, NULL);
}

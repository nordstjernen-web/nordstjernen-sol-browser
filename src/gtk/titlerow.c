/* Nordstjernen — title bar row that centres the browser name beside the tabs. */

#include "titlerow.h"

#define NS_TITLE_ROW_CONTROLS_GAP 6
#define NS_TITLE_ROW_NAME_GAP 24
#define NS_TITLE_ROW_TRAILING_GAP 10
#define NS_TITLE_ROW_FULL_NAME "Nordstjernen Sol Explorer " NS_VERSION
#define NS_TITLE_ROW_SHORT_NAME "Nordstjernen Sol Explorer"

typedef struct {
    GtkWidget *start_controls;
    GtkWidget *tabstrip;
    GtkWidget *full_name;
    GtkWidget *short_name;
    GtkWidget *end_controls;
} NsTitleRow;

static NsTitleRow *
title_row_parts(GtkWidget *row)
{
    return g_object_get_data(G_OBJECT(row), "ns-title-row");
}

static void
measure_width(GtkWidget *w, int *minimum, int *natural)
{
    gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, minimum, natural,
                       NULL, NULL);
}

static int
natural_width(GtkWidget *w)
{
    int natural = 0;
    measure_width(w, NULL, &natural);
    return natural;
}

static int
controls_width(GtkWidget *controls)
{
    if (!gtk_widget_get_visible(controls))
        return 0;
    return natural_width(controls) + NS_TITLE_ROW_CONTROLS_GAP;
}

static void
title_row_measure(GtkWidget *row, GtkOrientation orientation, int for_size,
                  int *minimum, int *natural, int *minimum_baseline,
                  int *natural_baseline)
{
    (void)for_size;
    (void)minimum_baseline;
    (void)natural_baseline;
    NsTitleRow *r = title_row_parts(row);
    *minimum = *natural = 0;
    if (orientation == GTK_ORIENTATION_VERTICAL) {
        GtkWidget *children[] = { r->start_controls, r->tabstrip,
                                  r->full_name, r->short_name,
                                  r->end_controls };
        for (guint i = 0; i < G_N_ELEMENTS(children); i++) {
            int min = 0, nat = 0;
            gtk_widget_measure(children[i], GTK_ORIENTATION_VERTICAL, -1,
                               &min, &nat, NULL, NULL);
            *minimum = MAX(*minimum, min);
            *natural = MAX(*natural, nat);
        }
        return;
    }
    int tabs_min = 0, tabs_nat = 0;
    measure_width(r->tabstrip, &tabs_min, &tabs_nat);
    int edges = controls_width(r->start_controls) +
                controls_width(r->end_controls) + NS_TITLE_ROW_NAME_GAP +
                NS_TITLE_ROW_TRAILING_GAP;
    *minimum = edges + tabs_min + natural_width(r->short_name);
    *natural = edges + tabs_nat + natural_width(r->full_name);
}

static void
title_row_place(GtkWidget *row, GtkWidget *child, int x, int width,
                int height)
{
    if (gtk_widget_get_direction(row) == GTK_TEXT_DIR_RTL)
        x = gtk_widget_get_width(row) - x - width;
    gtk_widget_size_allocate(child, &(GtkAllocation){ x, 0, width, height },
                             -1);
}

static int
centred_name_x(GtkWidget *row, int width, int name_width)
{
    int origin = 0, span = width;
    GtkWidget *header = gtk_widget_get_ancestor(row, GTK_TYPE_HEADER_BAR);
    graphene_point_t p;
    if (header && gtk_widget_compute_point(row, header,
                                           &GRAPHENE_POINT_INIT(0, 0), &p)) {
        span = gtk_widget_get_width(header);
        origin = (int)p.x;
        if (gtk_widget_get_direction(row) == GTK_TEXT_DIR_RTL)
            origin = span - origin - width;
    }
    return (span - name_width) / 2 - origin;
}

static void
title_row_allocate(GtkWidget *row, int width, int height, int baseline)
{
    (void)baseline;
    NsTitleRow *r = title_row_parts(row);
    int left = 0, right = width;
    if (gtk_widget_get_visible(r->start_controls)) {
        int w = natural_width(r->start_controls);
        title_row_place(row, r->start_controls, left, w, height);
        left += w + NS_TITLE_ROW_CONTROLS_GAP;
    }
    if (gtk_widget_get_visible(r->end_controls)) {
        int w = natural_width(r->end_controls);
        title_row_place(row, r->end_controls, right - w, w, height);
        right -= w + NS_TITLE_ROW_CONTROLS_GAP;
    }

    int tabs_min = 0, tabs_nat = 0;
    measure_width(r->tabstrip, &tabs_min, &tabs_nat);
    int full_width = natural_width(r->full_name);
    int room = right - left - NS_TITLE_ROW_NAME_GAP -
               NS_TITLE_ROW_TRAILING_GAP;
    gboolean full = tabs_nat + full_width <= room;
    GtkWidget *name = full ? r->full_name : r->short_name;
    int name_width = full ? full_width : natural_width(r->short_name);
    int tabs_width = CLAMP(room - name_width, tabs_min, tabs_nat);
    title_row_place(row, r->tabstrip, left, tabs_width, height);

    int name_x = centred_name_x(row, width, name_width);
    if (name_x < left + tabs_width + NS_TITLE_ROW_NAME_GAP ||
        name_x + name_width > right - NS_TITLE_ROW_TRAILING_GAP)
        name_x = right - NS_TITLE_ROW_TRAILING_GAP - name_width;
    gtk_widget_set_child_visible(r->full_name, full);
    gtk_widget_set_child_visible(r->short_name, !full);
    title_row_place(row, name, name_x, name_width, height);
}

static GtkWidget *
title_row_controls(GtkWidget *row, GtkPackType side)
{
    GtkWidget *controls = gtk_window_controls_new(side);
    g_object_bind_property(controls, "empty", controls, "visible",
                           G_BINDING_SYNC_CREATE |
                               G_BINDING_INVERT_BOOLEAN);
    gtk_widget_set_parent(controls, row);
    return controls;
}

static GtkWidget *
title_row_name(GtkWidget *row, const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "ns-brand-title");
    gtk_widget_set_can_target(label, FALSE);
    gtk_widget_set_parent(label, row);
    return label;
}

GtkWidget *
ns_title_row_new(GtkWidget *tabstrip)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_layout_manager(
        row, gtk_custom_layout_new(NULL, title_row_measure,
                                   title_row_allocate));
    gtk_widget_set_hexpand(row, TRUE);
    NsTitleRow *r = g_new0(NsTitleRow, 1);
    r->start_controls = title_row_controls(row, GTK_PACK_START);
    r->tabstrip = tabstrip;
    gtk_widget_set_parent(tabstrip, row);
    r->full_name = title_row_name(row, NS_TITLE_ROW_FULL_NAME);
    r->short_name = title_row_name(row, NS_TITLE_ROW_SHORT_NAME);
    r->end_controls = title_row_controls(row, GTK_PACK_END);
    g_object_set_data_full(G_OBJECT(row), "ns-title-row", r, g_free);
    return row;
}

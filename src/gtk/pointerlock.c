/* pointerlock.c - hides, confines and re-centres the mouse pointer for a page that locked it. */
#include "pointerlock.h"
#include "i18n.h"

#include <math.h>

#ifdef GDK_WINDOWING_WIN32
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#endif

#define NS_POINTER_LOCK_NOTICE_SECONDS 4

struct NsPointerLock {
    GtkWidget *area;
    GtkWidget *notice;
    guint      notice_timeout;
    gboolean   active;
    double     anchor_x, anchor_y;
    double     last_x, last_y;
    double     rest_x, rest_y;
#ifdef GDK_WINDOWING_WIN32
    gboolean   warping;
    POINT      centre;
    double     scale;
#endif
};

static void
pl_hide_notice(NsPointerLock *pl)
{
    if (pl->notice_timeout)
        g_source_remove(pl->notice_timeout);
    pl->notice_timeout = 0;
    if (pl->notice)
        gtk_widget_set_visible(pl->notice, FALSE);
}

static gboolean
pl_notice_expired(gpointer data)
{
    NsPointerLock *pl = data;
    pl->notice_timeout = 0;
    pl_hide_notice(pl);
    return G_SOURCE_REMOVE;
}

static void
pl_show_notice(NsPointerLock *pl)
{
    if (!pl->notice) return;
    pl_hide_notice(pl);
    gtk_widget_set_visible(pl->notice, TRUE);
    pl->notice_timeout = g_timeout_add_seconds(NS_POINTER_LOCK_NOTICE_SECONDS,
                                               pl_notice_expired, pl);
}

NsPointerLock *
ns_pointer_lock_new(GtkWidget *area, GtkWidget *overlay)
{
    NsPointerLock *pl = g_new0(NsPointerLock, 1);
    pl->area = area;
    if (overlay && GTK_IS_OVERLAY(overlay)) {
        pl->notice = gtk_label_new(
            ns_i18n("This page has taken over the mouse pointer. "
                    "Press Esc to get it back."));
        gtk_widget_add_css_class(pl->notice, "osd");
        gtk_widget_set_halign(pl->notice, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(pl->notice, GTK_ALIGN_START);
        gtk_widget_set_margin_top(pl->notice, 24);
        gtk_widget_set_can_target(pl->notice, FALSE);
        gtk_widget_set_visible(pl->notice, FALSE);
        g_object_ref(pl->notice);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), pl->notice);
    }
    return pl;
}

#ifdef GDK_WINDOWING_WIN32
static HWND
pl_hwnd(NsPointerLock *pl, GtkNative **native_out)
{
    GtkNative *native = gtk_widget_get_native(pl->area);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if (!surface || !GDK_IS_WIN32_SURFACE(surface)) return NULL;
    *native_out = native;
    return gdk_win32_surface_get_handle(surface);
}

static gboolean
pl_confine(NsPointerLock *pl)
{
    GtkNative *native = NULL;
    HWND hwnd = pl_hwnd(pl, &native);
    graphene_rect_t bounds;
    if (!hwnd ||
        !gtk_widget_compute_bounds(pl->area, GTK_WIDGET(native), &bounds))
        return FALSE;
    double sx = 0, sy = 0;
    gtk_native_get_surface_transform(native, &sx, &sy);
    pl->scale = gdk_surface_get_scale(gtk_native_get_surface(native));
    if (!(pl->scale > 0)) pl->scale = 1.0;
    RECT r = {
        (LONG)floor((bounds.origin.x + sx) * pl->scale),
        (LONG)floor((bounds.origin.y + sy) * pl->scale),
        (LONG)ceil((bounds.origin.x + sx + bounds.size.width) * pl->scale),
        (LONG)ceil((bounds.origin.y + sy + bounds.size.height) * pl->scale),
    };
    POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    RECT screen = { tl.x, tl.y, br.x, br.y };
    ClipCursor(&screen);
    pl->centre.x = (tl.x + br.x) / 2;
    pl->centre.y = (tl.y + br.y) / 2;
    SetCursorPos(pl->centre.x, pl->centre.y);
    return TRUE;
}
#endif

void
ns_pointer_lock_engage(NsPointerLock *pl, double x, double y)
{
    if (!pl || pl->active) return;
    pl->active = TRUE;
    pl->anchor_x = pl->last_x = x;
    pl->anchor_y = pl->last_y = y;
    pl->rest_x = pl->rest_y = 0;
    gtk_widget_set_cursor_from_name(pl->area, "none");
#ifdef GDK_WINDOWING_WIN32
    pl->warping = pl_confine(pl);
#endif
    pl_show_notice(pl);
}

void
ns_pointer_lock_release(NsPointerLock *pl)
{
    if (!pl || !pl->active) return;
    pl->active = FALSE;
#ifdef GDK_WINDOWING_WIN32
    if (pl->warping) ClipCursor(NULL);
    pl->warping = FALSE;
#endif
    gtk_widget_set_cursor(pl->area, NULL);
    pl_hide_notice(pl);
}

gboolean
ns_pointer_lock_active(const NsPointerLock *pl)
{
    return pl && pl->active;
}

static int
pl_take_whole(double *rest, double delta)
{
    *rest += delta;
    double whole = trunc(*rest);
    *rest -= whole;
    return (int)whole;
}

gboolean
ns_pointer_lock_motion(NsPointerLock *pl, double x, double y, int *dx, int *dy)
{
    if (!pl || !pl->active) return FALSE;
    double mx = x - pl->last_x, my = y - pl->last_y;
    pl->last_x = x;
    pl->last_y = y;
#ifdef GDK_WINDOWING_WIN32
    if (pl->warping) {
        POINT p;
        if (!GetCursorPos(&p)) return FALSE;
        mx = (double)(p.x - pl->centre.x) / pl->scale;
        my = (double)(p.y - pl->centre.y) / pl->scale;
        if (p.x != pl->centre.x || p.y != pl->centre.y)
            pl_confine(pl);
    }
#endif
    *dx = pl_take_whole(&pl->rest_x, mx);
    *dy = pl_take_whole(&pl->rest_y, my);
    return TRUE;
}

void
ns_pointer_lock_anchor(const NsPointerLock *pl, double *x, double *y)
{
    *x = pl ? pl->anchor_x : 0;
    *y = pl ? pl->anchor_y : 0;
}

void
ns_pointer_lock_free(NsPointerLock *pl)
{
    if (!pl) return;
    ns_pointer_lock_release(pl);
    pl_hide_notice(pl);
    g_clear_object(&pl->notice);
    g_free(pl);
}

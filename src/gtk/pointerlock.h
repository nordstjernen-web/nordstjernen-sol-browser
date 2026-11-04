/* pointerlock.h - hides, confines and re-centres the mouse pointer for a page that locked it. */
#ifndef NS_GTK_POINTERLOCK_H
#define NS_GTK_POINTERLOCK_H

#include <gtk/gtk.h>

typedef struct NsPointerLock NsPointerLock;

NsPointerLock *ns_pointer_lock_new(GtkWidget *area, GtkWidget *overlay);
void           ns_pointer_lock_free(NsPointerLock *pl);
void           ns_pointer_lock_engage(NsPointerLock *pl, double x, double y);
void           ns_pointer_lock_release(NsPointerLock *pl);
gboolean       ns_pointer_lock_active(const NsPointerLock *pl);
gboolean       ns_pointer_lock_motion(NsPointerLock *pl, double x, double y,
                                      int *dx, int *dy);
void           ns_pointer_lock_anchor(const NsPointerLock *pl, double *x,
                                      double *y);

#endif

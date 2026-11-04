/* Nordstjernen — persistent tab sessions kept in the profile's session.db. */

#ifndef NS_SESSIONSTORE_H
#define NS_SESSIONSTORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    char *url;
    char *title;
    int   scroll_x;
    int   scroll_y;
} ns_session_entry;

typedef struct {
    gint64     id;
    int        window;
    int        position;
    gboolean   active;
    int        index;
    GPtrArray *entries;
    char      *storage;
} ns_session_tab;

gboolean    ns_session_open(const char *instance_id);
void        ns_session_close(void);
gboolean    ns_session_available(void);

GPtrArray  *ns_session_take_tabs(gboolean own_only);
void        ns_session_tab_free(ns_session_tab *tab);

gint64      ns_session_tab_add(void);
void        ns_session_tab_save(const ns_session_tab *tab);
void        ns_session_tab_remove(gint64 id);

G_END_DECLS

#endif

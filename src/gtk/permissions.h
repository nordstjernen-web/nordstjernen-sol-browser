/* Nordstjernen — shell-owned per-site permission ledger shared by every tab. */

#ifndef NS_PERMISSIONS_H
#define NS_PERMISSIONS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    NS_PERM_CAMERA,
    NS_PERM_WEBGL,
    NS_PERM_N_KINDS
} ns_perm_kind;

typedef enum {
    NS_PERM_ASK,
    NS_PERM_ALLOW,
    NS_PERM_DENY
} ns_perm_state;

typedef void (*ns_perm_listener)(const char *origin, ns_perm_kind kind,
                                 ns_perm_state state, ns_perm_state previous,
                                 gpointer user_data);

void          ns_perm_init(gboolean persistent);
void          ns_perm_shutdown(void);

ns_perm_state ns_perm_get(const char *origin, ns_perm_kind kind);
void          ns_perm_set(const char *origin, ns_perm_kind kind,
                          ns_perm_state state);

char         *ns_perm_serialize(void);
char         *ns_perm_entry_line(const char *origin, ns_perm_kind kind,
                                 ns_perm_state state);

const char   *ns_perm_kind_name(ns_perm_kind kind);
gboolean      ns_perm_kind_from_name(const char *name, ns_perm_kind *out);

guint         ns_perm_add_listener(ns_perm_listener fn, gpointer user_data);
void          ns_perm_remove_listener(guint id);

G_END_DECLS

#endif

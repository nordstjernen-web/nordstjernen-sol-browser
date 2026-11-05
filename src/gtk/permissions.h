/* Nordstjernen — shell-owned per-site capability ledger shared by every tab. */

#ifndef NS_PERMISSIONS_H
#define NS_PERMISSIONS_H

#include <glib.h>

#include "capability.h"

G_BEGIN_DECLS

typedef ns_capability ns_perm_kind;
typedef ns_cap_state  ns_perm_state;

#define NS_PERM_ASK     NS_CAP_ASK
#define NS_PERM_ALLOW   NS_CAP_ALLOW
#define NS_PERM_DENY    NS_CAP_DENY
#define NS_PERM_N_KINDS NS_CAP_COUNT

typedef void (*ns_perm_listener)(const char *origin, ns_perm_kind kind,
                                 ns_perm_state state, ns_perm_state previous,
                                 gpointer user_data);

void          ns_perm_init(gboolean persistent);
void          ns_perm_shutdown(void);

ns_perm_state ns_perm_get(const char *origin, ns_perm_kind kind);
void          ns_perm_set(const char *origin, ns_perm_kind kind,
                          ns_perm_state state);

char         *ns_perm_serialize_origin(const char *origin);
char         *ns_perm_entry_line(const char *origin, ns_perm_kind kind,
                                 ns_perm_state state);

guint         ns_perm_add_listener(ns_perm_listener fn, gpointer user_data);
void          ns_perm_remove_listener(guint id);

G_END_DECLS

#endif

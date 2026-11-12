/* Nordstjernen — Connection-Allowlist response header: parsing and URL pattern matching. */

#ifndef NS_CONNECTION_ALLOWLIST_H
#define NS_CONNECTION_ALLOWLIST_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_conn_allowlist ns_conn_allowlist;

ns_conn_allowlist *ns_conn_allowlist_parse(const char *header,
                                           const char *response_origin);
void               ns_conn_allowlist_free(ns_conn_allowlist *list);
gboolean           ns_conn_allowlist_allows(const ns_conn_allowlist *list,
                                            const char *url);

G_END_DECLS

#endif

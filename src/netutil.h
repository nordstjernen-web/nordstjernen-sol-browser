/* Nordstjernen — address-bar, language and proxy-mask helpers shared by the shell and the engine. */

#ifndef NS_NETUTIL_H
#define NS_NETUTIL_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    NS_SEC_NONE = 0,
    NS_SEC_SECURE,
    NS_SEC_INVALID,
    NS_SEC_PLAIN,
    NS_SEC_DANGEROUS
} ns_security;

const char *ns_net_default_accept_language(void);
const char *ns_net_effective_accept_language(void);
char      **ns_net_navigator_languages(void);

char *ns_net_proxy_mask(const char *proxy_url);

gboolean ns_address_is_search(const char *s);
char    *ns_search_url_for(const char *query);
char    *ns_search_engine_name(void);
char    *ns_url_from_local_path(const char *path);

G_END_DECLS

#endif

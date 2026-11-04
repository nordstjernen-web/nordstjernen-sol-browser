/* Nordstjernen — browser-owned cookie store: in-memory partitions backed by one SQLite database. */

#ifndef NS_COOKIES_H
#define NS_COOKIES_H

#include <glib.h>

G_BEGIN_DECLS

void     ns_cookie_store_init(const char *dir, gboolean in_memory);
void     ns_cookie_store_shutdown(void);
void     ns_cookie_store_flush(void);
void     ns_cookie_store_flush_soon(void);

gboolean ns_cookie_store_set(const char *partition, const char *url,
                             const char *set_cookie, gboolean from_http);
char    *ns_cookie_store_header(const char *partition, const char *url,
                                int samesite_cross, gboolean include_httponly);
guint    ns_cookie_store_generation(void);

void     ns_cookie_store_clear(void);
void     ns_cookie_store_clear_site(const char *partition);
char    *ns_cookie_store_sites_json(void);
char    *ns_cookie_store_site_json(const char *partition);
char    *ns_cookie_store_visible_json(const char *partition, const char *url);

G_END_DECLS

#endif

/* Nordstjernen — Google Safe Browsing v5 local-list client. */

#ifndef NS_SAFEBROWSING_GOOGLE_H
#define NS_SAFEBROWSING_GOOGLE_H

#include "safebrowsing.h"

G_BEGIN_DECLS

gboolean      ns_sbg_available(void);
void          ns_sbg_start(void);
ns_sb_verdict ns_sbg_check_url(const char *url);
gboolean      ns_sbg_have_key(void);
void          ns_sbg_get_stats(ns_sb_stats *out);

G_END_DECLS

#endif

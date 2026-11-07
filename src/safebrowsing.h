/* Nordstjernen — phishing/malware checks (local list and Google Safe Browsing) and warning interstitial. */

#ifndef NS_SAFEBROWSING_H
#define NS_SAFEBROWSING_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_UNSAFE_CONTINUE_SCHEME "nordstjernen-unsafe-continue:"

typedef enum {
    NS_SB_SAFE,
    NS_SB_LOCAL_LIST,
    NS_SB_MALWARE,
    NS_SB_SOCIAL_ENGINEERING,
    NS_SB_UNWANTED_SOFTWARE,
} ns_sb_verdict;

gboolean      ns_safebrowsing_blocked(const char *host);
ns_sb_verdict ns_safebrowsing_check_url(const char *url);
void          ns_safebrowsing_allow_host(const char *host);
char         *ns_safebrowsing_interstitial(const char *url, const char *host,
                                           ns_sb_verdict verdict);

G_END_DECLS

#endif

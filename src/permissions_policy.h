/* Nordstjernen — Permissions Policy: the feature list and allowlist matching for the header and the iframe allow attribute. */

#ifndef NS_PERMISSIONS_POLICY_H
#define NS_PERMISSIONS_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    NS_PP_UNDECLARED,
    NS_PP_DENIED,
    NS_PP_ALLOWED
} ns_pp_verdict;

gsize        ns_pp_feature_count(void);
const char  *ns_pp_feature_name(gsize index);
gboolean     ns_pp_feature_known(const char *feature);
gboolean     ns_pp_feature_default_all(const char *feature);

ns_pp_verdict ns_pp_header_verdict(const char *header, const char *feature,
                                   const char *self_origin,
                                   const char *origin);
ns_pp_verdict ns_pp_allow_attr_verdict(const char *allow, const char *feature,
                                       const char *self_origin,
                                       const char *src_origin,
                                       const char *origin);
char        **ns_pp_header_allowlist(const char *header, const char *feature,
                                     const char *self_origin);

G_END_DECLS

#endif

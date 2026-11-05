/* Nordstjernen — the capability vocabulary shared by the shell and the engine. */

#ifndef NS_CAPABILITY_H
#define NS_CAPABILITY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    NS_CAP_CAMERA,
    NS_CAP_WEBGL,
    NS_CAP_COUNT
} ns_capability;

typedef enum {
    NS_CAP_ASK,
    NS_CAP_ALLOW,
    NS_CAP_DENY
} ns_cap_state;

typedef struct {
    const char  *name;
    const char  *label;
    const char  *prompt;
    ns_cap_state default_state;
    gboolean     reload_on_change;
} ns_capability_info;

const ns_capability_info *ns_capability_info_get(ns_capability cap);
const char *ns_capability_name(ns_capability cap);
gboolean    ns_capability_from_name(const char *name, ns_capability *out);
const char *ns_cap_state_name(ns_cap_state state);
ns_cap_state ns_cap_state_from_name(const char *name);

G_END_DECLS

#endif

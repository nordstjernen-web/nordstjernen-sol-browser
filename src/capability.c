/* Nordstjernen — the capability vocabulary shared by the shell and the engine. */

#include "capability.h"

#include <string.h>

static const ns_capability_info capabilities[NS_CAP_COUNT] = {
    [NS_CAP_CAMERA] = {
        .name = "camera",
        .label = "Camera and microphone",
        .prompt = "This site wants to use your camera and microphone",
        .default_state = NS_CAP_ASK,
        .reload_on_change = FALSE,
    },
    [NS_CAP_WEBGL] = {
        .name = "webgl",
        .label = "WebGL (3D graphics)",
        .prompt = "This site wants to use WebGL (3D graphics)",
        .default_state = NS_CAP_ALLOW,
        .reload_on_change = TRUE,
    },
    [NS_CAP_DOWNLOADS] = {
        .name = "downloads",
        .label = "Downloads",
        .prompt = "This site wants to download a file",
        .default_state = NS_CAP_ALLOW,
        .reload_on_change = FALSE,
    },
    [NS_CAP_POPUPS] = {
        .name = "popups",
        .label = "Pop-up windows",
        .prompt = "This site wants to open a new window",
        .default_state = NS_CAP_ALLOW,
        .reload_on_change = FALSE,
    },
};

const ns_capability_info *
ns_capability_info_get(ns_capability cap)
{
    return cap < NS_CAP_COUNT ? &capabilities[cap] : NULL;
}

const char *
ns_capability_name(ns_capability cap)
{
    return cap < NS_CAP_COUNT ? capabilities[cap].name : "";
}

gboolean
ns_capability_from_name(const char *name, ns_capability *out)
{
    for (int k = 0; name && k < NS_CAP_COUNT; k++) {
        if (strcmp(name, capabilities[k].name) == 0) {
            *out = (ns_capability)k;
            return TRUE;
        }
    }
    return FALSE;
}

const char *
ns_cap_state_name(ns_cap_state state)
{
    switch (state) {
    case NS_CAP_ALLOW: return "allow";
    case NS_CAP_DENY:  return "deny";
    default:           return "ask";
    }
}

ns_cap_state
ns_cap_state_from_name(const char *name)
{
    if (g_strcmp0(name, "allow") == 0) return NS_CAP_ALLOW;
    if (g_strcmp0(name, "deny") == 0) return NS_CAP_DENY;
    return NS_CAP_ASK;
}

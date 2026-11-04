/* Nordstjernen — webcam capture (V4L2) and per-site camera permission. */

#ifndef NS_CAMERA_H
#define NS_CAMERA_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_camera ns_camera;

typedef struct ns_camera_info {
    char *device;
    char *label;
} ns_camera_info;

ns_camera  *ns_camera_open(const char *device);
ns_texture *ns_camera_next_frame(ns_camera *cam);

ns_camera  *ns_camera_acquire(void);
void        ns_camera_release(void);
ns_camera  *ns_camera_active(void);
const char *ns_camera_device(const ns_camera *cam);
void        ns_camera_close(ns_camera *cam);

GPtrArray  *ns_camera_enumerate(void);
void        ns_camera_info_free(ns_camera_info *info);

typedef struct ns_js ns_js;

int      ns_camera_permission(ns_js *js);
char    *ns_camera_take_pending_origin(void);
void     ns_camera_set_decision(const char *origin, int allow);

enum { NS_CAMERA_ASK = 0, NS_CAMERA_ALLOW = 1, NS_CAMERA_DENY = 2 };
int      ns_camera_set_state(const char *origin, int state);
void     ns_camera_clear_decisions(void);

G_END_DECLS

#endif

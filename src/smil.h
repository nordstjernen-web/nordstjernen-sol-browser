/* Nordstjernen — SVG SMIL animation timeline API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#ifndef NS_SMIL_H
#define NS_SMIL_H
#include <glib.h>
#include "dom.h"
G_BEGIN_DECLS

#define NS_SMIL_MOTION_ATTR "\001motion"

typedef struct ns_smil ns_smil;

ns_smil *ns_smil_new(const ns_node *doc, gint64 now_us);
void     ns_smil_free(ns_smil *s);
gboolean ns_smil_tick(ns_smil *s, guint64 dom_serial, gint64 now_us);
gboolean ns_smil_has_active(const ns_smil *s);

typedef enum ns_smil_event_kind {
    NS_SMIL_EVENT_BEGIN,
    NS_SMIL_EVENT_END,
    NS_SMIL_EVENT_REPEAT,
} ns_smil_event_kind;

typedef gboolean (*ns_smil_event_cb)(const ns_node *node,
                                     ns_smil_event_kind kind, int iteration,
                                     gpointer user);
const char *ns_smil_event_type(ns_smil_event_kind kind);
void     ns_smil_drain_events(ns_smil *s, ns_smil_event_cb cb, gpointer user);
void     ns_smil_settle_events(ns_smil *s);
void     ns_smil_dom_event(const ns_node *target, const char *type);

void        ns_smil_refresh(const ns_node *n);
const char *ns_smil_attr(const ns_node *n, const char *name);
gboolean    ns_smil_node_animated(const ns_node *n);

void     ns_smil_pause(const ns_node *svg, gboolean paused);
gboolean ns_smil_paused(const ns_node *svg);
double   ns_smil_current_time(const ns_node *el);
void     ns_smil_set_current_time(const ns_node *svg, double seconds);
gboolean ns_smil_begin_element(const ns_node *anim, double offset);
gboolean ns_smil_end_element(const ns_node *anim, double offset);
gboolean ns_smil_start_time(const ns_node *anim, double *out);
gboolean ns_smil_simple_duration(const ns_node *anim, double *out);
const ns_node *ns_smil_target(const ns_node *anim);

G_END_DECLS
#endif

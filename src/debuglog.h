/* Nordstjernen — in-process debug event log shared with the JS console.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_DEBUGLOG_H
#define NS_DEBUGLOG_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_dlog_level {
    NS_DLOG_INFO,
    NS_DLOG_WARN,
    NS_DLOG_ERROR,
    NS_DLOG_RENDER,
    NS_DLOG_NET,
    NS_DLOG_JS,
} ns_dlog_level;

typedef struct ns_dlog_entry {
    ns_dlog_level level;
    char         *category;
    char         *message;
} ns_dlog_entry;

typedef void (*ns_dlog_listener)(const ns_dlog_entry *entry, gpointer user_data);

void ns_debug_log_init(void);

void ns_debug_log_emit(ns_dlog_level level, const char *category,
                       const char *fmt, ...) G_GNUC_PRINTF(3, 4);

guint ns_debug_log_subscribe(ns_dlog_listener cb, gpointer user_data);
void  ns_debug_log_unsubscribe(guint id);


const char *ns_dlog_level_name(ns_dlog_level lvl);

const char *ns_debug_log_file_path(void);

G_END_DECLS

#endif

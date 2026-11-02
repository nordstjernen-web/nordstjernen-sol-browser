/* Nordstjernen — opt-in loopback control channel that lets a local MCP client drive the GUI. */

#ifndef NORDSTJERNEN_GTK_REMOTECTL_H
#define NORDSTJERNEN_GTK_REMOTECTL_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_REMOTE_CONTROL_FILE "remote-control.json"

char    *ns_remote_control_file_path(void);
void     ns_remote_control_request(void);
void     ns_remote_control_init(void);
void     ns_remote_control_stop(void);
void     ns_remote_control_shutdown(void);

G_END_DECLS

#endif

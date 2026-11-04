/* Nordstjernen — shared limits for the out-of-process renderer shells (GTK). */

#ifndef NS_PROC_LIMITS_H
#define NS_PROC_LIMITS_H

#define NS_PROC_MAX_WIDTH        6144
#define NS_PROC_MAX_HEIGHT       3456
#define NS_PROC_MAX_RESTARTS     3
#define NS_PROC_MAX_JS_REDIRECTS 20
#define NS_PROC_SETTLE_MS        400
#define NS_PROC_CONSOLE_POLL_MS  250

#define NS_PROC_ZOOM_MIN  0.25
#define NS_PROC_ZOOM_MAX  5.0

#define NS_PROC_RENDERER_ENV       "NS_RENDERER"
#define NS_PROC_SETTLE_ENV         "NS_SETTLE_MS"
#define NS_PROC_SINGLE_PROCESS_ENV "NS_SINGLE_PROCESS"
#define NS_PROC_RENDERER_NAME "nordstjernen-renderer"
#define NS_PROC_HEADLESS_FLAG "--headless-host"

#endif

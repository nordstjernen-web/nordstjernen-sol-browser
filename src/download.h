/* Nordstjernen — renderer-side download: streams a response into a file under the cache directory. */
#ifndef NS_DOWNLOAD_H
#define NS_DOWNLOAD_H

#include <glib.h>

typedef struct {
    int      state;
    guint64  bytes;
    guint64  total;
    char    *path;
    char    *final_url;
    char    *content_type;
    char    *error;
} ns_download_status;

enum {
    NS_DOWNLOAD_RUNNING = 0,
    NS_DOWNLOAD_DONE = 1,
    NS_DOWNLOAD_FAILED = 2,
};

long     ns_download_start(const char *url, const char *top_url);
gboolean ns_download_poll(long id, ns_download_status *out);
void     ns_download_cancel(long id);
void     ns_download_status_clear(ns_download_status *st);

#endif

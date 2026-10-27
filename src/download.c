/* Nordstjernen — renderer-side download: streams a response into a file under the cache directory. */
#include "download.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "net.h"

#define NS_DOWNLOAD_MAX_BYTES (16ULL << 30)

typedef struct {
    grefcount     rc;
    long          id;
    char         *path;
    FILE         *file;
    GCancellable *cancel;
    guint64       bytes;
    guint64       total;
    int           state;
    char         *final_url;
    char         *content_type;
    char         *error;
} ns_download;

static GHashTable *g_downloads;
static long        g_next_download_id;

static ns_download *
download_ref(ns_download *d)
{
    g_ref_count_inc(&d->rc);
    return d;
}

static void
download_unref(ns_download *d)
{
    if (!g_ref_count_dec(&d->rc)) return;
    if (d->file) fclose(d->file);
    g_clear_object(&d->cancel);
    g_free(d->path);
    g_free(d->final_url);
    g_free(d->content_type);
    g_free(d->error);
    g_free(d);
}

static void
download_fail(ns_download *d, const char *why)
{
    if (d->state != NS_DOWNLOAD_RUNNING) return;
    d->state = NS_DOWNLOAD_FAILED;
    d->error = g_strdup(why);
    if (d->file) {
        fclose(d->file);
        d->file = NULL;
    }
    g_unlink(d->path);
    g_cancellable_cancel(d->cancel);
}

static guint64
header_content_length(const char *raw_headers)
{
    if (!raw_headers) return 0;
    for (const char *p = raw_headers; *p;) {
        const char *eol = strpbrk(p, "\r\n");
        gsize len = eol ? (gsize)(eol - p) : strlen(p);
        if (len > 15 && g_ascii_strncasecmp(p, "content-length:", 15) == 0) {
            const char *v = p + 15;
            while (*v == ' ' || *v == '\t') v++;
            return g_ascii_strtoull(v, NULL, 10);
        }
        if (!eol) break;
        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }
    return 0;
}

static void
download_chunk(gpointer user_data, long status, const char *raw_headers,
               const guint8 *data, gsize len)
{
    ns_download *d = user_data;
    (void)status;
    if (d->state != NS_DOWNLOAD_RUNNING || !d->file) return;
    if (d->total == 0)
        d->total = header_content_length(raw_headers);
    if (d->bytes + len > NS_DOWNLOAD_MAX_BYTES) {
        download_fail(d, "file is larger than the download limit");
        return;
    }
    if (len && fwrite(data, 1, len, d->file) != len) {
        download_fail(d, "cannot write the download file");
        return;
    }
    d->bytes += len;
}

static void
download_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    ns_download *d = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (d->state == NS_DOWNLOAD_RUNNING) {
        gboolean flushed = d->file && fflush(d->file) == 0;
        if (d->file) {
            fclose(d->file);
            d->file = NULL;
        }
        if (!resp || resp->error)
            download_fail(d, resp && resp->error ? resp->error
                                                 : err ? err->message
                                                       : "download failed");
        else if (resp->status < 200 || resp->status >= 300)
            download_fail(d, "the server refused the download");
        else if (!flushed)
            download_fail(d, "cannot write the download file");
        else {
            d->state = NS_DOWNLOAD_DONE;
            d->final_url = g_strdup(resp->final_url ? resp->final_url : "");
            d->content_type = g_strdup(resp->content_type ? resp->content_type
                                                          : "");
        }
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    download_unref(d);
}

static char *
download_dir(void)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "nordstjernen",
                                 "downloads", NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

long
ns_download_start(const char *url, const char *top_url)
{
    if (!url || !*url) return 0;
    if (!g_downloads)
        g_downloads = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL,
                                            (GDestroyNotify)download_unref);
    g_autofree char *dir = download_dir();
    long id = ++g_next_download_id;
    char *path = g_strdup_printf("%s%c%d-%ld.part", dir, G_DIR_SEPARATOR,
                                 (int)getpid(), id);
    FILE *file = g_fopen(path, "wb");
    if (!file) {
        g_free(path);
        return 0;
    }
    g_chmod(path, 0600);
    ns_download *d = g_new0(ns_download, 1);
    g_ref_count_init(&d->rc);
    d->id = id;
    d->path = path;
    d->file = file;
    d->cancel = g_cancellable_new();
    g_hash_table_insert(g_downloads, GINT_TO_POINTER(id), d);
    ns_net_download_async(url, top_url, d->cancel, download_chunk, d,
                          download_finished, download_ref(d));
    return id;
}

gboolean
ns_download_poll(long id, ns_download_status *out)
{
    memset(out, 0, sizeof *out);
    ns_download *d = g_downloads
        ? g_hash_table_lookup(g_downloads, GINT_TO_POINTER(id)) : NULL;
    if (!d) return FALSE;
    out->state = d->state;
    out->bytes = d->bytes;
    out->total = d->total;
    out->path = g_strdup(d->path);
    out->final_url = g_strdup(d->final_url ? d->final_url : "");
    out->content_type = g_strdup(d->content_type ? d->content_type : "");
    out->error = g_strdup(d->error ? d->error : "");
    if (d->state != NS_DOWNLOAD_RUNNING)
        g_hash_table_remove(g_downloads, GINT_TO_POINTER(id));
    return TRUE;
}

void
ns_download_cancel(long id)
{
    ns_download *d = g_downloads
        ? g_hash_table_lookup(g_downloads, GINT_TO_POINTER(id)) : NULL;
    if (!d) return;
    download_fail(d, "cancelled");
    g_hash_table_remove(g_downloads, GINT_TO_POINTER(id));
}

void
ns_download_status_clear(ns_download_status *st)
{
    g_free(st->path);
    g_free(st->final_url);
    g_free(st->content_type);
    g_free(st->error);
    memset(st, 0, sizeof *st);
}

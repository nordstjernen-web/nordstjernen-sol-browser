/* Nordstjernen — webcam capture (V4L2) and per-site camera permission. */

#include "camera.h"

#include <string.h>

#include "image.h"
#include "js.h"
#include "net.h"

static GMutex      g_camera_lock;
static GHashTable *g_camera_decisions;
static char *g_camera_pending;

void
ns_camera_info_free(ns_camera_info *info)
{
    if (!info) return;
    g_free(info->device);
    g_free(info->label);
    g_free(info);
}

static void
ns_camera_record_decision(const char *origin, gboolean allow)
{
    if (!g_camera_decisions)
        g_camera_decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    g_hash_table_insert(g_camera_decisions, g_strdup(origin),
                        GINT_TO_POINTER(allow ? 1 : 2));
}

char *
ns_camera_take_pending_origin(void)
{
    g_mutex_lock(&g_camera_lock);
    char *origin = g_camera_pending;
    g_camera_pending = NULL;
    g_mutex_unlock(&g_camera_lock);
    return origin;
}

void
ns_camera_set_decision(const char *origin, int allow)
{
    ns_camera_set_state(origin, allow ? NS_CAMERA_ALLOW : NS_CAMERA_DENY);
}

int
ns_camera_set_state(const char *origin, int state)
{
    if (!origin || !*origin) return 0;
    g_mutex_lock(&g_camera_lock);
    gpointer prev = NULL;
    gboolean was_pending = FALSE;
    if (g_camera_decisions &&
        g_hash_table_lookup_extended(g_camera_decisions, origin, NULL, &prev))
        was_pending = GPOINTER_TO_INT(prev) == 3;
    if (state == NS_CAMERA_ALLOW || state == NS_CAMERA_DENY)
        ns_camera_record_decision(origin, state == NS_CAMERA_ALLOW);
    else if (g_camera_decisions && !was_pending)
        g_hash_table_remove(g_camera_decisions, origin);
    if (state != NS_CAMERA_ASK && g_camera_pending &&
        g_strcmp0(g_camera_pending, origin) == 0)
        g_clear_pointer(&g_camera_pending, g_free);
    g_mutex_unlock(&g_camera_lock);
    return was_pending && state != NS_CAMERA_ASK;
}

static gboolean
camera_decision_settled(gpointer key, gpointer value, gpointer user_data)
{
    (void)key; (void)user_data;
    return GPOINTER_TO_INT(value) != 3;
}

void
ns_camera_clear_decisions(void)
{
    g_mutex_lock(&g_camera_lock);
    if (g_camera_decisions)
        g_hash_table_foreach_remove(g_camera_decisions,
                                    camera_decision_settled, NULL);
    g_mutex_unlock(&g_camera_lock);
}

int
ns_camera_permission(ns_js *js)
{
    const char *url = ns_js_current_url(js);
    char *origin = ns_url_origin_from(url);
    gboolean opaque = !origin || !*origin || strcmp(origin, "null") == 0;
    if (opaque) {
        g_free(origin);
        origin = g_strdup(url && *url ? url : "this page");
    }

    g_mutex_lock(&g_camera_lock);
    if (!g_camera_decisions)
        g_camera_decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    gpointer prev = NULL;
    if (g_hash_table_lookup_extended(g_camera_decisions, origin, NULL, &prev)) {
        int v = GPOINTER_TO_INT(prev);
        if (opaque && v != 3)
            g_hash_table_remove(g_camera_decisions, origin);
        g_mutex_unlock(&g_camera_lock);
        g_free(origin);
        if (v == 1) return 1;
        if (v == 2) return 0;
        return -1;
    }

    g_hash_table_insert(g_camera_decisions, g_strdup(origin),
                        GINT_TO_POINTER(3));
    g_free(g_camera_pending);
    g_camera_pending = g_strdup(origin);
    g_mutex_unlock(&g_camera_lock);
    g_free(origin);
    return -1;
}

static ns_camera *g_active_camera;
static int g_active_camera_refs;

ns_camera *
ns_camera_acquire(void)
{
    if (!g_active_camera)
        g_active_camera = ns_camera_open(NULL);
    if (g_active_camera)
        g_active_camera_refs++;
    return g_active_camera;
}

void
ns_camera_release(void)
{
    if (g_active_camera_refs > 0 && --g_active_camera_refs == 0) {
        ns_camera_close(g_active_camera);
        g_active_camera = NULL;
    }
}

ns_camera *
ns_camera_active(void)
{
    return g_active_camera;
}

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NS_CAMERA_BUFFERS 4

typedef struct ns_camera_buf {
    void  *start;
    size_t length;
} ns_camera_buf;

struct ns_camera {
    int            fd;
    char          *device;
    int            width;
    int            height;
    guint32        pixfmt;
    ns_camera_buf  bufs[NS_CAMERA_BUFFERS];
    guint          n_bufs;
    gboolean       streaming;
};

static int
xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static void
yuyv_to_bgra(const guint8 *src, int width, int height, gsize src_stride,
             guint8 *dst, gsize dst_stride)
{
    for (int y = 0; y < height; y++) {
        const guint8 *s = src + (gsize)y * src_stride;
        guint8 *d = dst + (gsize)y * dst_stride;
        for (int x = 0; x + 1 < width; x += 2) {
            int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
            s += 4;
            for (int k = 0; k < 2; k++) {
                int yy = (k == 0 ? y0 : y1) - 16;
                int uu = u - 128, vv = v - 128;
                int r = (298 * yy + 409 * vv + 128) >> 8;
                int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
                int b = (298 * yy + 516 * uu + 128) >> 8;
                d[0] = (guint8)(b < 0 ? 0 : b > 255 ? 255 : b);
                d[1] = (guint8)(g < 0 ? 0 : g > 255 ? 255 : g);
                d[2] = (guint8)(r < 0 ? 0 : r > 255 ? 255 : r);
                d[3] = 255;
                d += 4;
            }
        }
    }
}

static void
ns_camera_unmap(ns_camera *cam)
{
    for (guint i = 0; i < cam->n_bufs; i++) {
        if (cam->bufs[i].start && cam->bufs[i].start != MAP_FAILED)
            munmap(cam->bufs[i].start, cam->bufs[i].length);
        cam->bufs[i].start = NULL;
    }
    cam->n_bufs = 0;
}

ns_camera *
ns_camera_open(const char *device)
{
    if (!device || !*device) device = "/dev/video0";
    int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        return NULL;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        memset(&fmt, 0, sizeof fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 640;
        fmt.fmt.pix.height = 480;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
            close(fd);
            return NULL;
        }
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG &&
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        close(fd);
        return NULL;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof req);
    req.count = NS_CAMERA_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
        close(fd);
        return NULL;
    }

    ns_camera *cam = g_new0(ns_camera, 1);
    cam->fd = fd;
    cam->device = g_strdup(device);
    cam->width = (int)fmt.fmt.pix.width;
    cam->height = (int)fmt.fmt.pix.height;
    cam->pixfmt = fmt.fmt.pix.pixelformat;

    for (guint i = 0; i < req.count && i < NS_CAMERA_BUFFERS; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) break;
        cam->bufs[i].length = buf.length;
        cam->bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, buf.m.offset);
        if (cam->bufs[i].start == MAP_FAILED) {
            cam->bufs[i].start = NULL;
            break;
        }
        cam->n_bufs++;
    }
    if (cam->n_bufs < 2) {
        ns_camera_close(cam);
        return NULL;
    }

    for (guint i = 0; i < cam->n_bufs; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ns_camera_close(cam);
            return NULL;
        }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ns_camera_close(cam);
        return NULL;
    }
    cam->streaming = TRUE;
    return cam;
}

ns_texture *
ns_camera_next_frame(ns_camera *cam)
{
    if (!cam || !cam->streaming) return NULL;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) != 0)
        return NULL;
    if (buf.index >= cam->n_bufs) return NULL;

    const guint8 *data = cam->bufs[buf.index].start;
    gsize used = buf.bytesused;
    ns_texture *tex = NULL;

    if (cam->pixfmt == V4L2_PIX_FMT_MJPEG) {
        int w = 0, h = 0;
        tex = ns_image_decode_bytes(data, used, &w, &h);
        if (tex) {
            cam->width = w;
            cam->height = h;
        }
    } else if (cam->pixfmt == V4L2_PIX_FMT_YUYV) {
        gsize stride = (gsize)cam->width * 4;
        gsize buf_len = stride * (gsize)cam->height;
        guint8 *bgra = g_malloc(buf_len);
        yuyv_to_bgra(data, cam->width, cam->height,
                     (gsize)cam->width * 2, bgra, stride);
        GBytes *bytes = g_bytes_new_take(bgra, buf_len);
        tex = ns_texture_new(cam->width, cam->height,
                             NS_TEXTURE_BGRA_PREMULTIPLIED, bytes, stride);
        g_bytes_unref(bytes);
    }

    xioctl(cam->fd, VIDIOC_QBUF, &buf);
    return tex;
}

const char *ns_camera_device(const ns_camera *cam) { return cam ? cam->device : NULL; }

void
ns_camera_close(ns_camera *cam)
{
    if (!cam) return;
    if (cam->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    }
    ns_camera_unmap(cam);
    if (cam->fd >= 0) close(cam->fd);
    g_free(cam->device);
    g_free(cam);
}

static char *
ns_camera_label_for(const char *device)
{
    int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    char *label = NULL;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
        (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
        cap.card[0])
        label = g_strdup((const char *)cap.card);
    close(fd);
    return label;
}

GPtrArray *
ns_camera_enumerate(void)
{
    GPtrArray *out = g_ptr_array_new();
    for (int i = 0; i < 64; i++) {
        char dev[32];
        g_snprintf(dev, sizeof dev, "/dev/video%d", i);
        if (!g_file_test(dev, G_FILE_TEST_EXISTS)) continue;
        char *label = ns_camera_label_for(dev);
        if (!label) continue;
        ns_camera_info *info = g_new0(ns_camera_info, 1);
        info->device = g_strdup(dev);
        info->label = label;
        g_ptr_array_add(out, info);
    }
    return out;
}

#else

struct ns_camera { int unused; };

ns_camera  *ns_camera_open(const char *device) { (void)device; return NULL; }
ns_texture *ns_camera_next_frame(ns_camera *cam) { (void)cam; return NULL; }
const char *ns_camera_device(const ns_camera *cam) { (void)cam; return NULL; }
void        ns_camera_close(ns_camera *cam) { (void)cam; }
GPtrArray  *ns_camera_enumerate(void) { return g_ptr_array_new(); }

#endif

# Embedding Nordstjernen in a C application

Nordstjernen ships a small C API for driving the engine from another
program: fetch a URL, run its scripts, lay it out, and pull back the
rendered text or a PNG/PDF image. There is no GTK window — it is the
same synchronous pipeline the `--headless` driver uses, exposed as a
library.

The public header is **`libnordstjernen.h`** (`src/libnordstjernen.h`,
installed as `<prefix>/include/nordstjernen/libnordstjernen.h`) and the
build installs a shared library (`libnordstjernen.so` / `.dll` /
`.dylib`). The header is plain C with no GLib or GTK types, so a consumer
only needs the header and the link flag. No pkg-config file is generated.

The same API drives the Android shell (see [`Android.md`](Android.md)).
For the JVM, the Java
binding in `java/` wraps it through a JNI bridge
(`org.nordstjernen.Nordstjernen`, JDK 21) — see
[`java/README.md`](../java/README.md).

## API

```c
int          ns_browser_init(void);
ns_browser  *ns_browser_open(const char *url, int viewport_width, int settle_ms);
char        *ns_browser_render_text(ns_browser *browser);
int          ns_browser_render_image(ns_browser *browser, const char *path);
void         ns_browser_close(ns_browser *browser);
void         ns_browser_shutdown(void);
```

| Function | Behaviour |
| --- | --- |
| `ns_browser_init` | Brings up networking, the disk cache, and the font system. Call once before anything else; returns `0` on success. Enables `file://` and bare local-path loading. |
| `ns_browser_open` | Fetches `url`, decodes and parses the HTML, runs its scripts, pumps the event loop for `settle_ms` milliseconds, then lays the page out at `viewport_width` pixels (height is derived). `url` may be `http(s)://`, `file://`, `about:`, `data:`, or a local filesystem path. Returns an opaque handle, or `NULL` on a hard fetch/parse failure. |
| `ns_browser_render_text` | Returns the rendered visible text as a newly-allocated, NUL-terminated UTF-8 string. **The caller frees it with `free()`.** Returns `NULL` if nothing was laid out. |
| `ns_browser_render_image` | Renders the full page to `path`. A `.pdf` extension writes PDF; anything else writes PNG. Images are fetched lazily on the first call. Returns `0` on success. |
| `ns_browser_close` | Frees a handle from `ns_browser_open`. |
| `ns_browser_shutdown` | Tears down the subsystems brought up by `ns_browser_init`. |

These six calls are the minimal fetch-and-render pipeline. The header also
declares the interactive surface the mobile shells and the out-of-process
renderer use — viewport rendering into a caller buffer
(`ns_browser_render_rgba`, `ns_browser_render_argb32`), input
(`ns_browser_click`, `ns_browser_key`, `ns_browser_hover`,
`ns_browser_scroll_at`), `ns_browser_tick` to keep scripts and animations
running after the initial settle, `ns_browser_eval`, find-in-page, and the
`ns_browser_take_pending_*` queues for navigation, downloads and other
page requests. Each is documented by its comment in `src/libnordstjernen.h`.

## Example

```c
#include <libnordstjernen.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (ns_browser_init() != 0)
        return 1;

    ns_browser *b = ns_browser_open("https://example.org", 1024, 500);
    if (b) {
        char *text = ns_browser_render_text(b);
        if (text) {
            fputs(text, stdout);
            free(text);
        }
        ns_browser_render_image(b, "page.png");
        ns_browser_close(b);
    }

    ns_browser_shutdown();
    return 0;
}
```

Build against the installed library:

```sh
cc app.c -I/usr/local/include/nordstjernen -lnordstjernen -o app
```

(Adjust the include path to the install prefix.)

Or against an uninstalled build tree:

```sh
cc app.c -I src \
   -L builddir/src -lnordstjernen \
   -Wl,-rpath,builddir/src -o app
```

## Notes

- The API is single-threaded and drives a private GLib main loop;
  call it from one thread and do not interleave instances across
  threads.
- `ns_browser_init` / `ns_browser_shutdown` bracket the whole session;
  open and close as many `ns_browser` handles between them as you like
  (one at a time).
- A larger `settle_ms` gives async scripts, fonts, and animations more
  time to converge before layout is captured; `0` skips the wait.
- The root-refusal guard belongs to the `nordstjernen` executable, not
  the library, and the library does not sandbox itself — the embedding
  host is responsible for its own privilege and sandbox posture. A host
  that runs the engine on untrusted content can opt in with
  `ns_browser_sandbox(argv[0])` (Linux Landlock + seccomp, the macOS
  Seatbelt write-confinement, Windows process mitigations), called once after `ns_browser_init` and
  before the first `ns_browser_open`.

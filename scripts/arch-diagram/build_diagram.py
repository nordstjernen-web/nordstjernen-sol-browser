#!/usr/bin/env python3
"""Draw docs/software-architecture-diagram.png: processes as columns, engine subsystems as layers."""

import os
import re
import subprocess
import sys
from datetime import date

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "docs", "software-architecture-diagram.png")
FONT_DIR = "/usr/share/fonts/truetype/dejavu/"

W = 4600
MARGIN = 110
INK = "#1F2933"
MUTED = "#52606D"
GRID = "#D9DEE3"
CHIP_EDGE = "#8A97A3"

PALETTE = {
    "shell":    ("#EEF3FA", "#2F5F9E"),
    "renderer": ("#F2F8F2", "#3F7A3F"),
    "media":    ("#FBF6E8", "#A4751A"),
    "profile":  ("#F4F0FA", "#6D4A94"),
    "security": ("#FBF1EF", "#A83A2E"),
    "vendored": ("#F4F4F4", "#5B6770"),
}
IPC = "#2F5F9E"
PIXELS = "#2E8B57"
SPAWN = "#B9382C"
PIPE = "#D08A00"


def font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    return ImageFont.truetype(FONT_DIR + name, size)


F_TITLE, F_SUB = font(66, True), font(30)
F_COL, F_COLSUB = font(38, True), font(24)
F_LAYER, F_LAYERSUB = font(30, True), font(21)
F_NAME, F_DESC = font(25, True), font(21)
F_LABEL, F_LABELB, F_SMALL = font(23), font(25, True), font(20)
F_NUM = font(22, True)


def kloc(path):
    try:
        with open(os.path.join(ROOT, path), "rb") as f:
            n = sum(1 for _ in f)
    except OSError:
        return ""
    return " (%.1f k lines)" % (n / 1000.0) if n >= 1000 else ""


def snapshot():
    version = "unknown"
    try:
        with open(os.path.join(ROOT, "meson.build"), encoding="utf-8") as f:
            m = re.search(r"version:\s*'([^']+)'", f.read())
            if m:
                version = m.group(1)
    except OSError:
        pass
    try:
        sha = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        sha = ""
    return "%s%s, %s" % (version, " (%s)" % sha if sha else "", date.today().isoformat())


def wrap(draw, s, fnt, width):
    lines = []
    for paragraph in s.split("\n"):
        line = ""
        for word in paragraph.split(" "):
            trial = (line + " " + word).strip()
            if draw.textlength(trial, font=fnt) <= width:
                line = trial
            else:
                if line:
                    lines.append(line)
                line = word
        lines.append(line)
    return lines


def text(draw, xy, s, fnt, fill=INK, anchor="la"):
    draw.text(xy, s, font=fnt, fill=fill, anchor=anchor)


def rounded(draw, box, fill, outline, width=3, radius=18):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


class Chip:
    PAD = 18
    GAP = 22

    def __init__(self, name, desc, step=None, accent=False):
        self.name, self.desc, self.step, self.accent = name, desc, step, accent

    def measure(self, draw, width):
        inner = width - 2 * self.PAD - (44 if self.step is not None else 0)
        names = wrap(draw, self.name, F_NAME, inner)
        lines = wrap(draw, self.desc, F_DESC, width - 2 * self.PAD)
        h = self.PAD + len(names) * 31 + 4 + len(lines) * 27 + self.PAD - 4
        return names, lines, h

    def paint(self, draw, x, y, width, h, outline):
        names, lines, _ = self.measure(draw, width)
        rounded(draw, (x, y, x + width, y + h), "#FFFFFF",
                outline if self.accent else CHIP_EDGE, width=2, radius=12)
        for i, line in enumerate(names):
            text(draw, (x + self.PAD, y + self.PAD - 2 + i * 31), line, F_NAME)
        base = y + self.PAD + len(names) * 31 + 2
        for i, line in enumerate(lines):
            text(draw, (x + self.PAD, base + i * 27), line, F_DESC, MUTED)
        if self.step is not None:
            cx, cy = x + width - 26, y + 26
            draw.ellipse((cx - 19, cy - 19, cx + 19, cy + 19), fill=PIPE)
            text(draw, (cx, cy + 1), str(self.step), F_NUM, "#FFFFFF", "mm")


def layer(draw, x, y, width, title, subtitle, chips, columns, key=None,
          head_w=480, paint=True):
    fill, outline = PALETTE[key] if key else ("#FFFFFF", CHIP_EDGE)
    gap = Chip.GAP
    inner_x = x + head_w
    chip_w = (width - head_w - gap * (columns + 1)) // columns
    rows, row = [], []
    for chip in chips:
        row.append((chip, chip.measure(draw, chip_w)[2]))
        if len(row) == columns:
            rows.append(row)
            row = []
    if row:
        rows.append(row)
    head_h = 64 + len(wrap(draw, subtitle, F_LAYERSUB, head_w - 52)) * 26 + 22
    body_h = gap + sum(max(h for _, h in r) + gap for r in rows)
    total_h = max(head_h, body_h)
    if paint:
        rounded(draw, (x, y, x + width, y + total_h), fill, outline, width=2, radius=14)
        text(draw, (x + 26, y + 22), title, F_LAYER)
        for i, line in enumerate(wrap(draw, subtitle, F_LAYERSUB, head_w - 52)):
            text(draw, (x + 26, y + 64 + i * 26), line, F_LAYERSUB, MUTED)
        draw.line((inner_x, y + 14, inner_x, y + total_h - 14), fill=GRID, width=2)
        cy = y + gap
        for r in rows:
            rh = max(h for _, h in r)
            cx = inner_x + gap
            for chip, _ in r:
                chip.paint(draw, cx, cy, chip_w, rh, outline)
                cx += chip_w + gap
            cy += rh + gap
    return total_h


def panel(draw, x, y, width, title, subtitle, chips, key, paint=True):
    fill, outline = PALETTE[key]
    sub_lines = wrap(draw, subtitle, F_COLSUB, width - 60)
    head_h = 74 + len(sub_lines) * 29 + 20
    inner_w = width - 60
    heights = [chip.measure(draw, inner_w)[2] for chip in chips]
    total_h = head_h + sum(h + 18 for h in heights) + 12
    if paint:
        rounded(draw, (x, y, x + width, y + total_h), fill, outline, width=4, radius=22)
        text(draw, (x + 30, y + 24), title, F_COL, outline)
        for i, line in enumerate(sub_lines):
            text(draw, (x + 30, y + 74 + i * 29), line, F_COLSUB, MUTED)
        cy = y + head_h
        for chip, h in zip(chips, heights):
            chip.paint(draw, x + 30, cy, inner_w, h, outline)
            cy += h + 18
    return total_h


def head_at(draw, a, b, color, size):
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = (dx * dx + dy * dy) ** 0.5 or 1.0
    ux, uy = dx / length, dy / length
    px, py = -uy, ux
    left = (b[0] - ux * size + px * size * 0.55, b[1] - uy * size + py * size * 0.55)
    right = (b[0] - ux * size - px * size * 0.55, b[1] - uy * size - py * size * 0.55)
    draw.polygon([b, left, right], fill=color)


def arrow(draw, a, b, color, width=6, dashed=False, head=28, both=False):
    if dashed:
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = max(2, int(((dx * dx + dy * dy) ** 0.5) / 34))
        for i in range(0, n, 2):
            p = (a[0] + dx * i / n, a[1] + dy * i / n)
            q = (a[0] + dx * min(i + 1, n) / n, a[1] + dy * min(i + 1, n) / n)
            draw.line((p, q), fill=color, width=width)
    else:
        draw.line((a, b), fill=color, width=width)
    head_at(draw, a, b, color, head)
    if both:
        head_at(draw, b, a, color, head)


def caption(draw, cx, y, title, lines, color):
    text(draw, (cx, y), title, F_LABELB, color, "ma")
    for i, line in enumerate(lines):
        text(draw, (cx, y + 34 + i * 27), line, F_LABEL, MUTED, "ma")


SHELL = [
    Chip("gtk/appmain.c", "process bootstrap and mode dispatch: supervisor, GUI, "
         "headless, single-process"),
    Chip("gtk/procwindow.c" + kloc("src/gtk/procwindow.c"),
         "tabbed window: tabs, address bar, menus, downloads, site settings"),
    Chip("gtk/procview.c" + kloc("src/gtk/procview.c"),
         "per-tab client: blits the shared framebuffer, forwards input, handles "
         "the X-* events, shows the capability prompt bar"),
    Chip("rproc_http.c + ipc_http.c", "engine-protocol client and HTTP/1.1 + JSON "
         "framing; spawns each renderer and checks its /hello handshake"),
    Chip("gtk/permissions.c", "capability ledger per origin (ask / allow / deny), "
         "persisted in permissions.ini; a change is pushed to every tab at once"),
    Chip("gtk/sessionstore.c", "tabs, history entries and scroll positions as rows "
         "of session.db; an instance claims rows, nothing lives only in memory"),
    Chip("platform library", "config.c, history.c, bookmarks.c, i18n.c, db.c, "
         "security.c (Landlock for the shell), capability.c"),
]
MEDIA = [
    Chip("nordstjernen-audio", "decodes MPEG-1/MP2 (pl_mpeg), MP3 (minimp3) and "
         "Opus/Vorbis (libav); mixes and plays through SDL2"),
    Chip("nordstjernen-video", "operating-system H.264/AAC decode; frames handed "
         "back through a shared-memory ring"),
]
PROFILE = [
    Chip("session.db", "tabs and their entries (sessionstore.c)"),
    Chip("permissions.ini", "the capability ledger"),
    Chip("SQLite stores and key files", "history, bookmarks, cookies, IndexedDB, "
         "HTTP cache; localStorage"),
]
LAYERS = [
    ("Protocol server", "answers the shell's requests on fd 3; drains the "
     "side-channel queues into X-* reply headers", 3, None, [
        Chip("renderer_http.c", "main(): request loop; seals the sandbox before "
             "the first page", accent=True),
        Chip("renderer_serve.c" + kloc("src/renderer_serve.c"),
             "/hello /open /render /tick /click /key /eval /permission "
             "/capability …", accent=True),
        Chip("engine_protocol.h + capability.h", "the versioned contract and the "
             "capability vocabulary, compiled into both sides", accent=True)]),
    ("Engine core", "libnordstjernen: the plain-C API that the renderer, the "
     "headless driver and embedders all use", 3, None, [
        Chip("libnordstjernen.c" + kloc("src/libnordstjernen.c"),
             "ns_browser_*: open, input, tick (timers, rAF, animations), "
             "render_argb32, pending-event queues"),
        Chip("engine.c + render.c", "fetch → cascade → layout → capture pipeline; "
             "style and box-tree rebuild, container-query pass"),
        Chip("headless.c", "--headless, --dump, --wpt, --eval: the engine with no "
             "shell and no IPC")]),
    ("Document pipeline", "bytes to DOM; the numbered badges trace a page load "
     "through the engine", 4, None, [
        Chip("net.c" + kloc("src/net.c"), "libcurl fetcher: HTTP/2, HSTS, "
             "mixed-content upgrade, private-network guard, partitioned cookies",
             step=1),
        Chip("html.c", "charset decode: BOM, declared charset, uchardet", step=2),
        Chip("html_lexbor.c + xml.c", "lexbor tokeniser and tree builder, XML and "
             "XHTML parser; lexbor tree → native DOM", step=3),
        Chip("dom.c" + kloc("src/dom.c"), "ns_node tree with id, class and tag "
             "indexes and an attribute Bloom filter", step=4)]),
    ("Style, layout and paint", "one main thread, software rendering through "
     "Cairo and the in-tree Pango fork (web-text-lib)", 4, None, [
        Chip("css.c" + kloc("src/css.c"), "parser, selectors, cascade: layers, "
             ":has(), @property, container queries, UA sheet", step=5),
        Chip("layout.c" + kloc("src/layout.c"), "block, inline, flex, grid, table, "
             "multicol; margin collapsing; anchor positioning", step=6),
        Chip("paint.c" + kloc("src/paint.c"), "stacking contexts, transforms, "
             "filters, clip paths, culling", step=7),
        Chip("css_media.c + anim.c", "media queries; transitions and @keyframes"),
        Chip("font.c + web-text-lib", "@font-face (WOFF, WOFF2 via FreeType), "
             "shaping cache, Fontconfig"),
        Chip("image.c + decoders", "Wuffs (PNG, APNG, GIF, BMP, JPEG), libwebp, "
             "AVIF, JPEG XL, ICO, in-engine SVG"),
        Chip("mathml.c, forms.c, selection.c", "presentation MathML; form "
             "validation and submission; text selection")]),
    ("JavaScript", "QuickJS-ng fork, bytecode interpreter only; the Web platform "
     "bindings are the largest module", 4, None, [
        Chip("js.c" + kloc("src/js.c"), "DOM, events, fetch and XHR with CORS, "
             "workers, storage, observers, realms, capability gates"),
        Chip("quickjs/ (fork)", "Temporal, ICU-free Intl, engine hooks, no JIT"),
        Chip("data/js/polyfills.js", "spec object models over thin native "
             "backends (Range, XMLSerializer, …)"),
        Chip("js_canvas.c, js_perf.c, bytecode_cache.c, ext.c", "Canvas 2D; "
             "performance.*; SHA-256-keyed bytecode cache; WebExtensions host")]),
    ("Web APIs", "each bound into JavaScript by js.c; media and graphics cross "
     "into helper processes or the GPU", 4, None, [
        Chip("webcrypto.c", "crypto.subtle over OpenSSL EVP"),
        Chip("idb.c", "IndexedDB on SQLite"),
        Chip("ws.c, eventsource.c", "WebSocket and Server-Sent Events over libcurl"),
        Chip("wasm.c + wamr/", "WebAssembly through the WAMR interpreter"),
        Chip("webgl.c + glctx.c", "WebGL 1 and 2 on OpenGL ES, offscreen EGL"),
        Chip("webgpu.c", "experimental navigator.gpu over wgpu-native"),
        Chip("camera.c, mic.c", "getUserMedia behind the camera capability gate"),
        Chip("video.c, video_decode.c", "inline MPEG-1 and WebM; MSE segments; OS "
             "decoders for H.264 and AAC")]),
    ("Network policy", "every request passes these gates inside the renderer",
     4, None, [
        Chip("cache.c", "SQLite-indexed HTTP cache with revalidation"),
        Chip("cookies.c", "partitioned store, cookie prefixes, SameSite"),
        Chip("csp.c", "Content-Security-Policy, including "
             "upgrade-insecure-requests"),
        Chip("safebrowsing.c", "local host blocklist; no network callout")]),
    ("Security", "confinement is sealed before the first page; what a page "
     "may do beyond rendering is a capability", 2, "security", [
        Chip("security.c" + kloc("src/security.c"), "Landlock ruleset and "
             "seccomp-bpf filter (no execve, ptrace, mount, bpf); Seatbelt; "
             "Windows mitigations", accent=True),
        Chip("capability.c", "the shared vocabulary: name, prompt, default, "
             "revoke action; a gate suspends the page until the shell answers",
             accent=True)]),
]
DEPENDENCIES = [
    ("In-tree libraries", "forked or vendored, built from the tree and "
     "modified freely; no system fallback", 6, "vendored", [
        Chip("lexbor (fork)", "HTML tokeniser and tree builder; WHATWG URL parser"),
        Chip("QuickJS-ng (fork)", "JavaScript engine: bytecode interpreter, no JIT"),
        Chip("WAMR", "WebAssembly Micro Runtime, classic interpreter"),
        Chip("web-text-lib (Pango fork)", "text shaping with a process-wide glyph "
             "cache; renamed symbols"),
        Chip("Wuffs", "memory-safe PNG, APNG, GIF, BMP and JPEG decoders"),
        Chip("pl_mpeg + minimp3", "MPEG-1 video, MP2 and MP3 audio (single files)")]),
    ("System libraries", "never vendored; taken from the platform's packages", 6,
     "vendored", [
        Chip("GTK 4 + GLib/GIO", "the shell's windows and widgets; event loops, "
             "threads, I/O in every process"),
        Chip("Cairo", "software rasteriser for paint.c and the framebuffer"),
        Chip("FreeType + Fontconfig", "font files, @font-face loading, font matching"),
        Chip("libcurl", "HTTP/1.1, HTTP/2, WebSocket transport, HSTS, Alt-Svc"),
        Chip("OpenSSL", "TLS through libcurl; crypto.subtle (webcrypto.c)"),
        Chip("SQLite", "history, bookmarks, cookies, IndexedDB, HTTP cache index, "
             "session.db"),
        Chip("libpsl, zlib, brotli, zstd", "public-suffix list; content decoding"),
        Chip("uchardet", "charset detection for html.c"),
        Chip("libwebp, libavif, libjxl", "WebP incl. animation; AVIF and JPEG XL "
             "(optional)"),
        Chip("FFmpeg libav* (LGPL)", "WebM demux and VP8/VP9 decode; Opus and "
             "Vorbis in the audio helper"),
        Chip("GStreamer, Media Foundation, VideoToolbox", "operating-system H.264 "
             "and AAC decoders"),
        Chip("SDL2", "audio output and microphone capture in the helpers"),
        Chip("libepoxy + EGL", "OpenGL ES context for WebGL"),
        Chip("wgpu-native", "WebGPU backend (fetched at build time, never "
             "committed)"),
        Chip("libseccomp", "the renderer's syscall filter on Linux")]),
]


SUPERVISOR = [
    Chip("watchdog.c (supervisor)", "the process you launch: spawns the shell, "
         "restarts it on a crash or hang, crash-loop cap of 5 restarts per minute"),
    Chip("watchdog.c (heartbeat thread)", "runs inside the shell; turns a wedged "
         "main loop into an exit the supervisor recovers"),
    Chip("threaddump.c", "SIGQUIT thread dump, linked into the shell and the "
         "renderer"),
]


def vertical_channel(draw, x, y0, y1, color, direction, title, lines, dashed=False):
    if direction == "down":
        arrow(draw, (x, y0), (x, y1), color, dashed=dashed)
    elif direction == "up":
        arrow(draw, (x, y1), (x, y0), color, dashed=dashed)
    else:
        arrow(draw, (x, y0), (x, y1), color, both=True)
    ty = (y0 + y1) // 2 - (34 + len(lines) * 27) // 2
    text(draw, (x + 28, ty), title, F_LABELB, color)
    for i, line in enumerate(lines):
        text(draw, (x + 28, ty + 36 + i * 27), line, F_LABEL, MUTED)


def render(draw, paint):
    top = 250
    full = W - 2 * MARGIN
    gap = 36
    if paint:
        text(draw, (MARGIN, 70), "Nordstjernen — software architecture", F_TITLE)
        text(draw, (MARGIN, 150), "Process model, engine subsystems and the "
             "boundaries between them.   Snapshot %s." % snapshot(), F_SUB, MUTED)
        draw.line((MARGIN, 205, W - MARGIN, 205), fill=INK, width=3)

    y = top
    h = layer(draw, MARGIN, y, full, "Supervisor", "nordstjernen (watchdog): owns "
              "no UI; the only process that can spawn the shell", SUPERVISOR, 3,
              key="shell", paint=paint)
    if paint:
        arrow(draw, (MARGIN + 240, y + h), (MARGIN + 240, y + h + gap), SPAWN,
              dashed=True, head=22)
    y += h + gap
    shell_y = y
    h = layer(draw, MARGIN, y, full, "Browser shell", "nordstjernen (GTK 4): "
              "windows, tabs, address bar, prompts and settings. Engine-free: it "
              "never parses HTML, runs CSS or executes JavaScript.", SHELL, 4,
              key="shell", paint=paint)
    y += h + gap

    side_w = 1180
    profile_h = panel(draw, MARGIN, y, side_w, "Profile: persistent data", "the "
                      "source of truth; a window is a view of it and a renderer a "
                      "worker on it", PROFILE, "profile", paint=False)
    media_h = panel(draw, W - MARGIN - side_w, y, side_w, "Media helpers",
                    "spawned lazily per tab by the shell, outside the renderer "
                    "sandbox; driven by a line protocol on stdin and stdout",
                    MEDIA, "media", paint=False)
    band_h = max(profile_h, media_h, 420)
    if paint:
        panel(draw, MARGIN, y, side_w, "Profile: persistent data", "the source of "
              "truth; a window is a view of it and a renderer a worker on it",
              PROFILE, "profile", paint=True)
        panel(draw, W - MARGIN - side_w, y, side_w, "Media helpers", "spawned "
              "lazily per tab by the shell, outside the renderer sandbox; driven "
              "by a line protocol on stdin and stdout", MEDIA, "media", paint=True)
        mid_x0 = MARGIN + side_w + 60
        mid_w = W - MARGIN - side_w - 60 - mid_x0
        col = mid_w // 3
        y0, y1 = y - gap + 8, y + band_h + gap - 8
        vertical_channel(draw, mid_x0 + 40, y0, y1, IPC, "both", "Engine protocol",
                         ["HTTP/1.1 + JSON on an AF_UNIX",
                          "socketpair (fd 3); versioned,",
                          "/hello comes first. Requests go",
                          "down; replies carry X-Nav,",
                          "X-Capability, X-Download,",
                          "X-Audio and X-Config events."])
        vertical_channel(draw, mid_x0 + col + 40, y0, y1, PIXELS, "up",
                         "Shared-memory framebuffer",
                         ["memfd or shm_open segment,",
                          "Cairo ARGB32; the descriptor is",
                          "passed with SCM_RIGHTS. The",
                          "renderer paints, the shell blits."])
        vertical_channel(draw, mid_x0 + 2 * col + 40, y0, y1, SPAWN, "down",
                         "Spawn and supervise",
                         ["one renderer per tab, restarted",
                          "on a crash up to a limit; ledger",
                          "decisions pushed down through",
                          "/permission and /capability."], dashed=True)
        mx = W - MARGIN - side_w // 2
        arrow(draw, (mx, y - gap + 8), (mx, y - 6), PALETTE["media"][1],
              dashed=True, head=22)
        px = MARGIN + side_w // 2
        arrow(draw, (px, y - gap + 8), (px, y - 6), PALETTE["profile"][1], head=22,
              both=True)
    y += band_h + gap

    ren_y = y
    ly = y + 150
    ren_layers_h = 0
    for title, subtitle, columns, key, chips in LAYERS:
        ren_layers_h += layer(draw, MARGIN + 30, 0, full - 60, title, subtitle,
                              chips, columns, key=key, paint=False) + 24
    ren_h = 150 + ren_layers_h + 6
    if paint:
        rounded(draw, (MARGIN, ren_y, W - MARGIN, ren_y + ren_h),
                PALETTE["renderer"][0], PALETTE["renderer"][1], width=4, radius=22)
        text(draw, (MARGIN + 30, ren_y + 24), "Renderer: nordstjernen-renderer, "
             "one process per tab", F_COL, PALETTE["renderer"][1])
        for i, line in enumerate(wrap(draw, "the whole engine, sandboxed: Landlock "
                                      "and seccomp-bpf on Linux, Seatbelt on macOS, "
                                      "mitigation policies on Windows; no JIT "
                                      "anywhere; treated as untrusted by the shell.",
                                      F_COLSUB, full - 60)):
            text(draw, (MARGIN + 30, ren_y + 74 + i * 29), line, F_COLSUB, MUTED)
        for title, subtitle, columns, key, chips in LAYERS:
            ly += layer(draw, MARGIN + 30, ly, full - 60, title, subtitle, chips,
                        columns, key=key, paint=True) + 24
    y = ren_y + ren_h + gap + 14

    for title, subtitle, columns, key, chips in DEPENDENCIES:
        y += layer(draw, MARGIN, y, full, title, subtitle, chips, columns, key=key,
                   paint=paint) + 24
    return y


def legend(draw, x, y):
    text(draw, (x, y), "Legend", F_LAYER)
    yy = y + 48
    for color, label, dashed in [(IPC, "control channel (engine protocol)", False),
                                 (PIXELS, "pixel path (shared memory)", False),
                                 (SPAWN, "process spawn and supervision", True)]:
        arrow(draw, (x + 6, yy + 14), (x + 60, yy + 14), color, width=5,
              dashed=dashed, head=18)
        text(draw, (x + 76, yy + 2), label, F_LABEL, MUTED)
        yy += 36
    draw.ellipse((x + 16, yy - 2, x + 48, yy + 30), fill=PIPE)
    text(draw, (x + 32, yy + 15), "n", F_NUM, "#FFFFFF", "mm")
    text(draw, (x + 76, yy + 2), "page-load pipeline, steps 1 to 7", F_LABEL, MUTED)
    return yy + 40


def main():
    probe = ImageDraw.Draw(Image.new("RGB", (W, 100)))
    bottom = render(probe, paint=False)
    legend_h = 48 + 4 * 36 + 20
    img = Image.new("RGB", (W, bottom + 30 + legend_h + MARGIN), "#FFFFFF")
    draw = ImageDraw.Draw(img)
    render(draw, paint=True)
    legend(draw, MARGIN, bottom + 20)
    img.save(OUT, optimize=True)
    print("wrote", OUT, img.size)


if __name__ == "__main__":
    sys.exit(main())

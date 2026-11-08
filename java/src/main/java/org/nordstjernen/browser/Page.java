/* Nordstjernen — one web page in a renderer process of its own.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import org.nordstjernen.Downloads;
import org.nordstjernen.NordstjernenException;
import org.nordstjernen.RendererTimeoutException;
import org.nordstjernen.internal.Json;
import org.nordstjernen.internal.RendererProcess;

import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A web page. It is fetched, scripted, laid out and painted by a
 * {@code nordstjernen-renderer} process that belongs to this page alone and
 * starts with the first {@link #navigate}.
 *
 * <p>A page may be used from any thread; its requests are carried out one at
 * a time. Every request has a deadline, {@link #timeout()}: a renderer that
 * does not answer in time is stopped and the call fails with
 * {@link RendererTimeoutException}. After that, and after a renderer crash,
 * the page has no document until it is navigated again, which starts a fresh
 * renderer.
 *
 * <p>Coordinates are CSS pixels in the document unless a method says
 * otherwise, so a point stays the same whatever the scroll position.
 */
public final class Page implements AutoCloseable {

    /** How a navigation is carried out. */
    public record Navigation(Duration settle, boolean fromHistory, boolean userActivated) {

        public Navigation {
            if (settle == null || settle.isNegative()) {
                throw new IllegalArgumentException("settle must not be negative");
            }
        }

        /** Let scripts run for 600 ms, as a navigation the user asked for. */
        public static Navigation defaults() {
            return new Navigation(Duration.ofMillis(600), false, true);
        }

        /** How long scripts, timers and late requests may run before the page is taken as loaded. */
        public Navigation withSettle(Duration settle) {
            return new Navigation(settle, fromHistory, userActivated);
        }

        /**
         * A back or forward step: the renderer may restore the document from
         * its back-forward cache instead of fetching and running it again.
         */
        public Navigation withFromHistory(boolean fromHistory) {
            return new Navigation(settle, fromHistory, userActivated);
        }

        /** Whether a person asked for this navigation; the engine gates popups and the like on it. */
        public Navigation withUserActivated(boolean userActivated) {
            return new Navigation(settle, fromHistory, userActivated);
        }
    }

    /** What to capture in a {@link #screenshot(Screenshot)}. */
    public record Screenshot(int x, int y, int width, int height, double scale) {

        public Screenshot {
            if (x < 0 || y < 0 || width < 0 || height < 0 || !(scale > 0)) {
                throw new IllegalArgumentException("a screenshot region and scale must be positive");
            }
        }

        /** The whole document, at one device pixel per CSS pixel. */
        public static Screenshot fullPage() {
            return new Screenshot(0, 0, 0, 0, 1.0);
        }

        /** {@code width} by {@code height} CSS pixels from the document point {@code (x, y)}. */
        public static Screenshot region(int x, int y, int width, int height) {
            return new Screenshot(x, y, width, height, 1.0);
        }

        /** Device pixels per CSS pixel: 2 for a high-density image. */
        public Screenshot withScale(double scale) {
            return new Screenshot(x, y, width, height, scale);
        }
    }

    /** The diagnostic views of a page {@link #dump} can produce. */
    public enum Dump {
        DOM("dom"), LAYOUT("layout"), TEXT("text"), LINKS("links"),
        NETWORK("network"), PERFORMANCE("performance");

        private final String wire;

        Dump(String wire) {
            this.wire = wire;
        }
    }

    /** The kinds of key event {@link #key} delivers. */
    public enum KeyKind {
        DOWN(0), UP(1),
        /** Insert the key string as text, as an input method or a dead key would. */
        TEXT(2),
        /** A keypress, skipped by the engine when the preceding keydown was cancelled. */
        PRESS(3),
        /** Paste the key string as clipboard text. */
        PASTE(4);

        private final int wire;

        KeyKind(int wire) {
            this.wire = wire;
        }
    }

    /** What {@link #select} does with the text selection. */
    public enum Selection {
        /** Start a new selection at the point. */
        ANCHOR(0),
        /** Extend the selection to the point. */
        EXTEND(1),
        CLEAR(2),
        /** Select the whole document, or the focused field's text. */
        ALL(3),
        /** Return the selected text. */
        COPY(4),
        /** Select the word at the point. */
        WORD(5),
        /** Select the block at the point. */
        BLOCK(6),
        /** Remove and return the focused field's selection. */
        CUT(7);

        private final int wire;

        Selection(int wire) {
            this.wire = wire;
        }
    }

    /** Where {@link #find} looks next. */
    public enum FindDirection {
        /** Search afresh, starting at a document position. */
        FROM(0), NEXT(1), PREVIOUS(2);

        private final int wire;

        FindDirection(int wire) {
            this.wire = wire;
        }
    }

    /** The result of moving the pointer: whether anything changed, the link and the CSS cursor there. */
    public record Hover(boolean changed, String link, String cursor) {
    }

    /** The result of a search: the match count, the current match (from 1) and where to scroll to see it. */
    public record Found(int total, int current, int scrollY) {
    }

    /** A media element at a point. */
    public record Media(String url, boolean video, boolean stream) {
    }

    /** What a right-click met: a page that drew its own menu, a text field. */
    public record ContextMenu(boolean handledByPage, boolean editable) {
    }

    /** The focused text field's contents and selection, in UTF-8 byte offsets. */
    public record Editable(boolean active, String value, int caret, int anchor) {
    }

    /** The result of a key event: a navigation it triggered, and whether the page consumed it. */
    public record KeyResult(String navigation, boolean prevented) {
    }

    /** Premultiplied BGRA pixels, as in {@link Frame}. */
    public record Image(byte[] pixels, int width, int height, int stride) {
    }

    /** The progress of a download the renderer is fetching. */
    public record Download(State state, long bytes, long total, Path stagedFile,
                           String finalUrl, String contentType, String error) {

        /** The states a download goes through. */
        public enum State { RUNNING, DONE, FAILED }
    }

    private final BrowserContext context;
    private RendererProcess renderer;
    private boolean closed;
    private volatile Duration timeout;
    private volatile int viewportWidth;
    private volatile int viewportHeight;
    private volatile double devicePixelRatio = 1.0;
    private volatile String title = "";
    private volatile String url = "";
    private volatile String serverAddress;
    private volatile Security security = Security.NONE;
    private volatile int pageWidth;
    private volatile int pageHeight;
    private volatile String engineVersion = "";
    private volatile boolean popupsSupported;
    private volatile String openerToken = "";
    private volatile String openerOrigin = "";
    private volatile ResourcePolicy resourcePolicy;
    private volatile ContextOptions.ColorScheme colorScheme;
    private volatile String content;

    Page(BrowserContext context, int viewportWidth, int viewportHeight) {
        if (viewportWidth < 1 || viewportHeight < 1) {
            throw new IllegalArgumentException("the viewport must be at least one pixel a side");
        }
        this.context = context;
        this.viewportWidth = viewportWidth;
        this.viewportHeight = viewportHeight;
        this.timeout = context.engine().options().timeout();
        this.resourcePolicy = context.options().resourcePolicy();
        this.colorScheme = context.options().colorScheme();
    }

    public BrowserContext context() {
        return context;
    }

    /** How long a request to this page may take. */
    public Duration timeout() {
        return timeout;
    }

    public synchronized void setTimeout(Duration timeout) {
        if (timeout == null || timeout.isNegative() || timeout.isZero()) {
            throw new IllegalArgumentException("timeout must be positive");
        }
        this.timeout = timeout;
        if (renderer != null) {
            renderer.setTimeoutMs(timeout.toMillis());
        }
    }

    /** What this page may load; the context's policy unless set. */
    public ResourcePolicy resourcePolicy() {
        return resourcePolicy;
    }

    /**
     * Change what this page may load. The policy holds for every request
     * from now on, including those of the document already showing.
     */
    public synchronized void setResourcePolicy(ResourcePolicy policy) {
        if (policy == null) {
            throw new IllegalArgumentException("policy must not be null");
        }
        this.resourcePolicy = policy;
        if (renderer != null && renderer.isAlive()) {
            renderer.request("/policy", policy.toJson(), timeout.toMillis());
        }
    }

    public ContextOptions.ColorScheme colorScheme() {
        return colorScheme;
    }

    /** What {@code prefers-color-scheme} answers; the document restyles at once. */
    public synchronized void setColorScheme(ContextOptions.ColorScheme scheme) {
        if (scheme == null) {
            throw new IllegalArgumentException("scheme must not be null");
        }
        this.colorScheme = scheme;
        if (renderer != null && renderer.isAlive()) {
            renderer.request("/colorscheme", "{\"dark\":"
                + (scheme == ContextOptions.ColorScheme.DARK ? 1 : 0) + "}", timeout.toMillis());
        }
    }

    /** Load {@code url} as a navigation the user asked for; see {@link #navigate(String, Navigation)}. */
    public Response navigate(String url) {
        return navigate(url, Navigation.defaults());
    }

    /**
     * Load {@code url}: an absolute URL, an {@code about:} page, a
     * {@code data:} URL or a local path. The call returns once the document
     * is fetched, parsed, scripted for {@link Navigation#settle()} and laid
     * out. If the page has no running renderer, one is started.
     *
     * @return what the navigation ended in; {@link Response#ok()} is false
     *         when the engine could not open a document
     */
    public Response navigate(String url, Navigation how) {
        if (url == null || url.isEmpty()) {
            throw new IllegalArgumentException("url must not be empty");
        }
        this.content = null;
        return open(url, "", how);
    }

    private Response open(String url, String content, Navigation how) {
        RendererProcess process = renderer(true);
        String body = "{\"url\":" + Json.quote(url)
            + ",\"width\":" + viewportWidth + ",\"height\":" + viewportHeight
            + ",\"settle_ms\":" + how.settle().toMillis()
            + ",\"history\":" + (how.fromHistory() ? 1 : 0)
            + ",\"user_activated\":" + (how.userActivated() ? 1 : 0)
            + ",\"popups\":" + (popupsSupported ? 1 : 0)
            + ",\"opener_token\":" + Json.quote(openerToken)
            + ",\"opener_origin\":" + Json.quote(openerOrigin)
            + ",\"dpr\":" + decimal(devicePixelRatio) + content + "}";
        String json = process.request("/open", body, timeout.toMillis()).text();
        if (!Json.flag(json, "ok")) {
            return new Response(false, url, "", Security.NONE, null, null);
        }
        this.title = Json.string(json, "title");
        this.url = Json.string(json, "url");
        this.serverAddress = Json.stringOrNull(json, "ip");
        this.security = Security.of(Json.integer(json, "security", 0));
        this.pageWidth = Json.integer(json, "page_width", viewportWidth);
        this.pageHeight = Json.integer(json, "page_height", viewportHeight);
        return new Response(true, this.url, this.title, this.security, this.serverAddress,
                            Json.stringOrNull(json, "nav"));
    }

    /** Show {@code html} as the document, with no base URL; see {@link #setContent(String, String, Navigation)}. */
    public Response setContent(String html) {
        return setContent(html, null, Navigation.defaults());
    }

    /** Show {@code html} as the document at {@code baseUrl}; see {@link #setContent(String, String, Navigation)}. */
    public Response setContent(String html, String baseUrl) {
        return setContent(html, baseUrl, Navigation.defaults());
    }

    /**
     * Show {@code html} as the document, without fetching it from anywhere.
     * {@code baseUrl} becomes the document's URL: relative links, style
     * sheets, scripts and images resolve against it, and it decides the
     * document's origin, so the page reads the cookies and storage this
     * context holds for that origin. With a {@code file:} base URL the
     * document may load files from that directory and below. With null the
     * document is {@code about:blank}, has an origin of its own, and only
     * absolute URLs in it load.
     *
     * <p>What the document may load is the page's {@link ResourcePolicy}. The
     * call returns once the document is parsed, scripted for
     * {@link Navigation#settle()} and laid out, with its subresources loaded.
     */
    public Response setContent(String html, String baseUrl, Navigation how) {
        if (html == null) {
            throw new IllegalArgumentException("html must not be null");
        }
        String base = baseUrl == null || baseUrl.isEmpty() ? "about:blank" : baseUrl;
        this.content = html;
        return open(base, ",\"html\":" + Json.quote(html.replace('\0', '�')), how);
    }

    /** Load the current document again; one given as HTML is shown again as it was given. */
    public Response reload() {
        String current = url;
        if (current.isEmpty()) {
            throw new IllegalStateException("there is no document to reload");
        }
        String html = content;
        if (html != null) {
            return setContent(html, current);
        }
        return navigate(current);
    }

    /** The document's title, empty before the first navigation. */
    public String title() {
        return title;
    }

    /** The document's URL after redirects, empty before the first navigation. */
    public String url() {
        return url;
    }

    public Security security() {
        return security;
    }

    /** The address the document was fetched from, or null. */
    public String serverAddress() {
        return serverAddress;
    }

    /** The laid-out page width in CSS pixels. */
    public int pageWidth() {
        return pageWidth;
    }

    /** The laid-out page height in CSS pixels. */
    public int pageHeight() {
        return pageHeight;
    }

    public int viewportWidth() {
        return viewportWidth;
    }

    public int viewportHeight() {
        return viewportHeight;
    }

    /** The version of the engine in this page's renderer, empty until one has started. */
    public String engineVersion() {
        return engineVersion;
    }

    /**
     * Resize the viewport, in CSS pixels. A loaded document is laid out again
     * and sees a {@code resize} event.
     */
    public void setViewport(int width, int height) {
        if (width < 1 || height < 1) {
            throw new IllegalArgumentException("the viewport must be at least one pixel a side");
        }
        this.viewportWidth = width;
        this.viewportHeight = height;
        RendererProcess process = runningOrNull();
        if (process == null) {
            return;
        }
        String json = process.request("/viewport", "{\"width\":" + width + ",\"height\":" + height
            + ",\"dpr\":" + decimal(devicePixelRatio) + "}", timeout.toMillis()).text();
        if (Json.flag(json, "ok")) {
            this.pageWidth = Json.integer(json, "page_width", pageWidth);
            this.pageHeight = Json.integer(json, "page_height", pageHeight);
        }
    }

    /**
     * The page's {@code window.devicePixelRatio}, which picks its
     * {@code srcset} candidates and resolution media queries. It travels with
     * the next navigation, viewport change and render.
     */
    public void setDevicePixelRatio(double ratio) {
        if (ratio > 0 && !Double.isInfinite(ratio)) {
            this.devicePixelRatio = ratio;
        }
    }

    public double devicePixelRatio() {
        return devicePixelRatio;
    }

    /** The page's text as laid out, the way a reader would see it. */
    public String text() {
        String text = dump(Dump.TEXT);
        return text == null ? "" : text;
    }

    /**
     * The targets of the page's links as absolute URLs, without duplicates
     * and in document order. {@code javascript:} links and links to a
     * fragment of the page itself are left out.
     */
    public List<String> links() {
        String joined = dump(Dump.LINKS);
        return joined == null || joined.isEmpty() ? List.of() : List.of(joined.split("\n"));
    }

    /** A diagnostic view of the page, or null when there is nothing to report. */
    public String dump(Dump kind) {
        return Json.stringOrNull(
            call("/dump", "{\"kind\":" + Json.quote(kind.wire) + "}").text(), "text");
    }

    /**
     * Run {@code script} in the page and return its completion value
     * converted to a string, or null when it has none.
     */
    public String evaluate(String script) {
        return Json.stringOrNull(
            call("/eval", "{\"src\":" + Json.quote(script == null ? "" : script) + "}").text(),
            "text");
    }

    /**
     * Send {@code request}, a JSON {@code {"fn": …, "args": […]}} object, to
     * the engine's remote-control helper, which the renderer runs in a
     * script realm of its own, and return its reply, or null when the page
     * has no scripting. A reply starting {@code "error: "} is the helper's
     * error.
     */
    public String remoteCall(String request) {
        return Json.stringOrNull(
            call("/remote-call", "{\"request\":" + Json.quote(request) + "}").text(),
            "text");
    }

    /** What the page wrote to its console since the last call, or null. */
    public String consoleOutput() {
        return Json.stringOrNull(call("/console", "{}").text(), "text");
    }

    /** The whole page as a PNG image, one device pixel per CSS pixel. */
    public byte[] screenshot() {
        return screenshot(Screenshot.fullPage());
    }

    /**
     * A PNG image of the page, encoded by the renderer. The document is
     * painted in one pass, so an element with {@code position: fixed} appears
     * once. Each side is capped at 30,000 device pixels.
     */
    public byte[] screenshot(Screenshot shot) {
        String body = "{\"x\":" + shot.x() + ",\"y\":" + shot.y() + ",\"width\":" + shot.width()
            + ",\"height\":" + shot.height() + ",\"scale\":" + decimal(shot.scale()) + "}";
        RendererProcess.Response resp = call("/screenshot", body);
        if (resp.body.length == 0) {
            throw new NordstjernenException("the renderer could not take the screenshot");
        }
        return resp.body;
    }

    /** The page printed to A4 sheets with half-inch margins; see {@link #pdf(PdfOptions)}. */
    public byte[] pdf() {
        return pdf(PdfOptions.a4());
    }

    /**
     * The page as a paged PDF document. It is laid out again for the sheet,
     * with its {@code @media print} styles, and cut into pages between lines,
     * table rows and boxes marked {@code break-inside: avoid}; forced breaks
     * are honoured and a heading stays with what follows it. Text is real,
     * selectable text with embedded fonts, links stay clickable, headings
     * become the document outline, and the page sees {@code beforeprint} and
     * {@code afterprint}. The screen layout is restored afterwards.
     *
     * @throws NordstjernenException when the renderer cannot produce the
     *                               document, or predates the request
     */
    public byte[] pdf(PdfOptions options) {
        RendererProcess.Response resp = call("/pdf", options.toJson());
        if (resp.body.length == 0) {
            throw new NordstjernenException("the renderer could not print the page");
        }
        return resp.body;
    }

    /**
     * Write the whole page to {@code file}: a PDF when the name ends in
     * {@code .pdf}, otherwise a PNG. The renderer writes the file itself, so
     * on Linux the path must lie where its sandbox lets it write.
     */
    public void export(Path file) {
        String json = call("/export",
            "{\"path\":" + Json.quote(file.toAbsolutePath().toString()) + "}").text();
        if (Json.integer(json, "ok", -1) != 0) {
            throw new NordstjernenException("could not write " + file);
        }
    }

    /**
     * Paint a view of the page: {@code width} by {@code height} device pixels
     * showing the document from {@code (scrollX, scrollY)} at {@code scale}
     * device pixels per CSS pixel. The frame is no larger than
     * {@link Engine.Options#frameWidth()} by {@link Engine.Options#frameHeight()}.
     *
     * @param caret a text field of the page holds the keyboard focus, so its caret should blink
     */
    public Frame render(int scrollX, int scrollY, int width, int height, double scale,
                        boolean caret) {
        if (width <= 0 || height <= 0) {
            throw new IllegalArgumentException("width and height must be positive");
        }
        String body = "{\"width\":" + width + ",\"height\":" + height
            + ",\"scroll_x\":" + scrollX + ",\"scroll_y\":" + scrollY
            + ",\"scale\":" + decimal(scale) + ",\"caret\":" + (caret ? 1 : 0)
            + ",\"dpr\":" + decimal(devicePixelRatio) + "}";
        RendererProcess.Response resp = call("/render", body);
        int anim = header(resp, "X-Anim", 0);
        boolean unchanged = "1".equals(resp.header("X-Unchanged")) || resp.body.length < 4;
        this.pageWidth = header(resp, "X-PageW", pageWidth);
        this.pageHeight = header(resp, "X-PageH", pageHeight);
        int w = header(resp, "X-W", width);
        int h = header(resp, "X-H", height);
        String download = resp.header("X-Download");
        PageEvents events = new PageEvents(
            emptyToNull(resp.header("X-Nav")), downloadUrl(download), downloadName(download),
            emptyToNull(resp.header("X-Window-Action")), audioCommands(resp.header("X-Audio")),
            header(resp, "X-Clipboard", 0) != 0, header(resp, "X-Popup", 0) != 0,
            capabilities(resp.header("X-Capability"), resp.header("X-Camera")),
            emptyToNull(resp.header("X-WebGL")));
        return new Frame(unchanged ? null : resp.body, w, h, header(resp, "X-Stride", w * 4),
                         unchanged, (anim & 1) != 0, (anim & 2) != 0, pageWidth, pageHeight,
                         header(resp, "X-ScrollX", -1), header(resp, "X-ScrollY", -1), events);
    }

    /**
     * Let the page's timers, transitions and network work progress without
     * painting. A host that is not rendering calls this to keep the page
     * alive, and to learn its title and URL when scripts change them.
     */
    public Tick tick() {
        String json = call("/tick", "{}").text();
        this.pageWidth = Json.integer(json, "page_width", pageWidth);
        this.pageHeight = Json.integer(json, "page_height", pageHeight);
        String newTitle = Json.string(json, "title");
        String newUrl = Json.string(json, "url");
        if (!newUrl.isEmpty()) {
            this.url = newUrl;
            this.title = newTitle;
        }
        String download = Json.stringOrNull(json, "download");
        PageEvents events = new PageEvents(
            Json.stringOrNull(json, "nav"), downloadUrl(download), downloadName(download),
            Json.stringOrNull(json, "window_action"),
            audioCommands(Json.stringOrNull(json, "audio")), false, Json.flag(json, "popup"),
            capabilities(Json.stringOrNull(json, "capability"), Json.stringOrNull(json, "camera")),
            Json.stringOrNull(json, "webgl"));
        return new Tick(Json.flag(json, "changed"), Json.flag(json, "animating"),
                        Json.integer(json, "wake_ms", 0), pageWidth, pageHeight, title, url,
                        events);
    }

    /** The page's icon, or null when it has none. */
    public Image favicon() {
        RendererProcess.Response resp = call("/favicon", "{}");
        int w = header(resp, "X-W", 0);
        int h = header(resp, "X-H", 0);
        int stride = header(resp, "X-Stride", w * 4);
        if (w <= 0 || h <= 0 || stride < w * 4 || resp.body.length < (long) stride * h) {
            return null;
        }
        return new Image(resp.body, w, h, stride);
    }

    /**
     * Press the primary button at a point. Follow with {@link #mouseUp()}.
     * Here and in the other pointer methods a point is in CSS pixels from the
     * top left of the document, not of the viewport: add the scroll offset.
     *
     * @param modifiers shift 1, control 2, alt 4, meta 8
     * @return the link under the point, or null
     */
    public String mouseDown(int x, int y, int modifiers) {
        return Json.stringOrNull(call("/click",
            "{\"x\":" + x + ",\"y\":" + y + ",\"mods\":" + modifiers + "}").text(), "href");
    }

    /** Release a pending press. Returns a URL to navigate to when the click followed a link, else null. */
    public String mouseUp() {
        return Json.stringOrNull(call("/release", "{}").text(), "href");
    }

    /**
     * Click at a point: the page sees pointer, mouse and click events, a
     * field under the point takes the focus, a form may submit.
     *
     * @return a URL to navigate to when the click followed a link, else null
     */
    public String click(int x, int y) {
        mouseDown(x, y, 0);
        return mouseUp();
    }

    /** Move the pointer to a point, driving {@code :hover} and the mouse-move events. */
    public Hover hover(int x, int y) {
        String json = call("/hover", "{\"x\":" + x + ",\"y\":" + y + "}").text();
        return new Hover(Json.flag(json, "changed"), Json.stringOrNull(json, "href"),
                         Json.stringOrNull(json, "cursor"));
    }

    /** The link at a point, or null. */
    public String linkAt(int x, int y) {
        return Json.stringOrNull(call("/link", "{\"x\":" + x + ",\"y\":" + y + "}").text(),
                                 "href");
    }

    /** The audio or video element at a point, or null. */
    public Media mediaAt(int x, int y) {
        String json = call("/media", "{\"x\":" + x + ",\"y\":" + y + "}").text();
        String media = Json.string(json, "url");
        return media.isEmpty() ? null
            : new Media(media, Json.flag(json, "is_video"), Json.flag(json, "stream"));
    }

    /**
     * Scroll whatever lies under a point by {@code (dx, dy)} CSS pixels.
     * True when a scrollable element took it, in which case the host leaves
     * its own page scroll alone.
     */
    public boolean scrollAt(int x, int y, int dx, int dy) {
        return Json.flag(call("/scroll", "{\"x\":" + x + ",\"y\":" + y + ",\"dx\":" + dx
            + ",\"dy\":" + dy + "}").text(), "consumed");
    }

    /** Start dragging a scrollbar the page painted. True when the point hit one. */
    public boolean scrollbarPress(int x, int y) {
        return Json.flag(call("/scrollbar-press", "{\"x\":" + x + ",\"y\":" + y + "}").text(),
                         "hit");
    }

    public boolean scrollbarDrag(int x, int y) {
        return Json.flag(call("/scrollbar-drag", "{\"x\":" + x + ",\"y\":" + y + "}").text(),
                         "hit");
    }

    public void scrollbarRelease() {
        call("/scrollbar-release", "{}");
    }

    /** Deliver a {@code contextmenu} event at a point. */
    public ContextMenu contextMenu(int x, int y) {
        String json = call("/contextmenu", "{\"x\":" + x + ",\"y\":" + y + "}").text();
        return new ContextMenu(Json.flag(json, "prevented"), Json.flag(json, "edit"));
    }

    /** Drop files on the page at a point, as a drag from a file manager would. True when the page took them. */
    public boolean dropFiles(int x, int y, List<Path> files) {
        if (files == null || files.isEmpty()) {
            return false;
        }
        StringBuilder paths = new StringBuilder();
        for (Path file : files) {
            if (paths.length() > 0) {
                paths.append('\n');
            }
            paths.append(file.toAbsolutePath());
        }
        return Json.flag(call("/dropfiles", "{\"x\":" + x + ",\"y\":" + y + ",\"paths\":"
            + Json.quote(paths.toString()) + "}").text(), "changed");
    }

    /**
     * Deliver a key event to the focused element.
     *
     * @param key       the {@code KeyboardEvent.key} value, or the text for
     *                  {@link KeyKind#TEXT} and {@link KeyKind#PASTE}
     * @param code      the {@code KeyboardEvent.code} value
     * @param keyCode   the legacy numeric key code
     * @param modifiers shift 1, control 2, alt 4, meta 8
     */
    public KeyResult key(KeyKind kind, String key, String code, int keyCode, int modifiers) {
        String json = call("/key", "{\"kind\":" + kind.wire + ",\"key\":"
            + Json.quote(key == null ? "" : key) + ",\"code\":"
            + Json.quote(code == null ? "" : code) + ",\"keycode\":" + keyCode
            + ",\"mods\":" + modifiers + "}").text();
        return new KeyResult(Json.stringOrNull(json, "href"), Json.flag(json, "prevented"));
    }

    /** Insert {@code text} into the focused field, as typing it would. */
    public KeyResult type(String text) {
        return key(KeyKind.TEXT, text, "", 0, 0);
    }

    /**
     * Press and release a named key: {@code Enter}, {@code Tab},
     * {@code Backspace}, {@code ArrowDown} and the other
     * {@code KeyboardEvent.key} names.
     */
    public KeyResult press(String keyName) {
        KeyResult down = key(KeyKind.DOWN, keyName, keyName, 0, 0);
        key(KeyKind.UP, keyName, keyName, 0, 0);
        return down;
    }

    /** Paste {@code text} at the caret; the page sees {@code paste} and {@code input} events. */
    public KeyResult paste(String text) {
        return key(KeyKind.PASTE, text, "", 0, 0);
    }

    /** Drive the text selection. Returns the text for {@link Selection#COPY} and {@link Selection#CUT}, else null. */
    public String select(Selection what, int x, int y) {
        return Json.stringOrNull(call("/select", "{\"kind\":" + what.wire + ",\"x\":" + x
            + ",\"y\":" + y + "}").text(), "href");
    }

    /** Find {@code query} in the page. */
    public Found find(String query, boolean caseSensitive, FindDirection direction, int fromY) {
        String json = call("/find", "{\"query\":" + Json.quote(query == null ? "" : query)
            + ",\"case_sensitive\":" + (caseSensitive ? 1 : 0) + ",\"direction\":"
            + direction.wire + ",\"from_y\":" + fromY + "}").text();
        return new Found(Json.integer(json, "total", 0), Json.integer(json, "current", 0),
                         Json.integer(json, "scroll_y", 0));
    }

    /** Whether a text field of the page has the focus, so typing should go to it. */
    public boolean hasFocusedEditable() {
        return Json.flag(call("/focused-editable", "{}").text(), "active");
    }

    /** The focused text field's value and selection. */
    public Editable focusedEditable() {
        String json = call("/focused-editable-state", "{}").text();
        return new Editable(Json.flag(json, "active"), Json.string(json, "value"),
                            Json.integer(json, "caret", 0), Json.integer(json, "anchor", 0));
    }

    /** Move the focused field's caret and selection anchor, in UTF-8 byte offsets. */
    public boolean setEditableSelection(int caret, int anchor) {
        return Json.flag(call("/focused-editable-selection",
            "{\"caret\":" + caret + ",\"anchor\":" + anchor + "}").text(), "ok");
    }

    /** The text the page last wrote to the clipboard, or null. */
    public String clipboardText() {
        return emptyToNull(call("/clipboard", "{}").text());
    }

    /**
     * Have the renderer fetch {@code downloadUrl} into a staging file. The
     * request carries the context's cookies, and a {@code blob:} or
     * {@code data:} URL the page built resolves.
     *
     * @return an id for {@link #pollDownload} and {@link #cancelDownload}, or
     *         0 when the download could not start
     */
    public long startDownload(String downloadUrl) {
        if (downloadUrl == null || downloadUrl.isEmpty()) {
            return 0;
        }
        return Json.longInteger(
            call("/download-start", "{\"url\":" + Json.quote(downloadUrl) + "}").text(), "id", 0);
    }

    /**
     * The state of a download. The renderer makes progress only while it
     * serves requests, so poll at a steady pace, with {@link #tick} or
     * {@link #render} in between.
     */
    public Download pollDownload(long id) {
        String json = call("/download-poll", "{\"id\":" + id + "}").text();
        if (!Json.flag(json, "known")) {
            return new Download(Download.State.FAILED, 0, 0, null, null, null,
                                "unknown download");
        }
        int state = Json.integer(json, "state", 2);
        String path = Json.stringOrNull(json, "path");
        return new Download(
            state == 0 ? Download.State.RUNNING
                : state == 1 ? Download.State.DONE : Download.State.FAILED,
            Json.longInteger(json, "bytes", 0), Json.longInteger(json, "total", 0),
            state == 1 && path != null ? Path.of(path) : null,
            Json.stringOrNull(json, "final_url"), Json.stringOrNull(json, "content_type"),
            Json.stringOrNull(json, "error"));
    }

    /** Stop a download and discard what it fetched. */
    public void cancelDownload(long id) {
        call("/download-cancel", "{\"id\":" + id + "}");
    }

    /**
     * Move a finished download to {@code destination}, after checking that
     * the file the renderer reported lies in this context's staging
     * directory, and mark it with its origin as the platform expects.
     */
    public void saveDownload(Download finished, Path destination) {
        if (finished.state() != Download.State.DONE || finished.stagedFile() == null) {
            throw new IllegalArgumentException("the download has not finished");
        }
        Downloads.moveIntoPlace(finished.stagedFile(), destination, finished.finalUrl(),
                                context.stagingDirectories("downloads"));
    }

    /**
     * Whether this host opens popups itself. Without it {@code window.open()}
     * navigates the page it was called from; with it the request arrives as
     * an {@code open} operation from {@link #popupOperations()}. Takes effect
     * on the next navigation.
     */
    public void setPopupsSupported(boolean supported) {
        this.popupsSupported = supported;
    }

    /**
     * Mark this page as a popup another page opened, so it gets a working
     * {@code window.opener}: {@code token} and {@code origin} come from the
     * opener's {@code open} operation. Takes effect on the next navigation.
     */
    public void setOpener(String token, String origin) {
        this.openerToken = token == null ? "" : token;
        this.openerOrigin = origin == null ? "" : origin;
    }

    /**
     * The popup operations the page queued, when {@link PageEvents#popups()}
     * said there are some. Each is a map of string fields; {@code op} is
     * {@code open} (with {@code url}, {@code token} and {@code origin}),
     * {@code post}, {@code call}, {@code port} or {@code close}.
     */
    public List<Map<String, String>> popupOperations() {
        List<Map<String, String>> operations = new ArrayList<>();
        Object parsed;
        try {
            parsed = Json.parse(call("/popup-poll", "{}").text());
        } catch (IllegalArgumentException e) {
            return operations;
        }
        if (!(parsed instanceof List<?> items)) {
            return operations;
        }
        for (Object item : items) {
            if (!(item instanceof Map<?, ?> fields)) {
                continue;
            }
            Map<String, String> operation = new LinkedHashMap<>();
            for (Map.Entry<?, ?> field : fields.entrySet()) {
                if (field.getValue() instanceof String value) {
                    operation.put(String.valueOf(field.getKey()), value);
                }
            }
            operations.add(operation);
        }
        return operations;
    }

    /**
     * Hand the page one operation from the window at the other end of a popup
     * link: {@code kind} is {@code message}, {@code call}, {@code port} or
     * {@code closed}.
     */
    public void deliverPopupOperation(String kind, Map<String, String> fields) {
        StringBuilder json = new StringBuilder("[{\"op\":").append(Json.quote(kind));
        for (Map.Entry<String, String> field : fields.entrySet()) {
            if (!field.getKey().equals("op") && field.getValue() != null) {
                json.append(',').append(Json.quote(field.getKey())).append(':')
                    .append(Json.quote(field.getValue()));
            }
        }
        call("/popup-deliver", json.append("}]").toString());
    }

    /** Answer a capability request; the page resumes or is refused. */
    public void resolveCapability(CapabilityRequest request, boolean allow) {
        running().exchange("/capability", "{\"kind\":" + Json.quote(request.kind())
            + ",\"origin\":" + Json.quote(request.origin()) + ",\"allow\":" + (allow ? 1 : 0)
            + "}", timeout.toMillis());
    }

    /** Whether the page's renderer is running. False before the first navigation and after a crash or timeout. */
    public synchronized boolean isAlive() {
        return renderer != null && renderer.isAlive();
    }

    public synchronized boolean isClosed() {
        return closed;
    }

    /** Stop the page's renderer. The page can be navigated again, which starts a fresh one. */
    public synchronized void stop() {
        if (renderer != null) {
            renderer.close();
            renderer = null;
        }
        this.title = "";
        this.url = "";
        this.serverAddress = null;
        this.security = Security.NONE;
        this.pageWidth = 0;
        this.pageHeight = 0;
    }

    /** Stop the renderer and leave the context. */
    @Override
    public void close() {
        synchronized (this) {
            if (closed) {
                return;
            }
            closed = true;
            if (renderer != null) {
                renderer.close();
                renderer = null;
            }
        }
        context.forget(this);
    }

    private RendererProcess.Response call(String path, String body) {
        return running().request(path, body, timeout.toMillis());
    }

    private RendererProcess running() {
        RendererProcess process = renderer(false);
        if (process == null) {
            throw new NordstjernenException("the page has no running renderer; navigate it first");
        }
        return process;
    }

    private synchronized RendererProcess runningOrNull() {
        return renderer != null && renderer.isAlive() ? renderer : null;
    }

    private synchronized RendererProcess renderer(boolean start) {
        if (closed) {
            throw new IllegalStateException("the page is closed");
        }
        if (renderer != null && renderer.isAlive()) {
            return renderer;
        }
        if (!start) {
            if (renderer != null) {
                throw new NordstjernenException("the page's renderer is no longer running (it"
                    + " crashed or passed a deadline); navigate the page to start a new one");
            }
            return null;
        }
        if (renderer != null) {
            renderer.close();
        }
        Engine.Options options = context.engine().options();
        renderer = new RendererProcess(context.engine().renderer().toString(),
                                       options.frameWidth(), options.frameHeight(),
                                       context.inMemory(), context.environment());
        renderer.setTimeoutMs(timeout.toMillis());
        engineVersion = renderer.engineVersion();
        renderer.request("/policy", resourcePolicy.toJson(), timeout.toMillis());
        if (colorScheme == ContextOptions.ColorScheme.DARK) {
            renderer.request("/colorscheme", "{\"dark\":1}", timeout.toMillis());
        }
        return renderer;
    }

    private static List<CapabilityRequest> capabilities(String capability, String camera) {
        List<CapabilityRequest> out = new ArrayList<>();
        if (capability != null) {
            for (String entry : capability.split(";")) {
                String[] parts = entry.strip().split(" ", 2);
                if (parts.length == 2 && !parts[0].isEmpty() && !parts[1].isEmpty()) {
                    out.add(new CapabilityRequest(parts[0], parts[1]));
                }
            }
        }
        if (out.isEmpty() && camera != null && !camera.isEmpty()) {
            out.add(new CapabilityRequest("camera", camera));
        }
        return List.copyOf(out);
    }

    private static List<String> audioCommands(String audio) {
        if (audio == null || audio.isEmpty()) {
            return List.of();
        }
        List<String> out = new ArrayList<>();
        for (String command : audio.split("\u001f")) {
            if (!command.isEmpty()) {
                out.add(command);
            }
        }
        return List.copyOf(out);
    }

    private static String downloadUrl(String raw) {
        if (raw == null) {
            return null;
        }
        int tab = raw.indexOf('\t');
        return emptyToNull(tab < 0 ? raw : raw.substring(0, tab));
    }

    private static String downloadName(String raw) {
        int tab = raw == null ? -1 : raw.indexOf('\t');
        return tab < 0 ? null : emptyToNull(raw.substring(tab + 1));
    }

    private static String emptyToNull(String s) {
        return s == null || s.isEmpty() ? null : s;
    }

    private static int header(RendererProcess.Response resp, String name, int fallback) {
        String value = resp.header(name);
        if (value == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static String decimal(double value) {
        if (!(value > 0) || Double.isInfinite(value)) {
            value = 1.0;
        }
        long milli = Math.round(value * 1000.0);
        return (milli / 1000) + "." + String.format("%03d", milli % 1000);
    }
}

/* Nordstjernen — the earlier renderer-session API, kept as a shim over org.nordstjernen.browser.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen;

import org.nordstjernen.browser.BrowserContext;
import org.nordstjernen.browser.CapabilityRequest;
import org.nordstjernen.browser.ContextOptions;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.browser.Page;
import org.nordstjernen.browser.PageEvents;
import org.nordstjernen.swing.FrameImages;

import java.awt.image.BufferedImage;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * A long-lived browser backed by one renderer process.
 *
 * @deprecated use {@link Engine}, {@link BrowserContext} and {@link Page} from
 *     {@code org.nordstjernen.browser}, which this class now only forwards
 *     to. They name things instead of numbering them, hand back pixels
 *     without needing {@code java.desktop}, and let pages share a profile.
 */
@Deprecated(since = "1.0.45")
public final class RemoteBrowser implements AutoCloseable {

    public static final int MAX_W = 6144;
    public static final int MAX_H = 3456;

    /** Result of a render: the image plus any side-channel the page requested. */
    public static final class Frame {
        public final BufferedImage image;
        public final String nav;
        public final boolean unchanged;
        public final boolean animating;
        public final boolean caretBlinking;
        public final String webgl;
        public final String camera;
        public final String download;
        public final String downloadName;
        public final String windowAction;
        public final String audio;
        public final boolean clipboard;
        public final int pageWidth;
        public final int pageHeight;
        public final int requestedScrollY;
        public final int requestedScrollX;
        public final boolean popup;
        public final String capability;

        Frame(org.nordstjernen.browser.Frame f) {
            PageEvents e = f.events();
            this.image = FrameImages.toImage(f);
            this.unchanged = f.unchanged() || image == null;
            this.animating = f.animating();
            this.caretBlinking = f.caretBlinking();
            this.pageWidth = f.pageWidth();
            this.pageHeight = f.pageHeight();
            this.requestedScrollY = f.requestedScrollY();
            this.requestedScrollX = f.requestedScrollX();
            this.nav = e.navigation();
            this.webgl = e.webglOrigin();
            this.camera = cameraOf(e);
            this.download = e.downloadUrl();
            this.downloadName = e.downloadName();
            this.windowAction = e.windowAction();
            this.audio = audioOf(e);
            this.clipboard = e.clipboardWrite();
            this.popup = e.popups();
            this.capability = capabilityOf(e);
        }
    }

    public static final class Hover {
        public final boolean changed;
        public final String href;
        public final String cursor;

        Hover(boolean changed, String href, String cursor) {
            this.changed = changed;
            this.href = href;
            this.cursor = cursor;
        }
    }

    public static final class Find {
        public final int total;
        public final int current;
        public final int scrollY;

        Find(int total, int current, int scrollY) {
            this.total = total;
            this.current = current;
            this.scrollY = scrollY;
        }
    }

    public static final class Media {
        public final String url;
        public final boolean video;
        public final boolean stream;

        Media(String url, boolean video, boolean stream) {
            this.url = url;
            this.video = video;
            this.stream = stream;
        }
    }

    public static final class ContextMenu {
        public final boolean prevented;
        public final boolean editable;

        ContextMenu(boolean prevented, boolean editable) {
            this.prevented = prevented;
            this.editable = editable;
        }
    }

    public static final class Editable {
        public final boolean active;
        public final String value;
        public final int caret;
        public final int anchor;

        Editable(boolean active, String value, int caret, int anchor) {
            this.active = active;
            this.value = value;
            this.caret = caret;
            this.anchor = anchor;
        }
    }

    public static final class Tick {
        public final boolean changed;
        public final boolean animating;
        public final int pageWidth;
        public final int pageHeight;
        public final String nav;
        public final String webgl;
        public final String camera;
        public final String download;
        public final String downloadName;
        public final String windowAction;
        public final String audio;
        public final boolean popup;
        public final String capability;

        Tick(org.nordstjernen.browser.Tick t) {
            PageEvents e = t.events();
            this.changed = t.changed();
            this.animating = t.animating();
            this.pageWidth = t.pageWidth();
            this.pageHeight = t.pageHeight();
            this.nav = e.navigation();
            this.webgl = e.webglOrigin();
            this.camera = cameraOf(e);
            this.download = e.downloadUrl();
            this.downloadName = e.downloadName();
            this.windowAction = e.windowAction();
            this.audio = audioOf(e);
            this.popup = e.popups();
            this.capability = capabilityOf(e);
        }
    }

    public enum Security { NONE, SECURE, UNTRUSTED, INSECURE }

    public static final class Key {
        public final String nav;
        public final boolean prevented;

        Key(String nav, boolean prevented) {
            this.nav = nav;
            this.prevented = prevented;
        }
    }

    public static final class Download {
        public static final int RUNNING = 0;
        public static final int DONE = 1;
        public static final int FAILED = 2;

        public final int state;
        public final long bytes;
        public final long total;
        public final String path;
        public final String finalUrl;
        public final String contentType;
        public final String error;

        Download(Page.Download d) {
            this.state = d.state().ordinal();
            this.bytes = d.bytes();
            this.total = d.total();
            this.path = d.stagedFile() == null ? null : d.stagedFile().toString();
            this.finalUrl = d.finalUrl();
            this.contentType = d.contentType();
            this.error = d.error();
        }
    }

    private static Engine sharedEngine;

    private final boolean privateMode;
    private final BrowserContext ownContext;
    private final Page page;
    private String pendingNav;

    public RemoteBrowser() {
        this(false);
    }

    public RemoteBrowser(boolean privateMode) {
        this.privateMode = privateMode;
        Engine engine = engine();
        this.ownContext = privateMode ? engine.newContext(ContextOptions.inMemory()) : null;
        this.page = (privateMode ? ownContext : engine.userContext()).newPage();
    }

    private static synchronized Engine engine() {
        if (sharedEngine == null || sharedEngine.isClosed()) {
            sharedEngine = Engine.launch(Engine.Options.defaults().withFrameSize(MAX_W, MAX_H));
        }
        return sharedEngine;
    }

    public boolean isPrivate() {
        return privateMode;
    }

    public void setTimeout(Duration timeout) {
        page.setTimeout(timeout);
    }

    public Duration timeout() {
        return page.timeout();
    }

    public String engineVersion() {
        return page.engineVersion();
    }

    public boolean isAlive() {
        return page.isAlive();
    }

    public void setDevicePixelRatio(double dpr) {
        page.setDevicePixelRatio(dpr);
    }

    public double devicePixelRatio() {
        return page.devicePixelRatio();
    }

    public boolean navigate(String url, int viewportWidthCss, int viewportHeightCss,
                            int settleMs) {
        return navigate(url, viewportWidthCss, viewportHeightCss, settleMs, false, false);
    }

    public boolean navigate(String url, int viewportWidthCss, int viewportHeightCss,
                            int settleMs, boolean fromHistory, boolean userActivated) {
        if (url == null || url.isEmpty()) {
            return false;
        }
        if (viewportWidthCss != page.viewportWidth()
            || viewportHeightCss != page.viewportHeight()) {
            page.setViewport(Math.max(1, viewportWidthCss), Math.max(1, viewportHeightCss));
        }
        var response = page.navigate(url, Page.Navigation.defaults()
            .withSettle(Duration.ofMillis(Math.max(0, settleMs)))
            .withFromHistory(fromHistory).withUserActivated(userActivated));
        pendingNav = response.redirect();
        return response.ok();
    }

    public String pendingNav() {
        return pendingNav;
    }

    public void setViewport(int widthCss, int heightCss) {
        page.setViewport(Math.max(1, widthCss), Math.max(1, heightCss));
    }

    public Frame render(int scrollX, int scrollY, int width, int height, double scale) {
        return render(scrollX, scrollY, width, height, scale, false);
    }

    public Frame render(int scrollX, int scrollY, int width, int height, double scale,
                        boolean caretActive) {
        return new Frame(page.render(scrollX, scrollY, width, height, scale, caretActive));
    }

    public String linkAt(int x, int y) {
        return page.linkAt(x, y);
    }

    public Hover hover(int x, int y) {
        Page.Hover h = page.hover(x, y);
        return new Hover(h.changed(), h.link(), h.cursor());
    }

    public boolean scrollAt(int x, int y, int dx, int dy) {
        return page.scrollAt(x, y, dx, dy);
    }

    public boolean scrollbarPress(int x, int y) {
        return page.scrollbarPress(x, y);
    }

    public boolean scrollbarDrag(int x, int y) {
        return page.scrollbarDrag(x, y);
    }

    public void scrollbarRelease() {
        page.scrollbarRelease();
    }

    public boolean contextMenu(int x, int y) {
        return page.contextMenu(x, y).handledByPage();
    }

    public ContextMenu openContextMenu(int x, int y) {
        Page.ContextMenu m = page.contextMenu(x, y);
        return new ContextMenu(m.handledByPage(), m.editable());
    }

    public boolean dropFiles(int x, int y, List<String> paths) {
        if (paths == null || paths.isEmpty()) {
            return false;
        }
        List<Path> files = new ArrayList<>();
        for (String path : paths) {
            files.add(Path.of(path));
        }
        return page.dropFiles(x, y, files);
    }

    public Tick tick() {
        return new Tick(page.tick());
    }

    public String select(int kind, int x, int y) {
        Page.Selection[] kinds = Page.Selection.values();
        return kind < 0 || kind >= kinds.length ? null : page.select(kinds[kind], x, y);
    }

    public Find find(String query, boolean caseSensitive, int direction, int fromY) {
        Page.FindDirection[] directions = Page.FindDirection.values();
        Page.Found f = page.find(query, caseSensitive,
            directions[Math.max(0, Math.min(direction, directions.length - 1))], fromY);
        return new Find(f.total(), f.current(), f.scrollY());
    }

    public String cutSelection() {
        return page.select(Page.Selection.CUT, 0, 0);
    }

    public String paste(String text) {
        return text == null || text.isEmpty() ? null : page.paste(text).navigation();
    }

    public String clipboardText() {
        return page.clipboardText();
    }

    public String eval(String src) {
        return page.evaluate(src);
    }

    public String consoleDrain() {
        return page.consoleOutput();
    }

    public String dump(String kind) {
        for (Page.Dump dump : Page.Dump.values()) {
            if (dump.name().equalsIgnoreCase(kind)) {
                return page.dump(dump);
            }
        }
        return null;
    }

    public boolean focusedEditable() {
        return page.hasFocusedEditable();
    }

    public Editable focusedEditableState() {
        Page.Editable e = page.focusedEditable();
        return new Editable(e.active(), e.value(), e.caret(), e.anchor());
    }

    public boolean setFocusedEditableSelection(int caret, int anchor) {
        return page.setEditableSelection(caret, anchor);
    }

    public Media mediaAt(int x, int y) {
        Page.Media m = page.mediaAt(x, y);
        return m == null ? null : new Media(m.url(), m.video(), m.stream());
    }

    public byte[] screenshotPng(double scale) {
        return page.screenshot(Page.Screenshot.fullPage().withScale(scale));
    }

    public byte[] screenshotPng(int x, int y, int width, int height, double scale) {
        return page.screenshot(new Page.Screenshot(x, y, width, height, scale));
    }

    public boolean export(String path) {
        try {
            page.export(Path.of(path));
            return true;
        } catch (RendererTimeoutException e) {
            throw e;
        } catch (NordstjernenException e) {
            return false;
        }
    }

    public long downloadStart(String url) {
        return page.startDownload(url);
    }

    public Download downloadPoll(long id) {
        return new Download(page.pollDownload(id));
    }

    public void downloadCancel(long id) {
        page.cancelDownload(id);
    }

    public void setPopupsSupported(boolean supported) {
        page.setPopupsSupported(supported);
    }

    public void setOpener(String token, String origin) {
        page.setOpener(token, origin);
    }

    public List<Map<String, String>> popupPoll() {
        return page.popupOperations();
    }

    public void popupDeliver(String kind, Map<String, String> fields) {
        page.deliverPopupOperation(kind, fields);
    }

    public static List<String[]> capabilityRequests(String capability) {
        List<String[]> out = new ArrayList<>();
        if (capability == null) {
            return out;
        }
        for (String entry : capability.split(";")) {
            String[] parts = entry.strip().split(" ", 2);
            if (parts.length == 2 && !parts[0].isEmpty() && !parts[1].isEmpty()) {
                out.add(parts);
            }
        }
        return out;
    }

    public void resolveCapability(String kind, String origin, boolean allow) {
        page.resolveCapability(new CapabilityRequest(kind, origin), allow);
    }

    public void resolveWebgl(String origin, boolean allow) {
        resolveCapability("webgl", origin, allow);
    }

    public void resolveCamera(String origin, boolean allow) {
        resolveCapability("camera", origin, allow);
    }

    public BufferedImage favicon() {
        return FrameImages.toImage(page.favicon());
    }

    public String press(int x, int y, int mods) {
        return page.mouseDown(x, y, mods);
    }

    public String release() {
        return page.mouseUp();
    }

    public Key key(int kind, String key, String code, int keycode, int mods) {
        Page.KeyKind[] kinds = Page.KeyKind.values();
        Page.KeyResult r = page.key(kinds[Math.max(0, Math.min(kind, kinds.length - 1))],
                                    key, code, keycode, mods);
        return new Key(r.navigation(), r.prevented());
    }

    public String title() {
        return page.title();
    }

    public String url() {
        return page.url();
    }

    public int pageWidth() {
        return page.pageWidth();
    }

    public int pageHeight() {
        return page.pageHeight();
    }

    public Security security() {
        return Security.valueOf(page.security().name());
    }

    public String serverIp() {
        return page.serverAddress();
    }

    public void restart() {
        page.stop();
    }

    @Override
    public void close() {
        page.close();
        if (ownContext != null) {
            ownContext.close();
        }
    }

    public static List<String> audioCommands(String audio) {
        List<String> out = new ArrayList<>();
        if (audio != null) {
            for (String line : audio.split("\u001f")) {
                if (!line.isEmpty()) {
                    out.add(line);
                }
            }
        }
        return out;
    }

    private static String cameraOf(PageEvents events) {
        for (CapabilityRequest request : events.capabilities()) {
            if (request.kind().equals("camera")) {
                return request.origin();
            }
        }
        return null;
    }

    private static String audioOf(PageEvents events) {
        return events.audio().isEmpty() ? null : String.join("\u001f", events.audio());
    }

    private static String capabilityOf(PageEvents events) {
        if (events.capabilities().isEmpty()) {
            return null;
        }
        StringBuilder out = new StringBuilder();
        for (CapabilityRequest request : events.capabilities()) {
            if (out.length() > 0) {
                out.append(';');
            }
            out.append(request.kind()).append(' ').append(request.origin());
        }
        return out.toString();
    }
}

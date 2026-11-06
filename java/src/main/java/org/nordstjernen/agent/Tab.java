/* Nordstjernen — one tab of an agent's browser: a page plus the shell's half of it.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.browser.CapabilityRequest;
import org.nordstjernen.browser.Frame;
import org.nordstjernen.browser.Page;
import org.nordstjernen.browser.PageEvents;
import org.nordstjernen.browser.PdfOptions;
import org.nordstjernen.browser.Response;
import org.nordstjernen.browser.Tick;
import org.nordstjernen.internal.Json;
import org.nordstjernen.internal.Png;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.Supplier;

/**
 * A tab of an {@link AgentBrowser}. Where a {@link Page} is the document
 * alone, a tab adds what a browser window supplies around it: the scroll
 * position, back and forward, following the navigations a click, a form or a
 * script asks for, and waiting for the page to settle after each action.
 *
 * <p>Elements are named by the references ({@code ref_12}) that
 * {@link #snapshot} and {@link #find} print. Points are in CSS pixels from
 * the top left of the viewport, the same pixels {@link #screenshot} shows.
 * Every action returns a short account of what happened, ending in the
 * tab's URL and title; a failed one throws {@link AgentException} with a
 * message that says what to try instead.
 */
public final class Tab {

    private static final double SYNC_SCALE = 0.25;
    private static final int MAX_REDIRECTS = 10;
    private static final long QUIET_MS = 250;
    private static final long ACTION_SETTLE_MS = 2500;
    private static final long LOAD_SETTLE_MS = 6000;
    private static final long DOWNLOAD_WAIT_MS = 120_000;
    private static final long EVALUATION_WAIT_MS = 30_000;
    private static final int LINE_SCROLL = 40;

    private record Point(int x, int y, String description, String coveredBy) {
    }

    private final AgentBrowser browser;
    private final int id;
    private final Page page;
    private final List<String> history = new ArrayList<>();
    private final List<String> notes = new ArrayList<>();
    private int historyIndex = -1;
    private int scrollX;
    private int scrollY;
    private String asked;

    Tab(AgentBrowser browser, int id, Page page) {
        this.browser = browser;
        this.id = id;
        this.page = page;
    }

    public int id() {
        return id;
    }

    /** The page behind the tab, for anything the tab has no method for. */
    public Page page() {
        return page;
    }

    /** The tab in one line: {@code Tab 2: https://example.com/ - Example Domain}. */
    public String describe() {
        String title = page.title();
        String url = page.url();
        return "Tab " + id + ": " + (url.isEmpty() ? "about:blank" : url)
            + (title.isEmpty() ? "" : " - " + title);
    }

    /**
     * Load an address and wait for the page to settle. A bare host such as
     * {@code example.com} is taken as {@code https://example.com}.
     */
    public String navigate(String address) {
        return act("Loaded the page.", LOAD_SETTLE_MS, () -> {
            open(toUrl(address), true, true, false);
            return null;
        });
    }

    public String back() {
        return step(-1);
    }

    public String forward() {
        return step(1);
    }

    public String reload() {
        return act("Reloaded the page.", LOAD_SETTLE_MS, () -> {
            follow(page.reload(), false);
            return null;
        });
    }

    /**
     * The page as an outline: one line per element with its role, name,
     * reference, state and position, nested as the document is.
     *
     * @param interactiveOnly only the elements that can be acted on, as a flat list
     * @param maxChars        where to cut the outline off
     */
    public String snapshot(boolean interactiveOnly, int maxChars) {
        synchronized (browser) {
            syncScroll();
            return helper("readPage(" + Json.quote(interactiveOnly ? "interactive" : "all") + ","
                + Math.max(1000, maxChars) + ")");
        }
    }

    /**
     * The elements matching a CSS selector, or failing that those whose
     * text, accessible name, placeholder or value contains {@code query}.
     */
    public String find(String query, int limit) {
        synchronized (browser) {
            syncScroll();
            return helper("find(" + Json.quote(query) + "," + Math.max(1, limit) + ")");
        }
    }

    /** The page's title, URL and readable text. */
    public String text(int maxChars) {
        synchronized (browser) {
            int limit = Math.max(1000, maxChars);
            try {
                return helper("pageText(" + limit + ")");
            } catch (AgentException scriptingOff) {
                String text = page.text();
                String body = text == null ? "" : text.strip();
                return "Title: " + page.title() + "\nURL: " + page.url() + "\n\n"
                    + (body.length() > limit ? body.substring(0, limit) + "\n[text truncated]"
                                             : body);
            }
        }
    }

    /** Click the middle of an element, scrolling it into view first. */
    public String click(String ref) {
        return act(null, ACTION_SETTLE_MS, () -> pressAt(locate(ref), 0, 1));
    }

    /** Click a point of the viewport. */
    public String click(int x, int y) {
        return act(null, ACTION_SETTLE_MS, () -> pressAt(viewportPoint(x, y), 0, 1));
    }

    /**
     * Click an element with keys held.
     *
     * @param modifiers names joined by {@code +}, such as {@code shift} or {@code ctrl+shift}
     */
    public String click(String ref, String modifiers) {
        int held = modifiers == null || modifiers.isBlank() ? 0
            : KeyStroke.parse(modifiers + "+a").modifiers();
        return act(null, ACTION_SETTLE_MS, () -> pressAt(locate(ref), held, 1));
    }

    public String doubleClick(String ref) {
        return act(null, ACTION_SETTLE_MS, () -> pressAt(locate(ref), 0, 2));
    }

    public String doubleClick(int x, int y) {
        return act(null, ACTION_SETTLE_MS, () -> pressAt(viewportPoint(x, y), 0, 2));
    }

    /** Move the pointer over an element, as for a menu that opens on hover. */
    public String hover(String ref) {
        return act(null, ACTION_SETTLE_MS, () -> hoverAt(locate(ref)));
    }

    public String hover(int x, int y) {
        return act(null, ACTION_SETTLE_MS, () -> hoverAt(viewportPoint(x, y)));
    }

    /**
     * Type text into the focused field, or into {@code ref} after clicking
     * it when one is given. A newline presses Enter.
     */
    public String type(String ref, String text) {
        return act(null, ACTION_SETTLE_MS, () -> {
            String target = ref == null || ref.isEmpty() ? null : pressAt(locate(ref), 0, 1);
            String[] lines = text.split("\n", -1);
            for (int i = 0; i < lines.length; i++) {
                if (i > 0) {
                    press(KeyStroke.parse("Enter"));
                }
                if (!lines[i].isEmpty()) {
                    leave(page.type(lines[i]).navigation());
                }
            }
            return (target == null ? "" : target + "\n") + "Typed " + text.length()
                + " characters.";
        });
    }

    /**
     * Press keys or shortcuts, separated by spaces: {@code Enter},
     * {@code Tab}, {@code ctrl+a}, {@code ArrowDown ArrowDown Enter}.
     *
     * @param repeat how many times to press the whole sequence
     */
    public String press(String keys, int repeat) {
        return act(null, ACTION_SETTLE_MS, () -> {
            String[] names = keys.trim().split("\\s+");
            for (int round = 0; round < Math.max(1, repeat); round++) {
                for (String name : names) {
                    press(KeyStroke.parse(name));
                }
            }
            return "Pressed " + keys.trim() + (repeat > 1 ? " " + repeat + " times." : ".");
        });
    }

    /**
     * Turn the wheel over an element, or over the middle of the viewport
     * when {@code ref} is null. Whatever scrolls under that point scrolls;
     * otherwise the page does.
     *
     * @param direction {@code up}, {@code down}, {@code left} or {@code right}
     * @param pixels    how far, in CSS pixels; 0 means most of a viewport
     */
    public String scroll(String direction, int pixels, String ref) {
        return act(null, ACTION_SETTLE_MS, () -> {
            Point at = ref == null || ref.isEmpty()
                ? viewportPoint(page.viewportWidth() / 2, page.viewportHeight() / 2)
                : locate(ref);
            return wheel(at, direction, pixels);
        });
    }

    /** Scroll an element into the middle of the viewport. */
    public String scrollTo(String ref) {
        return act(null, ACTION_SETTLE_MS, () -> "Scrolled to " + locate(ref).description());
    }

    /**
     * Set a form field: the text of an input, textarea or editable element,
     * the option of a select by value or label (a list for a multiple
     * select), true or false for a checkbox or radio button. The page sees
     * {@code input} and {@code change} events.
     */
    public String setValue(String ref, Object value) {
        return act(null, ACTION_SETTLE_MS,
                   () -> helper("setValue(" + Json.quote(ref) + "," + Json.write(value) + ")"));
    }

    /**
     * Run JavaScript in the page and return the value of its last
     * expression, objects as JSON; a promise is waited for.
     */
    public String evaluate(String code) {
        synchronized (browser) {
            notes.clear();
            Map<?, ?> reply = outcome(helper("run(" + Json.quote(code) + ")"));
            long deadline = System.currentTimeMillis() + EVALUATION_WAIT_MS;
            while (reply.get("pending") != null) {
                if (System.currentTimeMillis() > deadline) {
                    throw new AgentException("the promise did not settle in "
                        + EVALUATION_WAIT_MS / 1000 + " seconds");
                }
                settle(0, 0);
                pause(30);
                reply = outcome(helper("poll(" + reply.get("pending") + ")"));
            }
            settle(QUIET_MS, ACTION_SETTLE_MS);
            if (reply.get("error") != null) {
                throw new AgentException(String.valueOf(reply.get("error")));
            }
            return String.valueOf(reply.get("value")) + noted();
        }
    }

    /** The viewport as a PNG, one image pixel per CSS pixel. */
    public byte[] screenshot() {
        synchronized (browser) {
            syncScroll();
            int width = page.viewportWidth();
            int height = page.viewportHeight();
            Frame frame = page.render(scrollX, scrollY, width, height, 1.0, false);
            if (frame.pixels() == null) {
                throw new AgentException("the page has nothing to show yet");
            }
            return Png.encodeBgra(frame.pixels(), frame.width(), frame.height(), frame.stride());
        }
    }

    /** The whole document as a paged PDF. */
    public byte[] pdf(PdfOptions options) {
        synchronized (browser) {
            return page.pdf(options);
        }
    }

    /** Let the page run for a while, following what it asks for. */
    public String waitFor(long millis) {
        synchronized (browser) {
            notes.clear();
            settle(Long.MAX_VALUE, Math.max(0, Math.min(millis, 60_000)));
            return "Waited " + millis + " ms." + noted() + "\n" + describe();
        }
    }

    void note(String text) {
        notes.add(text);
    }

    void openFromPage(String url) {
        open(url, false, true, false);
        settle(QUIET_MS, LOAD_SETTLE_MS);
    }

    private String act(String done, long settleMs, Supplier<String> action) {
        synchronized (browser) {
            notes.clear();
            String said = action.get();
            settle(QUIET_MS, settleMs);
            return (said == null ? done : said) + noted() + "\n" + describe();
        }
    }

    private String noted() {
        StringBuilder out = new StringBuilder();
        for (String note : notes) {
            out.append('\n').append(note);
        }
        notes.clear();
        return out.toString();
    }

    private String step(int by) {
        return act(by < 0 ? "Went back." : "Went forward.", LOAD_SETTLE_MS, () -> {
            int target = historyIndex + by;
            if (target < 0 || target >= history.size()) {
                throw new AgentException(by < 0 ? "there is no earlier page in this tab"
                                                : "there is no later page in this tab");
            }
            historyIndex = target;
            open(history.get(target), true, false, true);
            return null;
        });
    }

    private void open(String url, boolean userActivated, boolean record, boolean fromHistory) {
        Page.Navigation how = Page.Navigation.defaults().withUserActivated(userActivated)
            .withFromHistory(fromHistory);
        Response response = page.navigate(url, how);
        if (!response.ok()) {
            throw new AgentException("could not open " + url);
        }
        scrollX = 0;
        scrollY = 0;
        if (record) {
            while (history.size() > historyIndex + 1) {
                history.remove(history.size() - 1);
            }
            history.add(response.url());
            historyIndex = history.size() - 1;
        }
        follow(response, fromHistory);
    }

    private void follow(Response response, boolean fromHistory) {
        Response current = response;
        for (int hops = 0; current.ok() && current.redirect() != null && hops < MAX_REDIRECTS;
                hops++) {
            current = page.navigate(current.redirect(),
                                    Page.Navigation.defaults().withUserActivated(false));
        }
        scrollX = 0;
        scrollY = 0;
        if (!fromHistory && historyIndex >= 0 && current.ok()) {
            history.set(historyIndex, current.url());
        }
    }

    private void leave(String url) {
        if (url != null && !url.isEmpty()) {
            open(url, true, true, false);
        }
    }

    /**
     * Run the page until it has been still for {@code quietMs}, or
     * {@code maxMs} have passed, doing the window's part meanwhile.
     */
    private void settle(long quietMs, long maxMs) {
        long deadline = System.currentTimeMillis() + maxMs;
        long stillSince = System.currentTimeMillis();
        int navigations = 0;
        while (page.isAlive()) {
            boolean busy = turn();
            String navigation = asked;
            asked = null;
            if (navigation != null && navigations++ < MAX_REDIRECTS) {
                open(navigation, false, true, false);
                busy = true;
            }
            long now = System.currentTimeMillis();
            if (busy) {
                stillSince = now;
            }
            if (now >= deadline || now - stillSince >= quietMs) {
                return;
            }
            pause(15);
        }
    }

    /**
     * One turn of the loop a window runs: paint the page where the tab is
     * scrolled, which is how the engine learns the scroll position, then let
     * its timers and scripts run, and take in what it asks of its window.
     * True when the page changed or asked to be scrolled.
     */
    private boolean turn() {
        int width = Math.max(1, (int) Math.round(page.viewportWidth() * SYNC_SCALE));
        int height = Math.max(1, (int) Math.round(page.viewportHeight() * SYNC_SCALE));
        Frame frame = page.render(scrollX, scrollY, width, height, SYNC_SCALE, false);
        boolean busy = adoptScroll(frame) || !frame.unchanged();
        attend(frame.events());
        Tick tick = page.tick();
        attend(tick.events());
        return busy || tick.changed();
    }

    private boolean adoptScroll(Frame frame) {
        boolean moved = false;
        if (frame.requestedScrollX() >= 0 && frame.requestedScrollX() != scrollX) {
            scrollX = frame.requestedScrollX();
            moved = true;
        }
        if (frame.requestedScrollY() >= 0 && frame.requestedScrollY() != scrollY) {
            scrollY = frame.requestedScrollY();
            moved = true;
        }
        return moved;
    }

    private void syncScroll() {
        if (!page.isAlive()) {
            throw new AgentException("the tab has no page yet; navigate it first");
        }
        int x;
        int y;
        int turns = 0;
        do {
            x = scrollX;
            y = scrollY;
            turn();
        } while ((x != scrollX || y != scrollY) && ++turns < 4);
    }

    private void attend(PageEvents events) {
        if (events.popups()) {
            browser.handlePopupOperations(this, page.popupOperations());
        }
        for (CapabilityRequest request : events.capabilities()) {
            page.resolveCapability(request, false);
            notes.add("Declined the page's request to use " + request.kind() + ".");
        }
        if (events.downloadUrl() != null) {
            download(events.downloadUrl(), events.downloadName());
        }
        if (events.navigation() != null) {
            asked = events.navigation();
        }
    }

    private void download(String url, String suggestedName) {
        Path directory = browser.options().downloads();
        String name = fileName(suggestedName, url);
        if (directory == null) {
            notes.add("The page offered the file " + name
                + "; it was not saved because no download directory is set.");
            return;
        }
        long download = page.startDownload(url);
        long deadline = System.currentTimeMillis() + DOWNLOAD_WAIT_MS;
        Page.Download state = page.pollDownload(download);
        while (state.state() == Page.Download.State.RUNNING
                && System.currentTimeMillis() < deadline) {
            page.tick();
            pause(20);
            state = page.pollDownload(download);
        }
        if (state.state() != Page.Download.State.DONE) {
            page.cancelDownload(download);
            notes.add("The download of " + name + " failed: "
                + (state.error() == null ? "it took too long" : state.error()) + ".");
            return;
        }
        try {
            Files.createDirectories(directory);
            Path destination = unused(directory, name);
            page.saveDownload(state, destination);
            notes.add("Downloaded " + destination + " (" + state.bytes() + " bytes).");
        } catch (IOException | RuntimeException e) {
            notes.add("The download of " + name + " could not be saved: " + e.getMessage());
        }
    }

    private static String fileName(String suggested, String url) {
        String name = suggested == null ? "" : suggested;
        if (name.isBlank()) {
            String path = url.replaceFirst("[?#].*$", "");
            name = path.substring(path.lastIndexOf('/') + 1);
        }
        name = name.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").strip();
        while (name.startsWith(".")) {
            name = name.substring(1);
        }
        return name.isEmpty() ? "download" : name;
    }

    private static Path unused(Path directory, String name) {
        Path candidate = directory.resolve(name);
        int dot = name.lastIndexOf('.');
        String stem = dot > 0 ? name.substring(0, dot) : name;
        String extension = dot > 0 ? name.substring(dot) : "";
        for (int n = 1; Files.exists(candidate); n++) {
            candidate = directory.resolve(stem + " (" + n + ")" + extension);
        }
        return candidate;
    }

    private String helper(String invocation) {
        if (!page.isAlive()) {
            throw new AgentException("the tab has no page yet; navigate it first");
        }
        return PageScript.call(page, invocation);
    }

    private static Map<?, ?> outcome(String json) {
        return Json.parse(json) instanceof Map<?, ?> map ? map : Map.of("value", json);
    }

    private Point locate(String ref) {
        if (ref == null || ref.isBlank()) {
            throw new AgentException("an element reference such as ref_12 is needed");
        }
        syncScroll();
        Map<?, ?> found = outcome(helper("point(" + Json.quote(ref) + ")"));
        if (Boolean.TRUE.equals(found.get("scrolled"))) {
            syncScroll();
            found = outcome(helper("point(" + Json.quote(ref) + ")"));
        }
        Object covered = found.get("covered_by");
        return new Point((int) Math.round(number(found.get("px"))),
                         (int) Math.round(number(found.get("py"))),
                         String.valueOf(found.get("desc")),
                         covered == null ? null : covered.toString());
    }

    private Point viewportPoint(int x, int y) {
        syncScroll();
        if (x < 0 || y < 0 || x >= page.viewportWidth() || y >= page.viewportHeight()) {
            throw new AgentException("the point " + x + "," + y + " is outside the "
                + page.viewportWidth() + "x" + page.viewportHeight() + " viewport");
        }
        return new Point(x + scrollX, y + scrollY, "the point " + x + "," + y, null);
    }

    private static double number(Object value) {
        return value instanceof Number n ? n.doubleValue() : 0;
    }

    private String pressAt(Point at, int modifiers, int times) {
        String link = null;
        for (int i = 0; i < times && link == null; i++) {
            page.mouseDown(at.x(), at.y(), modifiers);
            link = page.mouseUp();
        }
        leave(link);
        return (times > 1 ? "Double-clicked " : "Clicked ") + at.description() + covered(at);
    }

    private String hoverAt(Point at) {
        page.hover(at.x(), at.y());
        return "Moved the pointer over " + at.description() + covered(at);
    }

    private static String covered(Point at) {
        return at.coveredBy() == null ? ""
            : "\nWarning: its centre is covered by " + at.coveredBy();
    }

    private String wheel(Point at, String direction, int pixels) {
        String way = direction == null ? "" : direction.toLowerCase(Locale.ROOT);
        if (!List.of("up", "down", "left", "right").contains(way)) {
            throw new AgentException("direction must be up, down, left or right");
        }
        boolean horizontal = way.equals("left") || way.equals("right");
        int sign = way.equals("down") || way.equals("right") ? 1 : -1;
        int span = horizontal ? page.viewportWidth() : page.viewportHeight();
        int by = sign * (pixels > 0 ? pixels : span * 4 / 5);
        int dx = horizontal ? by : 0;
        int dy = horizontal ? 0 : by;
        if (!page.scrollAt(at.x(), at.y(), dx, dy)) {
            scrollBy(dx, dy);
        }
        syncScroll();
        return "Scrolled " + way + " by " + Math.abs(by)
            + " px; the page is now at " + scrollX + "," + scrollY + " of "
            + page.pageWidth() + "x" + page.pageHeight() + ".";
    }

    private void scrollBy(int dx, int dy) {
        scrollX = clamp(scrollX + dx, page.pageWidth() - page.viewportWidth());
        scrollY = clamp(scrollY + dy, page.pageHeight() - page.viewportHeight());
    }

    private static int clamp(int value, int max) {
        return Math.max(0, Math.min(value, Math.max(0, max)));
    }

    private void press(KeyStroke stroke) {
        if (stroke.isShortcut('a')) {
            selectAll(stroke);
            return;
        }
        Page.KeyResult down = page.key(Page.KeyKind.DOWN, stroke.key(), stroke.code(),
                                       stroke.keyCode(), stroke.modifiers());
        String navigation = down.navigation();
        if (navigation == null && stroke.typesText()) {
            navigation = page.key(Page.KeyKind.PRESS, stroke.key(), stroke.code(),
                                  stroke.keyCode(), stroke.modifiers()).navigation();
        }
        page.key(Page.KeyKind.UP, stroke.key(), stroke.code(), stroke.keyCode(),
                 stroke.modifiers());
        if (navigation != null) {
            leave(navigation);
        } else if (!down.prevented() && stroke.modifiers() == 0 && !page.hasFocusedEditable()) {
            scrollForKey(stroke.key());
        }
    }

    private void selectAll(KeyStroke stroke) {
        boolean prevented = page.key(Page.KeyKind.DOWN, stroke.key(), stroke.code(),
                                     stroke.keyCode(), stroke.modifiers()).prevented();
        if (!prevented) {
            page.select(Page.Selection.ALL, 0, 0);
        }
        page.key(Page.KeyKind.UP, stroke.key(), stroke.code(), stroke.keyCode(),
                 stroke.modifiers());
    }

    private void scrollForKey(String key) {
        int pageStep = page.viewportHeight() * 4 / 5;
        switch (key) {
            case "ArrowDown" -> scrollBy(0, LINE_SCROLL);
            case "ArrowUp" -> scrollBy(0, -LINE_SCROLL);
            case "ArrowRight" -> scrollBy(LINE_SCROLL, 0);
            case "ArrowLeft" -> scrollBy(-LINE_SCROLL, 0);
            case "PageDown", " " -> scrollBy(0, pageStep);
            case "PageUp" -> scrollBy(0, -pageStep);
            case "Home" -> scrollY = 0;
            case "End" -> scrollBy(0, page.pageHeight());
            default -> { }
        }
    }

    private static String toUrl(String address) {
        String text = address == null ? "" : address.strip();
        if (text.isEmpty()) {
            throw new AgentException("an address is needed");
        }
        if (text.matches("(?i)^([a-z][a-z0-9+.-]*://|about:|data:|file:|blob:|view-source:).*")) {
            return text;
        }
        if (text.matches("(?i)^(localhost|\\[[0-9a-f:]+]|[0-9.]+)(:\\d+)?(/.*)?$")) {
            return "http://" + text;
        }
        return "https://" + text;
    }

    private static void pause(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new AgentException("interrupted");
        }
    }
}

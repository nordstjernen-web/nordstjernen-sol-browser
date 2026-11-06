/* Nordstjernen — a browser for a program or a model to drive: tabs without a window.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.browser.BrowserContext;
import org.nordstjernen.browser.ContextOptions;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.browser.Page;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A headless browser made for agents: tabs that keep their own scroll
 * position and history, follow the navigations a page asks for, open the
 * windows it opens as new tabs, and describe the page as an outline whose
 * elements carry references to act on.
 *
 * <pre>{@code
 * try (AgentBrowser browser = AgentBrowser.launch()) {
 *     Tab tab = browser.newTab("https://example.com");
 *     String outline = tab.snapshot(false, 40_000);   // ... link "More information..." [ref_3] ...
 *     tab.click("ref_3");
 *     byte[] png = tab.screenshot();
 * }
 * }</pre>
 *
 * <p>One action runs at a time across the whole browser; every method may
 * be called from any thread. {@link BrowserTools} offers the same actions as
 * named tools with JSON schemas, and {@link McpServer} serves those over the
 * Model Context Protocol.
 */
public final class AgentBrowser implements AutoCloseable {

    /**
     * How an agent's browser is set up.
     *
     * @param engine         how the engine is found and how long a request may take
     * @param context        the profile, resource policy and identity of the tabs
     * @param viewportWidth  the tabs' viewport width in CSS pixels
     * @param viewportHeight the tabs' viewport height in CSS pixels
     * @param downloads      where files a page offers are saved, or null to decline them
     */
    public record Options(Engine.Options engine, ContextOptions context, int viewportWidth,
                          int viewportHeight, Path downloads) {

        public Options {
            if (engine == null || context == null) {
                throw new IllegalArgumentException("engine and context must not be null");
            }
            if (viewportWidth < 200 || viewportHeight < 200) {
                throw new IllegalArgumentException("the viewport must be at least 200 pixels a side");
            }
        }

        /** A 1280 by 800 viewport, a profile deleted on close, no downloads. */
        public static Options defaults() {
            return new Options(Engine.Options.defaults(), ContextOptions.ephemeral(), 1280, 800,
                               null);
        }

        public Options withEngine(Engine.Options engine) {
            return new Options(engine, context, viewportWidth, viewportHeight, downloads);
        }

        public Options withContext(ContextOptions context) {
            return new Options(engine, context, viewportWidth, viewportHeight, downloads);
        }

        public Options withViewport(int width, int height) {
            return new Options(engine, context, width, height, downloads);
        }

        public Options withDownloads(Path directory) {
            return new Options(engine, context, viewportWidth, viewportHeight, directory);
        }
    }

    private record PopupLink(String token, Tab opener, Tab popup) {
    }

    private static final String[] POPUP_FIELDS = {
        "token", "to", "data", "origin", "target", "path", "ports", "id"
    };

    private final Options options;
    private final Engine engine;
    private final BrowserContext context;
    private final List<Tab> tabs = new ArrayList<>();
    private final Map<String, PopupLink> popupLinks = new HashMap<>();
    private Tab active;
    private int nextTabId = 1;
    private boolean closed;

    private AgentBrowser(Options options) {
        this.options = options;
        this.engine = Engine.launch(options.engine());
        try {
            this.context = engine.newContext(options.context());
        } catch (RuntimeException e) {
            engine.close();
            throw e;
        }
    }

    public static AgentBrowser launch() {
        return new AgentBrowser(Options.defaults());
    }

    public static AgentBrowser launch(Options options) {
        return new AgentBrowser(options);
    }

    public Options options() {
        return options;
    }

    /** Open a tab, make it the active one and load {@code address} in it; null opens a blank tab. */
    public synchronized Tab newTab(String address) {
        Tab tab = addTab();
        active = tab;
        if (address != null && !address.isBlank()) {
            tab.navigate(address);
        }
        return tab;
    }

    /** The open tabs, oldest first. */
    public synchronized List<Tab> tabs() {
        return List.copyOf(tabs);
    }

    /** The tab actions go to when none is named, opening one if there is none. */
    public synchronized Tab activeTab() {
        if (active == null) {
            active = addTab();
        }
        return active;
    }

    /** The tab with this id; 0 or less means the active tab. */
    public synchronized Tab tab(int id) {
        if (id <= 0) {
            return activeTab();
        }
        for (Tab tab : tabs) {
            if (tab.id() == id) {
                return tab;
            }
        }
        throw new AgentException("there is no tab " + id + "; the open tabs are " + tabIds());
    }

    public synchronized void selectTab(int id) {
        active = tab(id);
    }

    /** Close a tab; the newest remaining one becomes active. */
    public synchronized void closeTab(int id) {
        Tab tab = tab(id);
        tabs.remove(tab);
        unlinkPopups(tab);
        tab.page().close();
        if (active == tab) {
            active = tabs.isEmpty() ? null : tabs.get(tabs.size() - 1);
        }
    }

    /** One line per tab: id, URL and title, the active one marked. */
    public synchronized String describeTabs() {
        if (tabs.isEmpty()) {
            return "No tabs are open.";
        }
        StringBuilder out = new StringBuilder();
        for (Tab tab : tabs) {
            out.append(tab.describe()).append(tab == active ? " (active)" : "").append('\n');
        }
        return out.toString().stripTrailing();
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        tabs.clear();
        popupLinks.clear();
        context.close();
        engine.close();
    }

    private Tab addTab() {
        if (closed) {
            throw new IllegalStateException("the browser is closed");
        }
        Page page = context.newPage(options.viewportWidth(), options.viewportHeight());
        page.setPopupsSupported(true);
        Tab tab = new Tab(this, nextTabId++, page);
        tabs.add(tab);
        return tab;
    }

    private String tabIds() {
        List<Integer> ids = new ArrayList<>();
        for (Tab tab : tabs) {
            ids.add(tab.id());
        }
        return ids.toString();
    }

    /**
     * Carry out what a page queued for its windows: open the ones it asked
     * for as tabs, and pass messages between a window and its opener,
     * checking that the sender is the end allowed to send them.
     */
    void handlePopupOperations(Tab from, List<Map<String, String>> operations) {
        for (Map<String, String> op : operations) {
            String kind = op.getOrDefault("op", "");
            if (kind.equals("open")) {
                openPopup(from, op);
                continue;
            }
            PopupLink link = popupLinks.get(op.get("token"));
            if (link == null || (link.opener() != from && link.popup() != from)) {
                continue;
            }
            boolean fromPopup = link.popup() == from;
            boolean toPopup = "popup".equals(op.get("to"));
            switch (kind) {
                case "post" -> {
                    if (toPopup != fromPopup) {
                        deliver(toPopup ? link.popup() : link.opener(), "message", op);
                    }
                }
                case "call" -> {
                    if (fromPopup) {
                        deliver(link.opener(), "call", op);
                    }
                }
                case "port" -> deliver(fromPopup ? link.opener() : link.popup(), "port", op);
                case "close" -> closePopup(link);
                default -> { }
            }
        }
    }

    private void openPopup(Tab opener, Map<String, String> op) {
        String url = op.get("url");
        String token = op.get("token");
        if (url == null || url.isEmpty()) {
            return;
        }
        boolean linked = token != null && !token.isEmpty() && !popupLinks.containsKey(token);
        Tab popup = addTab();
        if (linked) {
            popup.page().setOpener(token, op.getOrDefault("origin", ""));
            popupLinks.put(token, new PopupLink(token, opener, popup));
        }
        active = popup;
        opener.note("The page opened tab " + popup.id() + ", which is now the active tab.");
        popup.openFromPage(url);
    }

    private void closePopup(PopupLink link) {
        if (tabs.contains(link.popup())) {
            link.opener().note("Tab " + link.popup().id() + " closed itself.");
            closeTab(link.popup().id());
        }
    }

    private void deliver(Tab to, String kind, Map<String, String> op) {
        if (to == null || !tabs.contains(to) || !to.page().isAlive()) {
            return;
        }
        Map<String, String> fields = new LinkedHashMap<>();
        for (String field : POPUP_FIELDS) {
            if (op.get(field) != null) {
                fields.put(field, op.get(field));
            }
        }
        to.page().deliverPopupOperation(kind, fields);
    }

    private void unlinkPopups(Tab gone) {
        for (var it = popupLinks.values().iterator(); it.hasNext();) {
            PopupLink link = it.next();
            if (link.popup() == gone) {
                deliver(link.opener(), "closed", Map.of("token", link.token()));
                it.remove();
            } else if (link.opener() == gone) {
                it.remove();
            }
        }
    }
}

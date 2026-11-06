/* Nordstjernen — the agent browser's actions as a catalogue of tools.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.browser.PdfOptions;
import org.nordstjernen.internal.Json;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * The actions of an {@link AgentBrowser} as tools for a tool-calling model.
 * The names, arguments and descriptions are those of the desktop browser's
 * own MCP server, so a prompt written for one works with the other.
 *
 * <p>Hand {@link #tools()} to an agent framework by mapping each
 * {@link BrowserTool} to its tool type (name, description, JSON Schema) and
 * routing calls to {@link #call(String, String)}; {@link McpServer} does
 * exactly that for the Model Context Protocol.
 */
public final class BrowserTools {

    /** What to tell a model about using the tools together. */
    public static final String INSTRUCTIONS =
        "These tools drive a headless Nordstjernen browser. Start with navigate, then "
        + "read_page or find to get element refs (ref_N) and act on them with click, type, "
        + "form_input, hover or scroll. Use screenshot to see the page; its pixel "
        + "coordinates are what x/y arguments expect. Refs go stale after navigation: read "
        + "the page again.";

    private static final String TAB = "\"tab_id\":{\"type\":\"integer\",\"description\":"
        + "\"Tab to act on, from tabs_list. Defaults to the active tab.\"}";
    private static final String REF = "\"ref\":{\"type\":\"string\",\"description\":"
        + "\"Element reference such as ref_12 from read_page or find.\"}";
    private static final String XY = "\"x\":{\"type\":\"number\",\"description\":"
        + "\"Viewport x in screenshot pixels.\"},\"y\":{\"type\":\"number\",\"description\":"
        + "\"Viewport y in screenshot pixels.\"}";

    private final AgentBrowser browser;
    private final List<BrowserTool> tools = new ArrayList<>();

    public BrowserTools(AgentBrowser browser) {
        this.browser = browser;
        addTabTools();
        addReadingTools();
        addPointerTools();
        addInputTools();
        addOutputTools();
    }

    public AgentBrowser browser() {
        return browser;
    }

    public List<BrowserTool> tools() {
        return List.copyOf(tools);
    }

    /**
     * Run a tool with its arguments as JSON text. A tool that fails comes
     * back as an error result whose text says why, not as an exception.
     */
    public BrowserTool.Result call(String name, String argumentsJson) {
        Object parsed;
        try {
            parsed = argumentsJson == null || argumentsJson.isBlank()
                ? Map.of() : Json.parse(argumentsJson);
        } catch (IllegalArgumentException e) {
            return BrowserTool.Result.error("the arguments are not JSON: " + e.getMessage());
        }
        return call(name, parsed instanceof Map<?, ?> map ? stringKeys(map) : Map.of());
    }

    /** Run a tool with its arguments already parsed. */
    public BrowserTool.Result call(String name, Map<String, Object> arguments) {
        for (BrowserTool tool : tools) {
            if (tool.name().equals(name)) {
                try {
                    return tool.handler().call(arguments == null ? Map.of() : arguments);
                } catch (RuntimeException e) {
                    String message = e.getMessage();
                    return BrowserTool.Result.error(message == null || message.isBlank()
                        ? e.getClass().getSimpleName() : message);
                }
            }
        }
        return BrowserTool.Result.error("there is no tool named " + name);
    }

    private static Map<String, Object> stringKeys(Map<?, ?> map) {
        Map<String, Object> out = new java.util.LinkedHashMap<>();
        map.forEach((key, value) -> out.put(String.valueOf(key), value));
        return out;
    }

    private void add(String name, String title, String description, String properties,
                     String required, BrowserTool.Handler handler) {
        String schema = "{\"type\":\"object\",\"properties\":{" + properties + "}"
            + (required == null ? "" : ",\"required\":[" + required + "]") + "}";
        tools.add(new BrowserTool(name, title, description, schema, handler));
    }

    private void addTabTools() {
        add("tabs_list", "List tabs",
            "List the open browser tabs with their tab_id, URL, title and which one is active.",
            "", null, a -> text(browser.describeTabs()));
        add("tabs_create", "Open tab",
            "Open a new tab and make it the active one, optionally at a URL, and wait for it"
            + " to load.",
            "\"url\":{\"type\":\"string\",\"description\":\"Address to load. Defaults to a"
            + " blank tab.\"}", null,
            a -> text("Opened " + browser.newTab(string(a, "url")).describe()));
        add("tabs_close", "Close tab", "Close a tab (the active one by default).", TAB, null,
            a -> {
                Tab tab = tab(a);
                browser.closeTab(tab.id());
                return text("Closed tab " + tab.id() + ".\n" + browser.describeTabs());
            });
        add("tab_select", "Select tab", "Make a tab the active one.", TAB, "\"tab_id\"", a -> {
            browser.selectTab(integer(a, "tab_id", 0));
            return text(browser.describeTabs());
        });
        add("navigate", "Navigate",
            "Load a URL in a tab and wait for the page to finish loading. Pass \"back\" or"
            + " \"forward\" to move through the tab's history.",
            "\"url\":{\"type\":\"string\",\"description\":\"URL, \\\"back\\\" or"
            + " \\\"forward\\\".\"}," + TAB, "\"url\"", a -> {
                String url = required(a, "url");
                Tab tab = tab(a);
                return text(switch (url) {
                    case "back" -> tab.back();
                    case "forward" -> tab.forward();
                    default -> tab.navigate(url);
                });
            });
        add("reload", "Reload", "Reload the page in a tab and wait for it to load.", TAB, null,
            a -> text(tab(a).reload()));
    }

    private void addReadingTools() {
        add("read_page", "Read page",
            "Return an accessibility-tree-like outline of the page: roles, accessible names,"
            + " states and viewport bounding boxes (CSS pixels), with stable refs such as"
            + " ref_12 for use with the other tools. filter=\"interactive\" lists only"
            + " elements that can be acted on.",
            "\"filter\":{\"type\":\"string\",\"enum\":[\"all\",\"interactive\"],"
            + "\"description\":\"all (default) or interactive.\"},\"max_chars\":{\"type\":"
            + "\"integer\",\"description\":\"Truncate after this many characters (default"
            + " 40000).\"}," + TAB, null,
            a -> text(tab(a).snapshot("interactive".equals(string(a, "filter")),
                                      integer(a, "max_chars", 40_000))));
        add("find", "Find elements",
            "Find elements by CSS selector or by visible text, accessible name, placeholder"
            + " or value. Returns matching elements with refs.",
            "\"query\":{\"type\":\"string\",\"description\":\"CSS selector or text to look"
            + " for (case-insensitive).\"},\"limit\":{\"type\":\"integer\",\"description\":"
            + "\"Most matches to return (default 20).\"}," + TAB, "\"query\"",
            a -> text(tab(a).find(required(a, "query"), integer(a, "limit", 20))));
        add("get_page_text", "Get page text",
            "Return the page's readable text (title, URL and body text) without markup.",
            "\"max_chars\":{\"type\":\"integer\",\"description\":\"Truncate after this many"
            + " characters (default 100000).\"}," + TAB, null,
            a -> text(tab(a).text(integer(a, "max_chars", 100_000))));
        add("javascript", "Run JavaScript",
            "Evaluate JavaScript in the page's global scope and return the value of the last"
            + " expression (objects as JSON). A returned Promise is awaited.",
            "\"code\":{\"type\":\"string\"}," + TAB, "\"code\"",
            a -> text(tab(a).evaluate(required(a, "code"))));
    }

    private void addPointerTools() {
        add("click", "Click",
            "Left-click an element (by ref, scrolled into view first) or a viewport point"
            + " (x, y in screenshot pixels) with real mouse input, then wait for any"
            + " navigation to settle.",
            REF + "," + XY + ",\"modifiers\":{\"type\":\"string\",\"description\":\"Optional"
            + " held keys, e.g. \\\"shift\\\" or \\\"ctrl+shift\\\".\"}," + TAB, null, a -> {
                Tab tab = tab(a);
                String ref = string(a, "ref");
                if (ref == null) {
                    return text(tab.click(point(a, "x"), point(a, "y")));
                }
                return text(tab.click(ref, string(a, "modifiers")));
            });
        add("double_click", "Double-click",
            "Double-click an element (by ref) or a viewport point.", REF + "," + XY + "," + TAB,
            null, a -> {
                String ref = string(a, "ref");
                return text(ref == null ? tab(a).doubleClick(point(a, "x"), point(a, "y"))
                                        : tab(a).doubleClick(ref));
            });
        add("hover", "Hover",
            "Move the mouse pointer over an element (by ref) or a viewport point.",
            REF + "," + XY + "," + TAB, null, a -> {
                String ref = string(a, "ref");
                return text(ref == null ? tab(a).hover(point(a, "x"), point(a, "y"))
                                        : tab(a).hover(ref));
            });
        add("scroll", "Scroll",
            "Scroll with the mouse wheel at an element (by ref) or the centre of the"
            + " viewport. With only a ref and no direction, scrolls that element into view.",
            "\"direction\":{\"type\":\"string\",\"enum\":[\"up\",\"down\",\"left\","
            + "\"right\"]},\"amount\":{\"type\":\"number\",\"description\":\"Distance in CSS"
            + " pixels (default most of a viewport).\"}," + REF + "," + TAB, null, a -> {
                String direction = string(a, "direction");
                String ref = string(a, "ref");
                if (direction == null && ref == null) {
                    throw new AgentException("give a direction, a ref, or both");
                }
                return text(direction == null ? tab(a).scrollTo(ref)
                    : tab(a).scroll(direction, integer(a, "amount", 0), ref));
            });
    }

    private void addInputTools() {
        add("type", "Type text",
            "Type text into the focused element, or click the element given by ref first."
            + " Newlines press Enter.",
            "\"text\":{\"type\":\"string\"}," + REF + "," + TAB, "\"text\"",
            a -> text(tab(a).type(string(a, "ref"), required(a, "text"))));
        add("key", "Press keys",
            "Press keys or shortcuts, space separated, e.g. \"Enter\", \"Tab\", \"ctrl+a\","
            + " \"shift+Tab\", \"ArrowDown ArrowDown Enter\", \"Escape\", \"PageDown\".",
            "\"keys\":{\"type\":\"string\"},\"repeat\":{\"type\":\"integer\",\"description\":"
            + "\"Repeat the whole sequence this many times (default 1).\"}," + TAB, "\"keys\"",
            a -> text(tab(a).press(required(a, "keys"),
                                   Math.min(100, integer(a, "repeat", 1)))));
        add("form_input", "Set form value",
            "Set the value of an input, textarea, select (by option value or label),"
            + " checkbox or radio (true/false), or contenteditable element, firing input and"
            + " change events.",
            REF + ",\"value\":{\"description\":\"New value: string, number, boolean, or an"
            + " array of option values for a multiple select.\"}," + TAB, "\"ref\",\"value\"",
            a -> {
                if (!a.containsKey("value")) {
                    throw new AgentException("ref and value are required");
                }
                return text(tab(a).setValue(required(a, "ref"), a.get("value")));
            });
        add("wait", "Wait",
            "Wait for a number of milliseconds (at most 30000), e.g. for a page to update.",
            "\"ms\":{\"type\":\"integer\",\"description\":\"Milliseconds to wait (default"
            + " 1000).\"}," + TAB, null,
            a -> text(tab(a).waitFor(Math.min(30_000, integer(a, "ms", 1000)))));
    }

    private void addOutputTools() {
        add("screenshot", "Screenshot",
            "Capture the visible viewport of a tab as a PNG. Image pixels are CSS viewport"
            + " pixels, the same coordinates click and hover take.", TAB, null, a -> {
                Tab tab = tab(a);
                return BrowserTool.Result.image(tab.describe(), tab.screenshot());
            });
        add("save_pdf", "Save as PDF",
            "Save the whole page as a paged A4 PDF with selectable text and working links,"
            + " in the download directory, and return the file's path.",
            "\"filename\":{\"type\":\"string\",\"description\":\"File name to save as"
            + " (default page.pdf).\"},\"landscape\":{\"type\":\"boolean\"}," + TAB, null,
            this::savePdf);
    }

    private BrowserTool.Result savePdf(Map<String, Object> a) {
        Path directory = browser.options().downloads();
        if (directory == null) {
            throw new AgentException("no download directory is set, so there is nowhere to"
                + " save the PDF");
        }
        String name = string(a, "filename");
        name = name == null ? "page.pdf" : Path.of(name).getFileName().toString();
        if (!name.toLowerCase(java.util.Locale.ROOT).endsWith(".pdf")) {
            name += ".pdf";
        }
        PdfOptions options = Boolean.TRUE.equals(a.get("landscape"))
            ? PdfOptions.a4().landscape() : PdfOptions.a4();
        byte[] pdf = tab(a).pdf(options);
        try {
            Files.createDirectories(directory);
            Path file = directory.resolve(name);
            Files.write(file, pdf);
            return text("Saved " + file + " (" + pdf.length + " bytes).");
        } catch (IOException e) {
            throw new AgentException("could not save the PDF: " + e.getMessage());
        }
    }

    private Tab tab(Map<String, Object> arguments) {
        return browser.tab(integer(arguments, "tab_id", 0));
    }

    private static BrowserTool.Result text(String text) {
        return BrowserTool.Result.text(text);
    }

    private static String string(Map<String, Object> arguments, String key) {
        Object value = arguments.get(key);
        return value == null || value.toString().isEmpty() ? null : value.toString();
    }

    private static String required(Map<String, Object> arguments, String key) {
        String value = string(arguments, key);
        if (value == null) {
            throw new AgentException(key + " is required");
        }
        return value;
    }

    private static int integer(Map<String, Object> arguments, String key, int fallback) {
        Object value = arguments.get(key);
        if (value instanceof Number number) {
            return (int) Math.round(number.doubleValue());
        }
        if (value instanceof String text && text.matches("-?\\d+")) {
            return Integer.parseInt(text);
        }
        return fallback;
    }

    private static int point(Map<String, Object> arguments, String key) {
        if (!(arguments.get(key) instanceof Number)) {
            throw new AgentException("give a ref, or both x and y");
        }
        return integer(arguments, key, 0);
    }
}

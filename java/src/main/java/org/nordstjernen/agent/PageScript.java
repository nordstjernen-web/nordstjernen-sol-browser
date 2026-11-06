/* Nordstjernen — calls into the in-page helper that reads and acts on a document.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.NordstjernenException;
import org.nordstjernen.browser.Page;
import org.nordstjernen.internal.Json;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Map;

/**
 * The engine's in-page helper ({@code data/js/remote-page.js}, the one the
 * desktop browser's own MCP server evaluates): it outlines a document as
 * roles, names and element references, finds elements, and sets form
 * values. The helper installs itself in the page on first use and keeps the
 * references it handed out, so a later call can act on one.
 */
final class PageScript {

    private static final String RESOURCE = "remote-page.js";
    private static volatile String source;

    private PageScript() {
    }

    /**
     * Call a function of the helper, such as {@code readPage("all",40000)},
     * and return what it returned.
     *
     * @throws AgentException when the helper threw, with its message
     */
    static String call(Page page, String invocation) {
        String script = "(function(){try{return JSON.stringify({value:(" + source()
            + ")." + invocation + "})}catch(e){return JSON.stringify({error:String("
            + "e&&e.message||e)})}})()";
        String reply = page.evaluate(script);
        if (reply == null || reply.isEmpty()) {
            throw new AgentException("the page returned nothing; it may have no scripting");
        }
        Object parsed;
        try {
            parsed = Json.parse(reply);
        } catch (IllegalArgumentException e) {
            throw new AgentException("the page returned: " + reply);
        }
        if (!(parsed instanceof Map<?, ?> map)) {
            throw new AgentException("the page returned: " + reply);
        }
        if (map.get("error") != null) {
            throw new AgentException(String.valueOf(map.get("error")));
        }
        Object value = map.get("value");
        return value == null ? "" : value.toString();
    }

    private static String source() {
        String loaded = source;
        if (loaded == null) {
            try (InputStream in = PageScript.class.getResourceAsStream(RESOURCE)) {
                if (in == null) {
                    throw new NordstjernenException("the jar is missing " + RESOURCE);
                }
                loaded = new String(in.readAllBytes(), StandardCharsets.UTF_8).strip();
            } catch (IOException e) {
                throw new NordstjernenException("cannot read " + RESOURCE, e);
            }
            source = loaded;
        }
        return loaded;
    }
}

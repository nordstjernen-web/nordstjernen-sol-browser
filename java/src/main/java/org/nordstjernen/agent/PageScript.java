/* Nordstjernen — calls into the engine's remote-control helper that reads and acts on a document.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.browser.Page;
import org.nordstjernen.internal.Json;

/**
 * The engine's remote-control helper ({@code data/js/remote-page.js}, the
 * one the desktop browser's own MCP server calls): it outlines a document as
 * roles, names and element references, finds elements, and sets form
 * values. The renderer runs it in a script realm of its own and keeps the
 * references it handed out, so a later call can act on one, out of the
 * page's reach.
 */
final class PageScript {

    private PageScript() {
    }

    /**
     * Call a function of the helper, such as {@code readPage("all",40000)},
     * and return what it returned.
     *
     * @throws AgentException when the helper threw, with its message
     */
    static String call(Page page, String invocation) {
        int open = invocation.indexOf('(');
        if (open <= 0 || !invocation.endsWith(")")) {
            throw new IllegalArgumentException("malformed helper call " + invocation);
        }
        String request = "{\"fn\":" + Json.quote(invocation.substring(0, open))
            + ",\"args\":[" + invocation.substring(open + 1, invocation.length() - 1) + "]}";
        String reply = page.remoteCall(request);
        if (reply == null) {
            throw new AgentException("the page returned nothing; it may have no scripting");
        }
        if (reply.startsWith("error: ")) {
            throw new AgentException(reply.substring("error: ".length()));
        }
        return reply;
    }
}

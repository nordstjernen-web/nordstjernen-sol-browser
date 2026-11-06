/* Nordstjernen — the outcome of a navigation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

/**
 * What a navigation ended in.
 *
 * @param ok            the engine opened a document; false when it could not
 * @param url           the document's URL after redirects
 * @param title         the document's title, empty when it has none
 * @param security      how the document travelled
 * @param serverAddress the address it was fetched from, or null
 * @param redirect      a navigation the page itself asked for while it
 *                      settled (a meta refresh, a script setting
 *                      {@code location}), or null; follow it to reach the
 *                      page the site meant to show
 */
public record Response(boolean ok, String url, String title, Security security,
                       String serverAddress, String redirect) {
}

/* Nordstjernen — one step of a page's event loop without painting.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

/**
 * The result of {@link Page#tick}: timers, transitions and network work
 * progressed, and this is what came of it.
 *
 * @param changed    the page changed and should be rendered again
 * @param animating  the page is still animating or loading
 * @param wakeMillis how long the host may sleep before the page needs
 *                   another tick, or 0 when it did not say
 * @param pageWidth  laid-out page width in CSS pixels
 * @param pageHeight laid-out page height in CSS pixels
 * @param title      the document's current title
 * @param url        the document's current URL, which a script may have
 *                   changed through the History API
 * @param events     what the page asked of its host
 */
public record Tick(boolean changed, boolean animating, int wakeMillis, int pageWidth,
                   int pageHeight, String title, String url, PageEvents events) {
}

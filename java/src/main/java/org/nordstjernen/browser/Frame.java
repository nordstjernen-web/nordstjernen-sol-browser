/* Nordstjernen — one rendered view of a page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

/**
 * The result of {@link Page#render}: pixels, when they changed, and what the
 * page asked of its host while painting.
 *
 * @param pixels           premultiplied BGRA rows (the bytes of a
 *                         little-endian ARGB word per pixel), or null when
 *                         {@code unchanged}: keep showing the previous frame
 * @param width            pixels per row
 * @param height           rows
 * @param stride           bytes per row, at least {@code width * 4}
 * @param unchanged        nothing was repainted since the last identical request
 * @param animating        the page is still animating or loading; keep rendering
 * @param caretBlinking    a caret is blinking in a focused field
 * @param pageWidth        laid-out page width in CSS pixels
 * @param pageHeight       laid-out page height in CSS pixels
 * @param requestedScrollX horizontal scroll position the page asked for, or -1
 * @param requestedScrollY vertical scroll position the page asked for
 *                         (an anchor, {@code scrollTo}), or -1
 * @param events           what the page asked of its host
 */
public record Frame(byte[] pixels, int width, int height, int stride, boolean unchanged,
                    boolean animating, boolean caretBlinking, int pageWidth, int pageHeight,
                    int requestedScrollX, int requestedScrollY, PageEvents events) {
}

/* Nordstjernen — transport security of a loaded page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

/** How the current page travelled from its server. */
public enum Security {
    /** Not applicable: {@code about:}, {@code data:} and local pages. */
    NONE,
    /** HTTPS with a validated certificate chain. */
    SECURE,
    /** HTTPS whose certificate chain did not validate. */
    UNTRUSTED,
    /** Plain HTTP: readable and modifiable in transit. */
    INSECURE;

    static Security of(int code) {
        return switch (code) {
            case 1 -> SECURE;
            case 2 -> UNTRUSTED;
            case 3 -> INSECURE;
            default -> NONE;
        };
    }
}

/* Nordstjernen — a page waiting for permission.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

/**
 * A capability a page asked for and is suspended on until
 * {@link Page#resolveCapability} answers.
 *
 * @param kind   the capability: {@code camera}, {@code popups},
 *               {@code fullscreen}, {@code pointer-lock}, {@code downloads}
 *               or {@code webgl}
 * @param origin the origin asking
 */
public record CapabilityRequest(String kind, String origin) {
}

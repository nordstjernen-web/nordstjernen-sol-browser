/* Nordstjernen — a renderer request that passed its deadline.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen;

/**
 * Thrown when the renderer process did not answer a request within its
 * deadline. The renderer has been stopped by then; start a fresh one (for a
 * {@link RemoteBrowser}, {@link RemoteBrowser#restart()}) before going on.
 */
public class RendererTimeoutException extends NordstjernenException {

    public RendererTimeoutException(String message, Throwable cause) {
        super(message, cause);
    }
}

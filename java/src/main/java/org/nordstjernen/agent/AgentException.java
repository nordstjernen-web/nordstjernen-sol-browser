/* Nordstjernen — an agent action that could not be carried out.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.NordstjernenException;

/**
 * An action the page or the caller made impossible: a stale element
 * reference, an element with no box, an unknown key. The message is written
 * for the model that asked, and says what to do instead where it can.
 */
public class AgentException extends NordstjernenException {

    private static final long serialVersionUID = 1L;

    public AgentException(String message) {
        super(message);
    }
}

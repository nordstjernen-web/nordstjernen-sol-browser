/* Nordstjernen — which profile a browser context uses.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import java.nio.file.Path;

/**
 * Where a {@link BrowserContext} keeps its cookies, cache, storage and
 * history.
 *
 * @param profile   which kind of profile
 * @param directory the profile directory of a {@link Profile#PERSISTENT}
 *                  context, otherwise null
 */
public record ContextOptions(Profile profile, Path directory) {

    /** The kinds of profile a context can have. */
    public enum Profile {
        /** A directory of its own, created for the context and deleted with it. */
        EPHEMERAL,
        /** A directory the caller names, kept between runs. */
        PERSISTENT,
        /** This user's own Nordstjernen profile, shared with the desktop browser. */
        USER,
        /**
         * Nothing on disk at all: each page keeps its cookies and storage in
         * memory, so pages of the context do not share them.
         */
        IN_MEMORY
    }

    public ContextOptions {
        if (profile == null) {
            throw new IllegalArgumentException("profile must not be null");
        }
        if ((profile == Profile.PERSISTENT) != (directory != null)) {
            throw new IllegalArgumentException("a directory goes with a persistent profile only");
        }
    }

    public static ContextOptions ephemeral() {
        return new ContextOptions(Profile.EPHEMERAL, null);
    }

    public static ContextOptions persistent(Path directory) {
        return new ContextOptions(Profile.PERSISTENT, directory);
    }

    public static ContextOptions userProfile() {
        return new ContextOptions(Profile.USER, null);
    }

    public static ContextOptions inMemory() {
        return new ContextOptions(Profile.IN_MEMORY, null);
    }
}

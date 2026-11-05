/* Nordstjernen — where the renderer keeps its per-user files.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The directories the engine resolves through GLib, as a shell that does not
 * link GLib has to work them out for itself.
 */
final class EngineDirs {

    private EngineDirs() {
    }

    /**
     * The directories {@code g_get_user_cache_dir()} may name for this user:
     * {@code XDG_CACHE_HOME} when set (GLib honours it everywhere), then the
     * platform default. On Windows that is the Internet cache folder under
     * the local application data directory, not the directory itself.
     */
    static List<Path> userCacheDirs() {
        List<Path> dirs = new ArrayList<>();
        String xdg = System.getenv("XDG_CACHE_HOME");
        if (xdg != null && !xdg.isEmpty()) {
            dirs.add(Path.of(xdg));
        }
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String home = System.getProperty("user.home", "");
        if (os.startsWith("windows")) {
            String local = System.getenv("LOCALAPPDATA");
            Path base = local != null && !local.isEmpty()
                ? Path.of(local) : Path.of(home, "AppData", "Local");
            dirs.add(base.resolve("Microsoft").resolve("Windows").resolve("INetCache"));
            dirs.add(base);
        } else if (os.contains("mac") || os.contains("darwin")) {
            dirs.add(Path.of(home, "Library", "Caches"));
            dirs.add(Path.of(home, ".cache"));
        } else {
            dirs.add(Path.of(home, ".cache"));
        }
        return dirs;
    }

    /** {@code <cache>/nordstjernen/<name>} for every candidate cache directory. */
    static List<Path> cacheSubdirs(String name) {
        List<Path> dirs = new ArrayList<>();
        for (Path cache : userCacheDirs()) {
            dirs.add(cache.resolve("nordstjernen").resolve(name));
        }
        return dirs;
    }
}

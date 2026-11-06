/* Nordstjernen — a profile and the pages that share it.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import org.nordstjernen.NordstjernenException;
import org.nordstjernen.internal.EngineDirs;

import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A browser profile: the cookies, cache, storage and history its pages share.
 * Pages of one context see each other's cookies; pages of different contexts
 * do not. Each page still runs in a renderer process of its own.
 */
public final class BrowserContext implements AutoCloseable {

    private final Engine engine;
    private final ContextOptions options;
    private final Path directory;
    private final List<Page> pages = new ArrayList<>();
    private boolean closed;

    BrowserContext(Engine engine, ContextOptions options) {
        this.engine = engine;
        this.options = options;
        try {
            this.directory = switch (options.profile()) {
                case EPHEMERAL -> Files.createTempDirectory("nordstjernen-profile-");
                case PERSISTENT -> Files.createDirectories(options.directory().toAbsolutePath());
                case USER, IN_MEMORY -> null;
            };
        } catch (IOException e) {
            throw new NordstjernenException("cannot create the profile directory: "
                + e.getMessage(), e);
        }
    }

    /** A page with an 1280 by 800 CSS-pixel viewport. */
    public Page newPage() {
        return newPage(1280, 800);
    }

    /**
     * A page with the given viewport in CSS pixels. Its renderer process
     * starts with the first navigation.
     */
    public synchronized Page newPage(int viewportWidth, int viewportHeight) {
        if (closed) {
            throw new IllegalStateException("the context is closed");
        }
        Page page = new Page(this, viewportWidth, viewportHeight);
        pages.add(page);
        return page;
    }

    public Engine engine() {
        return engine;
    }

    public ContextOptions options() {
        return options;
    }

    /**
     * The directory this context's profile lives in, or null for the user's
     * own profile and for an in-memory context.
     */
    public Path profileDirectory() {
        return directory;
    }

    /** The pages of this context that are still open. */
    public synchronized List<Page> pages() {
        return List.copyOf(pages);
    }

    public synchronized boolean isClosed() {
        return closed;
    }

    /** Close the context's pages; an ephemeral profile is deleted. */
    @Override
    public void close() {
        List<Page> open;
        synchronized (this) {
            if (closed) {
                return;
            }
            closed = true;
            open = new ArrayList<>(pages);
            pages.clear();
        }
        for (Page page : open) {
            page.close();
        }
        engine.forget(this);
        if (options.profile() == ContextOptions.Profile.EPHEMERAL) {
            deleteTree(directory);
        }
    }

    synchronized void forget(Page page) {
        pages.remove(page);
    }

    boolean inMemory() {
        return options.profile() == ContextOptions.Profile.IN_MEMORY;
    }

    /**
     * The variables that point a renderer at this context's profile. The
     * engine asks GLib for its directories, and GLib takes them from these on
     * every platform.
     */
    Map<String, String> environment() {
        Map<String, String> env = new LinkedHashMap<>();
        if (directory != null) {
            env.put("XDG_CONFIG_HOME", directory.resolve("config").toString());
            env.put("XDG_DATA_HOME", directory.resolve("data").toString());
            env.put("XDG_CACHE_HOME", directory.resolve("cache").toString());
        }
        return env;
    }

    /**
     * The directories a renderer of this context may stage files in, named
     * {@code name} under its cache directory: {@code downloads} for
     * downloads, {@code msaudio} for media streams.
     */
    List<Path> stagingDirectories(String name) {
        if (directory != null) {
            return List.of(directory.resolve("cache").resolve("nordstjernen").resolve(name));
        }
        return EngineDirs.cacheSubdirs(name);
    }

    private static void deleteTree(Path root) {
        if (root == null || !Files.exists(root)) {
            return;
        }
        try {
            Files.walkFileTree(root, new SimpleFileVisitor<>() {
                @Override
                public FileVisitResult visitFile(Path file, BasicFileAttributes attrs)
                        throws IOException {
                    Files.deleteIfExists(file);
                    return FileVisitResult.CONTINUE;
                }

                @Override
                public FileVisitResult postVisitDirectory(Path dir, IOException failure)
                        throws IOException {
                    Files.deleteIfExists(dir);
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException e) {
            root.toFile().deleteOnExit();
        }
    }
}

/* Nordstjernen — the entry point of the browser API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import org.nordstjernen.NordstjernenException;
import org.nordstjernen.internal.RendererProcess;

import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * The Nordstjernen engine as a Java program sees it: the place browser
 * contexts and their pages come from.
 *
 * <p>No engine code is loaded into the JVM. Every {@link Page} runs in a
 * {@code nordstjernen-renderer} process of its own, sandboxed by the renderer
 * itself, so a page that crashes or hangs costs that page and nothing else.
 *
 * <pre>{@code
 * try (Engine engine = Engine.launch();
 *      BrowserContext context = engine.newContext();
 *      Page page = context.newPage()) {
 *     Response response = page.navigate("https://example.com");
 *     String title = page.title();
 *     byte[] png = page.screenshot();
 * }
 * }</pre>
 *
 * <p>All classes of this package may be used from any thread. Requests to one
 * page are carried out one at a time, in the order they arrive.
 */
public final class Engine implements AutoCloseable {

    /**
     * How an engine is set up.
     *
     * @param renderer    the {@code nordstjernen-renderer} executable, or null
     *                    to find it: the {@code nordstjernen.renderer} system
     *                    property, {@code NORDSTJERNEN_RENDERER}, beside the
     *                    jar, then {@code PATH}
     * @param timeout     how long a request to a page may take before its
     *                    renderer is stopped and the call fails
     * @param frameWidth  the widest frame a page can render, in device pixels
     * @param frameHeight the tallest frame a page can render, in device pixels
     */
    public record Options(Path renderer, Duration timeout, int frameWidth, int frameHeight) {

        public Options {
            if (timeout == null || timeout.isNegative() || timeout.isZero()) {
                throw new IllegalArgumentException("timeout must be positive");
            }
            if (frameWidth < 1 || frameHeight < 1 || frameWidth > 32768 || frameHeight > 32768) {
                throw new IllegalArgumentException("frame size must be 1 to 32768 a side");
            }
        }

        /** Find the renderer, two minutes a request, frames up to 2560 by 4096. */
        public static Options defaults() {
            return new Options(null, Duration.ofMillis(RendererProcess.DEFAULT_TIMEOUT_MS),
                               2560, 4096);
        }

        public Options withRenderer(Path renderer) {
            return new Options(renderer, timeout, frameWidth, frameHeight);
        }

        public Options withTimeout(Duration timeout) {
            return new Options(renderer, timeout, frameWidth, frameHeight);
        }

        /**
         * The largest frame {@link Page#render} can return. A window that shows
         * pages needs its own size times the screen's scale factor here; the
         * renderer reserves this much memory per page.
         */
        public Options withFrameSize(int width, int height) {
            return new Options(renderer, timeout, width, height);
        }
    }

    private final Options options;
    private final String renderer;
    private final List<BrowserContext> contexts = new ArrayList<>();
    private BrowserContext userContext;
    private boolean closed;

    private Engine(Options options) {
        this.options = options;
        this.renderer = options.renderer() != null
            ? options.renderer().toAbsolutePath().toString()
            : RendererProcess.locateRenderer();
    }

    /**
     * Set the engine up with {@link Options#defaults()}.
     *
     * @throws NordstjernenException when no renderer executable is found
     */
    public static Engine launch() {
        return new Engine(Options.defaults());
    }

    /** Set the engine up with {@code options}. */
    public static Engine launch(Options options) {
        return new Engine(options);
    }

    /**
     * A context with a profile of its own that is deleted when the context
     * closes: its pages share cookies and storage with each other and with
     * nothing else.
     */
    public BrowserContext newContext() {
        return newContext(ContextOptions.ephemeral());
    }

    public synchronized BrowserContext newContext(ContextOptions contextOptions) {
        requireOpen();
        BrowserContext context = new BrowserContext(this, contextOptions);
        contexts.add(context);
        return context;
    }

    /**
     * The context that uses this user's own Nordstjernen profile, the one the
     * desktop browser keeps its cookies, cache and history in.
     */
    public synchronized BrowserContext userContext() {
        requireOpen();
        if (userContext == null || userContext.isClosed()) {
            userContext = new BrowserContext(this, ContextOptions.userProfile());
            contexts.add(userContext);
        }
        return userContext;
    }

    /** The renderer executable pages are run with. */
    public Path renderer() {
        return Path.of(renderer);
    }

    public Options options() {
        return options;
    }

    public synchronized boolean isClosed() {
        return closed;
    }

    /** Close every context and page that came from this engine. */
    @Override
    public void close() {
        List<BrowserContext> open;
        synchronized (this) {
            if (closed) {
                return;
            }
            closed = true;
            open = new ArrayList<>(contexts);
            contexts.clear();
        }
        for (BrowserContext context : open) {
            context.close();
        }
    }

    synchronized void forget(BrowserContext context) {
        contexts.remove(context);
    }

    private void requireOpen() {
        if (closed) {
            throw new IllegalStateException("the engine is closed");
        }
    }
}

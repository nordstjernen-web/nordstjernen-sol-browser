/* Nordstjernen — HTML in, PDF or PNG out.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

/**
 * Turns an HTML string into a paged PDF or a PNG image:
 *
 * <pre>{@code
 * try (HtmlRenderer renderer = HtmlRenderer.create()) {
 *     byte[] pdf = renderer.pdf("<h1>Invoice 42</h1>");
 *     byte[] png = renderer.png("<h1>Invoice 42</h1>", 800);
 * }
 * }</pre>
 *
 * <p>A renderer made by {@link #create()} loads nothing from outside the
 * document: no network and no local files, so the HTML has to carry its
 * styles, fonts and images inline, and HTML from an untrusted source cannot
 * make the machine fetch anything. {@link #create(ResourcePolicy)} lifts
 * that as far as the policy says. Nothing is kept on disk.
 *
 * <p>One renderer process serves every call, one call at a time; make
 * several renderers to render in parallel. Documents given the same base URL
 * share that origin's cookies and storage for as long as the renderer lives.
 */
public final class HtmlRenderer implements AutoCloseable {

    private static final int VIEWPORT_WIDTH = 1024;
    private static final int VIEWPORT_HEIGHT = 768;

    private final Engine ownEngine;
    private final BrowserContext context;
    private final Page page;

    private HtmlRenderer(Engine ownEngine, BrowserContext context) {
        this.ownEngine = ownEngine;
        this.context = context;
        this.page = context.newPage(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    }

    /** A renderer that loads nothing from outside the document. */
    public static HtmlRenderer create() {
        return create(ResourcePolicy.none());
    }

    /** A renderer whose documents may load what {@code policy} allows. */
    public static HtmlRenderer create(ResourcePolicy policy) {
        Engine engine = Engine.launch();
        try {
            return new HtmlRenderer(engine, engine.newContext(
                ContextOptions.inMemory().withResourcePolicy(policy)));
        } catch (RuntimeException e) {
            engine.close();
            throw e;
        }
    }

    /**
     * A renderer on an engine and with options the caller chose. Closing the
     * renderer closes its context and leaves the engine running.
     */
    public static HtmlRenderer create(Engine engine, ContextOptions options) {
        return new HtmlRenderer(null, engine.newContext(options));
    }

    /** {@code html} as an A4 PDF. */
    public byte[] pdf(String html) {
        return pdf(html, null, PdfOptions.a4());
    }

    /** {@code html} as a PDF laid out as {@code options} say. */
    public byte[] pdf(String html, PdfOptions options) {
        return pdf(html, null, options);
    }

    /**
     * {@code html} as a PDF, with relative URLs resolved against
     * {@code baseUrl}; see {@link Page#setContent(String, String, Page.Navigation)}.
     */
    public synchronized byte[] pdf(String html, String baseUrl, PdfOptions options) {
        layOutAt(VIEWPORT_WIDTH);
        page.setContent(html, baseUrl);
        return page.pdf(options);
    }

    /** The whole of {@code html}, laid out {@code width} CSS pixels wide, as a PNG. */
    public byte[] png(String html, int width) {
        return png(html, null, width, 1.0);
    }

    /**
     * The whole of {@code html} as a PNG: laid out {@code width} CSS pixels
     * wide, drawn at {@code scale} device pixels per CSS pixel, with relative
     * URLs resolved against {@code baseUrl}.
     */
    public synchronized byte[] png(String html, String baseUrl, int width, double scale) {
        layOutAt(width);
        page.setContent(html, baseUrl);
        return page.screenshot(Page.Screenshot.fullPage().withScale(scale));
    }

    private void layOutAt(int width) {
        if (page.viewportWidth() != width) {
            page.setViewport(width, VIEWPORT_HEIGHT);
        }
    }

    /** The page the documents are shown in, for anything this class has no method for. */
    public Page page() {
        return page;
    }

    @Override
    public synchronized void close() {
        context.close();
        if (ownEngine != null) {
            ownEngine.close();
        }
    }
}

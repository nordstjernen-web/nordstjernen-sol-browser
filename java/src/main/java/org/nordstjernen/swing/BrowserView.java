/* Nordstjernen — a Swing component that shows a page and lets the user work it.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.swing;

import org.nordstjernen.RendererTimeoutException;
import org.nordstjernen.browser.CapabilityRequest;
import org.nordstjernen.browser.Frame;
import org.nordstjernen.browser.Page;
import org.nordstjernen.browser.PageEvents;
import org.nordstjernen.browser.Response;
import org.nordstjernen.browser.Security;
import org.nordstjernen.browser.Tick;

import javax.swing.JComponent;
import javax.swing.JPanel;
import javax.swing.JRootPane;
import javax.swing.JScrollBar;
import javax.swing.KeyStroke;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import javax.swing.TransferHandler;
import javax.swing.text.JTextComponent;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Component;
import java.awt.Cursor;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.GraphicsConfiguration;
import java.awt.KeyboardFocusManager;
import java.awt.Point;
import java.awt.RenderingHints;
import java.awt.Toolkit;
import java.awt.datatransfer.DataFlavor;
import java.awt.datatransfer.StringSelection;
import java.awt.datatransfer.UnsupportedFlavorException;
import java.awt.event.ComponentAdapter;
import java.awt.event.ComponentEvent;
import java.awt.event.InputEvent;
import java.awt.event.KeyAdapter;
import java.awt.event.KeyEvent;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.MouseMotionAdapter;
import java.awt.event.MouseWheelEvent;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/**
 * A web page as a Swing component: it paints the page, scrolls it, and
 * passes the mouse and the keyboard to it. Add it to any container:
 *
 * <pre>{@code
 * Engine engine = Engine.launch();
 * BrowserView view = new BrowserView(engine.newContext().newPage());
 * frame.add(view);
 * view.navigate("https://example.com");
 * }</pre>
 *
 * <p>The view does a window's part for the page: it keeps the scroll
 * position, the zoom and the back and forward list, follows the navigations
 * a click, a form or a script asks for, keeps the page's timers running
 * while nothing moves, restarts a renderer that died, and puts copied text
 * on the system clipboard. What only the application around it can decide
 * comes to a {@link Listener}: the title and address to show, where a
 * download goes, whether a site may use the camera, what a right click
 * offers, what to do with a window the page opens.
 *
 * <p>The page lives in a renderer process of its own, so nothing a page
 * does can block or crash the application. Every method here is for the
 * event dispatch thread, and listeners are called on it. The view closes
 * its page when it is {@linkplain #close() closed}; the context and the
 * engine stay the caller's.
 */
public final class BrowserView extends JPanel implements AutoCloseable {

    private static final long serialVersionUID = 1L;

    /**
     * What a view tells the application around it. Every method has an
     * empty default, so a listener overrides only what it shows or decides.
     */
    public interface Listener {

        /** A navigation is about to start loading {@code url}. */
        default void navigationStarted(String url) {
        }

        /** A navigation ended; {@code ok} is false when no document opened. */
        default void navigationFinished(String url, boolean ok) {
        }

        /** The document's address changed: a new document, or its script rewrote the URL. */
        default void urlChanged(String url) {
        }

        default void titleChanged(String title) {
        }

        /** Whether going back or forward is possible changed. */
        default void historyChanged() {
        }

        /** How the document was fetched, and from which address, or null. */
        default void securityChanged(Security security, String serverAddress) {
        }

        /** The document's icon, or null when it has none. */
        default void faviconChanged(BufferedImage icon) {
        }

        /**
         * A line for a status bar: the link under the pointer, a zoom level,
         * "Copied to clipboard"; empty to clear it.
         */
        default void statusChanged(String text) {
        }

        /** A search finished: match {@code current} of {@code total}, both 0 for none. */
        default void findResult(String query, int current, int total) {
        }

        /** The user asked for a link in a window of its own, as with a middle click. */
        default void newWindowRequested(String url) {
        }

        /**
         * The page queued operations on its windows: opening one, posting a
         * message to one, closing one. Each map has an {@code op} and the
         * fields of {@link Page#popupOperations()}; a view that received an
         * operation meant for it takes it through
         * {@link BrowserView#deliverPopupOperation}.
         */
        default void popupOperations(List<Map<String, String>> operations) {
        }

        /**
         * The page offers a file. Fetch it with {@link Page#startDownload}
         * and the calls after it, or ignore it to decline.
         */
        default void downloadRequested(String url, String suggestedName) {
        }

        /**
         * The page waits for permission to use something. Answer each
         * request with {@link BrowserView#resolveCapability}.
         */
        default void capabilitiesRequested(List<CapabilityRequest> requests) {
        }

        /**
         * The user asked for a context menu the page did not supply.
         *
         * @param x        where, in the view's own coordinates
         * @param y        where, in the view's own coordinates
         * @param link     the link at that point, or null
         * @param editable whether the point is in a text field
         */
        default void contextMenuRequested(int x, int y, String link, boolean editable) {
        }

        /** The user clicked audio or video the engine does not play itself. */
        default void mediaRequested(Page.Media media) {
        }

        /** The page asked for {@code fullscreen-enter} or {@code fullscreen-exit}. */
        default void windowActionRequested(String action) {
        }

        /** Commands for the audio helper process, for {@code AudioHelper.send}. */
        default void audioCommands(List<String> commands, String pageUrl) {
        }

        /**
         * A key went down over the page. Return true to keep it from the
         * page, as for Escape closing the application's own find bar.
         */
        default boolean keyIntercepted(KeyEvent event) {
            return false;
        }
    }

    private static final int SETTLE_MS = 900;
    private static final int LINE_SCROLL = 60;
    private static final double ZOOM_MIN = 0.25;
    private static final double ZOOM_MAX = 5.0;
    private static final int[] ZOOM_LADDER_PERCENT = {
        25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400, 500
    };
    private static final int MAX_JS_REDIRECTS = 20;
    private static final int MAX_RESTARTS = 3;
    private static final int IDLE_REFRESH_MS = 120;
    private static final int ANIMATION_REFRESH_MS = 33;
    private static final int IDLE_TICK_MS = 400;
    private static final int MIN_WAKE_MS = 15;
    private static final long TITLE_CHECK_NANOS = 1_000_000_000L;
    private static final String[] POPUP_FIELDS = {
        "token", "to", "data", "origin", "target", "path", "ports", "id"
    };

    private final transient Page page;
    private final transient ExecutorService io = pageThread();
    private final transient List<Listener> listeners = new CopyOnWriteArrayList<>();
    private final int frameMaxWidth;
    private final int frameMaxHeight;

    private final Canvas canvas = new Canvas();
    private final JScrollBar vScroll = new JScrollBar(JScrollBar.VERTICAL);
    private final JScrollBar hScroll = new JScrollBar(JScrollBar.HORIZONTAL);

    private final List<String> history = new ArrayList<>();
    private int historyIndex = -1;
    private String shownTitle = "";
    private String shownUrl = "";

    private int scrollX;
    private int scrollY;
    private double scale = 1.0;
    private boolean loading;
    private int jsRedirects;
    private int restarts;
    private boolean syncingScrollbar;
    private Timer refreshTimer;
    private Timer idleTimer;
    private boolean renderBusy;
    private boolean idleBusy;
    private int stableFrames;
    private long lastTickNanos;

    private boolean hoverBusy;
    private boolean hoverPending;
    private int hoverPendingX;
    private int hoverPendingY;
    private String hoverLink;

    private boolean dragAnchored;
    private int pressDocX;
    private int pressDocY;
    private boolean hasSelection;
    private boolean suppressTextInsert;
    private boolean draggingScrollbar;
    private boolean caretActive;
    private final Map<Integer, String[]> heldKeys = new HashMap<>();
    private double wheelPendingX;
    private double wheelPendingY;

    private int viewportW = -1;
    private int viewportH = -1;
    private double viewportDpr = -1;
    private transient Runnable queuedNavigation;
    private boolean popupPollBusy;
    private boolean closed;

    /**
     * A view of {@code page}. The view takes the page over: it sets its
     * viewport, navigates it and closes it.
     */
    public BrowserView(Page page) {
        super(new BorderLayout());
        if (page == null) {
            throw new IllegalArgumentException("page must not be null");
        }
        this.page = page;
        this.frameMaxWidth = page.context().engine().options().frameWidth();
        this.frameMaxHeight = page.context().engine().options().frameHeight();
        add(canvas, BorderLayout.CENTER);
        add(vScroll, BorderLayout.EAST);
        add(hScroll, BorderLayout.SOUTH);
        installScrollbars();
        installPointer();
        installKeyboard();
        installFileDrop();
        canvas.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                syncViewport();
            }
        });
    }

    public void addListener(Listener listener) {
        listeners.add(listener);
    }

    public void removeListener(Listener listener) {
        listeners.remove(listener);
    }

    /** The page behind the view. Slow calls on it belong on {@link #pageExecutor()}. */
    public Page page() {
        return page;
    }

    /**
     * The one thread the view talks to its page on. Work an application
     * hands it runs in order with the view's own requests and off the event
     * dispatch thread; call {@link #refresh()} afterwards if it changed
     * what the page shows. Work queued after {@link #close()} runs once
     * the page has closed.
     */
    public Executor pageExecutor() {
        return io::submit;
    }

    /** Load {@code url} and add it to the back and forward list. */
    public void navigate(String url) {
        navigate(url, true, false, false);
    }

    /** Load the current address again. */
    public void reload() {
        if (url() != null) {
            navigate(url(), false, false, false);
        }
    }

    public void back() {
        if (!loading && canGoBack()) {
            historyIndex--;
            navigate(history.get(historyIndex), false, false, true);
        }
    }

    public void forward() {
        if (!loading && canGoForward()) {
            historyIndex++;
            navigate(history.get(historyIndex), false, false, true);
        }
    }

    public boolean canGoBack() {
        return historyIndex > 0;
    }

    public boolean canGoForward() {
        return historyIndex < history.size() - 1;
    }

    /** The address of the document showing, or null before the first one. */
    public String url() {
        return historyIndex >= 0 ? history.get(historyIndex) : null;
    }

    /** The title of the document showing. */
    public String title() {
        return shownTitle;
    }

    public boolean isLoading() {
        return loading;
    }

    /** The zoom factor: 1 is one CSS pixel per component pixel. */
    public double zoom() {
        return scale;
    }

    /** Zoom the page, between a quarter and five times its size, and lay it out again. */
    public void setZoom(double factor) {
        int permille = (int) Math.round(factor * 1000.0);
        permille = Math.max((int) (ZOOM_MIN * 1000), Math.min((int) (ZOOM_MAX * 1000), permille));
        double clamped = permille / 1000.0;
        if (clamped == scale) {
            return;
        }
        scale = clamped;
        status("Zoom " + (permille / 10) + "%");
        syncViewport();
    }

    /** Step up the zoom ladder the desktop browser uses. */
    public void zoomIn() {
        zoomStep(1);
    }

    public void zoomOut() {
        zoomStep(-1);
    }

    /**
     * Search the page for {@code query}, scroll to the match and highlight
     * the matches; the outcome comes to {@link Listener#findResult}. An
     * empty query clears the highlight.
     */
    public void find(String query, Page.FindDirection direction) {
        final String wanted = query == null ? "" : query;
        final int fromY = scrollY;
        io.submit(() -> {
            Page.Found found;
            try {
                found = page.find(wanted, false, direction, fromY);
            } catch (RuntimeException ex) {
                return;
            }
            SwingUtilities.invokeLater(() -> {
                if (found.total() > 0) {
                    setScrollY(Math.max(0, found.scrollY() - 40));
                }
                fire(l -> l.findResult(wanted, found.current(), found.total()));
                refresh();
            });
        });
    }

    /** Copy the selection to the system clipboard. */
    public void copy() {
        runEditCommand("copy", null, null, 0, 0);
    }

    public void cut() {
        runEditCommand("cut", null, null, 0, 0);
    }

    /** Paste the system clipboard's text at the caret. */
    public void paste() {
        runEditCommand("paste", null, null, 0, 0);
    }

    public void selectAll() {
        runEditCommand("selectAll", null, null, 0, 0);
    }

    /** Whether the user has selected something in the page. */
    public boolean hasSelection() {
        return hasSelection;
    }

    /** Give the page the keyboard focus. */
    public void focusPage() {
        canvas.requestFocusInWindow();
    }

    /** Paint the page again, and keep painting until it stops changing. */
    public void refresh() {
        if (closed) {
            return;
        }
        stableFrames = 0;
        if (idleTimer != null) {
            idleTimer.stop();
        }
        if (refreshTimer == null) {
            refreshTimer = new Timer(IDLE_REFRESH_MS, e -> renderOnce());
        }
        if (!refreshTimer.isRunning()) {
            refreshTimer.start();
        }
    }

    /** Answer a request from {@link Listener#capabilitiesRequested}. */
    public void resolveCapability(CapabilityRequest request, boolean allow) {
        io.submit(() -> {
            try {
                page.resolveCapability(request, allow);
            } catch (RuntimeException ex) {
                return;
            }
            SwingUtilities.invokeLater(this::refresh);
        });
    }

    /**
     * Hand this view's page an operation another page queued for it:
     * {@code message}, {@code call}, {@code port} or {@code closed}, with
     * the fields of the operation as it was reported.
     */
    public void deliverPopupOperation(String kind, Map<String, String> operation) {
        if (closed) {
            return;
        }
        Map<String, String> fields = new LinkedHashMap<>();
        for (String field : POPUP_FIELDS) {
            if (operation.get(field) != null) {
                fields.put(field, operation.get(field));
            }
        }
        io.submit(() -> {
            try {
                page.deliverPopupOperation(kind, fields);
            } catch (RuntimeException ex) {
                return;
            }
            SwingUtilities.invokeLater(this::refresh);
        });
    }

    /** Ask the page to leave fullscreen, as when the user pressed Escape. */
    public void exitPageFullscreen() {
        io.submit(() -> {
            try {
                page.evaluate("document.exitFullscreen()");
            } catch (RuntimeException ex) {
                return;
            }
            SwingUtilities.invokeLater(this::refresh);
        });
    }

    public boolean isClosed() {
        return closed;
    }

    /** Stop painting and close the page. The view shows its last frame from then on. */
    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        if (refreshTimer != null) {
            refreshTimer.stop();
        }
        if (idleTimer != null) {
            idleTimer.stop();
        }
        io.submit(page::close);
    }

    /**
     * One thread at a time, so requests keep their order, that goes away
     * when the view has been idle or closed for a while.
     */
    private static ExecutorService pageThread() {
        ThreadPoolExecutor pool = new ThreadPoolExecutor(1, 1, 20L, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(), work -> {
                Thread thread = new Thread(work, "ns-view");
                thread.setDaemon(true);
                return thread;
            });
        pool.allowCoreThreadTimeOut(true);
        return pool;
    }

    private void fire(Consumer<Listener> event) {
        for (Listener listener : listeners) {
            event.accept(listener);
        }
    }

    private void status(String text) {
        final String shown = text == null ? "" : text;
        fire(l -> l.statusChanged(shown));
    }

    private void installScrollbars() {
        vScroll.setUnitIncrement(LINE_SCROLL);
        vScroll.addAdjustmentListener(e -> {
            if (!syncingScrollbar) {
                scrollY = e.getValue();
                refresh();
            }
        });
        hScroll.setUnitIncrement(LINE_SCROLL);
        hScroll.setVisible(false);
        hScroll.addAdjustmentListener(e -> {
            if (!syncingScrollbar) {
                scrollX = e.getValue();
                refresh();
            }
        });
    }

    private void installPointer() {
        canvas.setFocusable(true);
        canvas.setFocusTraversalKeysEnabled(false);
        canvas.addMouseWheelListener(this::onWheel);
        canvas.addMouseListener(new MouseAdapter() {
            @Override
            public void mousePressed(MouseEvent e) {
                canvas.requestFocusInWindow();
                if (e.getButton() == 4) {
                    back();
                } else if (e.getButton() == 5) {
                    forward();
                } else if (e.getButton() == MouseEvent.BUTTON2) {
                    openLinkInNewWindow(e.getX(), e.getY());
                } else if (e.isPopupTrigger() || e.getButton() == MouseEvent.BUTTON3) {
                    askContextMenu(e.getX(), e.getY());
                } else if (e.getButton() == MouseEvent.BUTTON1) {
                    onPress(e);
                }
            }

            @Override
            public void mouseReleased(MouseEvent e) {
                if (e.isPopupTrigger()) {
                    askContextMenu(e.getX(), e.getY());
                } else if (e.getButton() == MouseEvent.BUTTON1 && draggingScrollbar) {
                    draggingScrollbar = false;
                    io.submit(page::scrollbarRelease);
                } else if (e.getButton() == MouseEvent.BUTTON1) {
                    onRelease(e.getX(), e.getY());
                }
            }
        });
        canvas.addMouseMotionListener(new MouseMotionAdapter() {
            @Override
            public void mouseMoved(MouseEvent e) {
                requestHover(docX(e.getX()), docY(e.getY()));
            }

            @Override
            public void mouseDragged(MouseEvent e) {
                if (!draggingScrollbar) {
                    onDrag(e.getX(), e.getY());
                    return;
                }
                final int dx = docX(e.getX());
                final int dy = docY(e.getY());
                io.submit(() -> {
                    page.scrollbarDrag(dx, dy);
                    SwingUtilities.invokeLater(BrowserView.this::refresh);
                });
            }
        });
    }

    private void installKeyboard() {
        canvas.addKeyListener(new KeyAdapter() {
            @Override
            public void keyPressed(KeyEvent e) {
                onKeyPressed(e);
            }

            @Override
            public void keyReleased(KeyEvent e) {
                onKeyReleased(e);
            }

            @Override
            public void keyTyped(KeyEvent e) {
                onKeyTyped(e);
            }
        });
    }

    private void installFileDrop() {
        canvas.setTransferHandler(new TransferHandler() {
            private static final long serialVersionUID = 1L;

            @Override
            public boolean canImport(TransferSupport support) {
                return support.isDrop()
                    && support.isDataFlavorSupported(DataFlavor.javaFileListFlavor);
            }

            @Override
            public boolean importData(TransferSupport support) {
                if (!canImport(support)) {
                    return false;
                }
                List<Path> files = new ArrayList<>();
                try {
                    Object data = support.getTransferable()
                        .getTransferData(DataFlavor.javaFileListFlavor);
                    for (Object o : (List<?>) data) {
                        files.add(((File) o).toPath().toAbsolutePath());
                    }
                } catch (UnsupportedFlavorException | IOException | RuntimeException ex) {
                    return false;
                }
                if (files.isEmpty()) {
                    return false;
                }
                Point p = support.getDropLocation().getDropPoint();
                final int dx = docX(p.x);
                final int dy = docY(p.y);
                io.submit(() -> {
                    page.dropFiles(dx, dy, files);
                    SwingUtilities.invokeLater(BrowserView.this::refresh);
                });
                return true;
            }
        });
    }

    private int docX(int canvasX) {
        return scrollX + (int) Math.round(canvasX / scale);
    }

    private int docY(int canvasY) {
        return scrollY + (int) Math.round(canvasY / scale);
    }

    private double visibleWidth() {
        return Math.max(1, canvas.getWidth()) / scale;
    }

    private double visibleHeight() {
        return Math.max(1, canvas.getHeight()) / scale;
    }

    private int cssViewportWidth() {
        int w = canvas.getWidth() > 0 ? canvas.getWidth() : 1000;
        return Math.max(1, (int) Math.round(w / scale));
    }

    private int cssViewportHeight() {
        int h = canvas.getHeight() > 0 ? canvas.getHeight() : 700;
        return Math.max(1, (int) Math.round(h / scale));
    }

    private double deviceScale() {
        GraphicsConfiguration gc = canvas.getGraphicsConfiguration();
        double s = gc != null ? gc.getDefaultTransform().getScaleX() : 1.0;
        return s >= 1.0 && s <= 8.0 ? s : 1.0;
    }

    private double rasterScale() {
        double raster = deviceScale();
        raster = Math.min(raster, (double) frameMaxWidth / Math.max(1, canvas.getWidth()));
        raster = Math.min(raster, (double) frameMaxHeight / Math.max(1, canvas.getHeight()));
        return Math.max(raster, 0.01);
    }

    private double pageDpr() {
        return scale * deviceScale();
    }

    private void syncViewport() {
        if (loading || closed || url() == null || canvas.getWidth() <= 1
                || canvas.getHeight() <= 1) {
            return;
        }
        final int w = cssViewportWidth();
        final int h = cssViewportHeight();
        final double dpr = pageDpr();
        if (w == viewportW && h == viewportH && dpr == viewportDpr) {
            updateScrollModel();
            refresh();
            return;
        }
        viewportW = w;
        viewportH = h;
        viewportDpr = dpr;
        io.submit(() -> {
            try {
                page.setDevicePixelRatio(dpr);
                page.setViewport(w, h);
            } catch (RuntimeException ex) {
                return;
            }
            SwingUtilities.invokeLater(() -> {
                updateScrollModel();
                refresh();
            });
        });
    }

    private int pageStep() {
        return Math.max(LINE_SCROLL, (int) visibleHeight() - LINE_SCROLL);
    }

    private int maxScrollY() {
        return Math.max(0, page.pageHeight() - (int) visibleHeight());
    }

    private int maxScrollX() {
        return Math.max(0, page.pageWidth() - (int) visibleWidth());
    }

    private void setScrollY(int y) {
        int clamped = Math.max(0, Math.min(maxScrollY(), y));
        boolean moved = clamped != scrollY;
        scrollY = clamped;
        syncScrollbars();
        if (moved) {
            refresh();
        }
    }

    private void setScrollX(int x) {
        int clamped = Math.max(0, Math.min(maxScrollX(), x));
        boolean moved = clamped != scrollX;
        scrollX = clamped;
        syncScrollbars();
        if (moved) {
            refresh();
        }
    }

    private void syncScrollbars() {
        syncingScrollbar = true;
        if (vScroll.getValue() != scrollY) {
            vScroll.setValue(scrollY);
        }
        if (hScroll.getValue() != scrollX) {
            hScroll.setValue(scrollX);
        }
        syncingScrollbar = false;
    }

    private void updateScrollModel() {
        int viewH = (int) visibleHeight();
        int maxV = Math.max(viewH, page.pageHeight());
        scrollY = Math.min(scrollY, Math.max(0, maxV - viewH));
        int viewW = (int) visibleWidth();
        int maxH = Math.max(viewW, page.pageWidth());
        scrollX = Math.min(scrollX, Math.max(0, maxH - viewW));
        syncingScrollbar = true;
        vScroll.setValues(scrollY, viewH, 0, maxV);
        vScroll.setBlockIncrement(pageStep());
        vScroll.setEnabled(maxV > viewH);
        hScroll.setValues(scrollX, viewW, 0, maxH);
        hScroll.setBlockIncrement(Math.max(LINE_SCROLL, viewW - LINE_SCROLL));
        hScroll.setEnabled(maxH > viewW);
        hScroll.setVisible(maxH > viewW);
        syncingScrollbar = false;
    }

    private void zoomStep(int direction) {
        int now = (int) Math.round(scale * 100.0);
        int last = ZOOM_LADDER_PERCENT.length - 1;
        int target = direction > 0 ? ZOOM_LADDER_PERCENT[last] : ZOOM_LADDER_PERCENT[0];
        if (direction > 0) {
            for (int percent : ZOOM_LADDER_PERCENT) {
                if (percent > now) {
                    target = percent;
                    break;
                }
            }
        } else {
            for (int i = last; i >= 0; i--) {
                if (ZOOM_LADDER_PERCENT[i] < now) {
                    target = ZOOM_LADDER_PERCENT[i];
                    break;
                }
            }
        }
        setZoom(target / 100.0);
    }

    /**
     * A page load is one blocking renderer request, so a navigation asked
     * for while another is still loading waits for it and then runs: the
     * latest one wins, as when a user clicks again.
     */
    private void navigate(String url, boolean record, boolean isRedirect, boolean fromHistory) {
        if (url == null || url.isEmpty() || closed) {
            return;
        }
        if (loading) {
            if (!fromHistory) {
                queuedNavigation = () -> navigate(url, record, isRedirect, false);
            }
            return;
        }
        if (!isRedirect) {
            jsRedirects = 0;
            restarts = 0;
        }
        loading = true;
        caretActive = false;
        heldKeys.clear();
        if (idleTimer != null) {
            idleTimer.stop();
        }
        canvas.setCursor(Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR));
        fire(l -> l.navigationStarted(url));
        status("Loading " + url + " …");
        final int vw = cssViewportWidth();
        final int vh = cssViewportHeight();
        final double dpr = pageDpr();
        viewportW = vw;
        viewportH = vh;
        viewportDpr = dpr;
        io.submit(() -> {
            Response response = load(url, vw, vh, dpr, fromHistory, !isRedirect);
            SwingUtilities.invokeLater(() -> loaded(url, response, record, isRedirect));
        });
    }

    private void loaded(String url, Response response, boolean record, boolean isRedirect) {
        loading = false;
        canvas.setCursor(Cursor.getDefaultCursor());
        Runnable queued = queuedNavigation;
        queuedNavigation = null;
        final boolean ok = response != null && response.ok();
        fire(l -> l.navigationFinished(url, ok));
        if (closed) {
            return;
        }
        if (!ok) {
            status("Failed to load " + url);
            if (queued != null) {
                queued.run();
            }
            return;
        }
        scrollX = 0;
        scrollY = 0;
        if (record) {
            while (history.size() > historyIndex + 1) {
                history.remove(history.size() - 1);
            }
            history.add(response.url());
            historyIndex = history.size() - 1;
        } else if (isRedirect && historyIndex >= 0) {
            history.set(historyIndex, response.url());
        }
        shownUrl = response.url();
        shownTitle = response.title();
        fire(l -> l.urlChanged(shownUrl));
        fire(l -> l.titleChanged(shownTitle));
        fire(Listener::historyChanged);
        fire(l -> l.securityChanged(page.security(), page.serverAddress()));
        status(shownTitle);
        updateScrollModel();
        Component focused = KeyboardFocusManager.getCurrentKeyboardFocusManager().getFocusOwner();
        if (!(focused instanceof JTextComponent)) {
            canvas.requestFocusInWindow();
        }
        syncViewport();
        refresh();
        requestFavicon();
        if (queued != null) {
            queued.run();
        } else if (response.redirect() != null && jsRedirects < MAX_JS_REDIRECTS) {
            jsRedirects++;
            navigate(response.redirect(), false, true, false);
        }
    }

    /**
     * Navigate on the page thread, starting a fresh renderer once if the
     * old one is gone. A fresh one has no back-forward cache to restore
     * from.
     */
    private Response load(String url, int vw, int vh, double dpr, boolean fromHistory,
                          boolean userActivated) {
        Page.Navigation how = Page.Navigation.defaults()
            .withSettle(Duration.ofMillis(SETTLE_MS))
            .withFromHistory(fromHistory).withUserActivated(userActivated);
        try {
            page.setDevicePixelRatio(dpr);
            fitViewport(vw, vh);
            return page.navigate(url, how);
        } catch (RendererTimeoutException ex) {
            stopRenderer();
            return null;
        } catch (RuntimeException ex) {
            if (restarts >= MAX_RESTARTS) {
                return null;
            }
            restarts++;
            try {
                page.stop();
                fitViewport(vw, vh);
                return page.navigate(url, how.withFromHistory(false));
            } catch (RuntimeException again) {
                return null;
            }
        }
    }

    private void fitViewport(int vw, int vh) {
        int w = Math.max(1, vw);
        int h = Math.max(1, vh);
        if (w != page.viewportWidth() || h != page.viewportHeight()) {
            page.setViewport(w, h);
        }
    }

    private void stopRenderer() {
        try {
            page.stop();
        } catch (RuntimeException ex) {
            SwingUtilities.invokeLater(
                () -> status("Renderer could not be restarted: " + ex.getMessage()));
        }
    }

    private void requestFavicon() {
        io.submit(() -> {
            BufferedImage icon;
            try {
                icon = FrameImages.toImage(page.favicon());
            } catch (RuntimeException ex) {
                return;
            }
            SwingUtilities.invokeLater(() -> fire(l -> l.faviconChanged(icon)));
        });
    }

    /**
     * Render the viewport once. On a HiDPI screen the frame is rasterised
     * at the screen's scale factor and drawn back at component size, so
     * text stays sharp. The loop runs at about 30 frames a second while
     * the page animates and stops after three unchanged frames; from then
     * on {@link #idleTick} keeps the page's timers going.
     */
    private void renderOnce() {
        if (renderBusy || idleBusy || loading || closed) {
            return;
        }
        renderBusy = true;
        final double raster = rasterScale();
        final int vw = (int) Math.ceil(Math.max(1, canvas.getWidth()) * raster);
        final int vh = (int) Math.ceil(Math.max(1, canvas.getHeight()) * raster);
        final int sx = scrollX;
        final int sy = scrollY;
        final double sc = scale * raster;
        final boolean caret = caretActive;
        final boolean checkTitle = System.nanoTime() - lastTickNanos > TITLE_CHECK_NANOS;
        io.submit(() -> {
            Frame frame;
            Tick tick = null;
            String copied = null;
            try {
                frame = page.render(sx, sy, vw, vh, sc, caret);
                if (frame.events().clipboardWrite()) {
                    copied = page.clipboardText();
                }
                if (checkTitle) {
                    tick = page.tick();
                }
            } catch (RuntimeException ex) {
                SwingUtilities.invokeLater(this::onRendererLost);
                return;
            }
            final BufferedImage image = FrameImages.toImage(frame);
            final Tick ticked = tick;
            final String clipboardText = copied;
            SwingUtilities.invokeLater(() -> rendered(frame, image, raster, ticked, clipboardText));
        });
    }

    private void rendered(Frame frame, BufferedImage image, double raster, Tick tick,
                          String clipboardText) {
        renderBusy = false;
        if (closed) {
            return;
        }
        if (image != null) {
            canvas.setImage(image, raster);
        }
        updateScrollModel();
        if (frame.requestedScrollY() >= 0 && frame.requestedScrollY() != scrollY) {
            setScrollY(frame.requestedScrollY());
        }
        if (frame.requestedScrollX() >= 0 && frame.requestedScrollX() != scrollX) {
            setScrollX(frame.requestedScrollX());
        }
        boolean still = frame.unchanged() && !frame.animating() && !frame.caretBlinking();
        stableFrames = still ? stableFrames + 1 : 0;
        refreshTimer.setDelay(frame.animating() ? ANIMATION_REFRESH_MS : IDLE_REFRESH_MS);
        if (clipboardText != null) {
            setClipboard(clipboardText);
            status("Copied to clipboard");
        }
        attend(frame.events());
        if (tick != null) {
            ticked(tick);
        }
        if (stableFrames >= 3 && !loading) {
            refreshTimer.stop();
            idleSoon(IDLE_TICK_MS);
        }
    }

    private void idleSoon(int millis) {
        if (closed) {
            return;
        }
        if (idleTimer == null) {
            idleTimer = new Timer(millis, e -> idleTick());
            idleTimer.setRepeats(false);
        }
        idleTimer.setInitialDelay(millis);
        idleTimer.restart();
    }

    /**
     * Let the page run while nothing is being painted: its timers fire,
     * late responses arrive, and a title or address a script changed is
     * noticed. The renderer says when its next timer is due; without one
     * the page is looked in on a few times a second.
     */
    private void idleTick() {
        if (renderBusy || idleBusy || loading || closed
                || (refreshTimer != null && refreshTimer.isRunning())) {
            return;
        }
        idleBusy = true;
        io.submit(() -> {
            Tick tick;
            try {
                tick = page.tick();
            } catch (RuntimeException ex) {
                SwingUtilities.invokeLater(() -> {
                    idleBusy = false;
                    onRendererLost();
                });
                return;
            }
            SwingUtilities.invokeLater(() -> {
                idleBusy = false;
                if (closed) {
                    return;
                }
                ticked(tick);
                if (tick.changed() || tick.animating()) {
                    refresh();
                } else if (!loading && (refreshTimer == null || !refreshTimer.isRunning())) {
                    int wake = tick.wakeMillis();
                    idleSoon(wake > 0 ? Math.max(MIN_WAKE_MS, Math.min(wake, IDLE_TICK_MS))
                                      : IDLE_TICK_MS);
                }
            });
        });
    }

    private void ticked(Tick tick) {
        lastTickNanos = System.nanoTime();
        if (loading) {
            return;
        }
        String url = tick.url();
        if (url != null && !url.isEmpty() && !url.equals(shownUrl)) {
            shownUrl = url;
            if (historyIndex >= 0) {
                history.set(historyIndex, url);
            }
            fire(l -> l.urlChanged(url));
        }
        String title = tick.title() == null ? "" : tick.title();
        if (!title.equals(shownTitle)) {
            shownTitle = title;
            fire(l -> l.titleChanged(title));
        }
        attend(tick.events());
    }

    private void attend(PageEvents events) {
        if (!events.audio().isEmpty()) {
            final String pageUrl = url();
            fire(l -> l.audioCommands(events.audio(), pageUrl));
        }
        if (events.windowAction() != null) {
            fire(l -> l.windowActionRequested(events.windowAction()));
        }
        if (!events.capabilities().isEmpty()) {
            fire(l -> l.capabilitiesRequested(events.capabilities()));
        }
        if (events.popups()) {
            pollPopups();
        }
        if (events.downloadUrl() != null) {
            fire(l -> l.downloadRequested(events.downloadUrl(), events.downloadName()));
        }
        if (events.navigation() != null && jsRedirects < MAX_JS_REDIRECTS) {
            jsRedirects++;
            navigate(events.navigation(), true, true, false);
        }
    }

    private void pollPopups() {
        if (popupPollBusy || closed) {
            return;
        }
        popupPollBusy = true;
        io.submit(() -> {
            List<Map<String, String>> polled;
            try {
                polled = page.popupOperations();
            } catch (RuntimeException ex) {
                polled = List.of();
            }
            final List<Map<String, String>> operations = polled;
            SwingUtilities.invokeLater(() -> {
                popupPollBusy = false;
                if (!closed && !operations.isEmpty()) {
                    fire(l -> l.popupOperations(operations));
                }
            });
        });
    }

    private void onRendererLost() {
        renderBusy = false;
        if (refreshTimer != null) {
            refreshTimer.stop();
        }
        if (closed) {
            return;
        }
        if (restarts >= MAX_RESTARTS || url() == null) {
            status("Renderer keeps failing — reload to retry");
            return;
        }
        restarts++;
        status("Renderer restarted");
        io.submit(() -> {
            try {
                page.stop();
            } catch (RuntimeException ex) {
                return;
            }
        });
        navigate(url(), false, true, false);
    }

    private static int modifiers(InputEvent e) {
        return (e.isShiftDown() ? 1 : 0) | (e.isControlDown() ? 2 : 0)
             | (e.isAltDown() ? 4 : 0) | (e.isMetaDown() ? 8 : 0);
    }

    /**
     * A scrollbar the page painted itself takes the press before the
     * document does, so dragging it scrolls instead of selecting text.
     */
    private void onPress(MouseEvent e) {
        final int dx = docX(e.getX());
        final int dy = docY(e.getY());
        pressDocX = dx;
        pressDocY = dy;
        dragAnchored = false;
        hasSelection = false;
        draggingScrollbar = false;
        final int mods = modifiers(e);
        io.submit(() -> {
            if (page.scrollbarPress(dx, dy)) {
                SwingUtilities.invokeLater(() -> {
                    draggingScrollbar = true;
                    refresh();
                });
                return;
            }
            String pressed = page.mouseDown(dx, dy, mods);
            if (pressed != null) {
                SwingUtilities.invokeLater(() -> navigate(pressed));
            }
        });
    }

    private void onDrag(int x, int y) {
        final int dx = docX(x);
        final int dy = docY(y);
        final boolean anchor = !dragAnchored;
        dragAnchored = true;
        hasSelection = true;
        io.submit(() -> {
            if (anchor) {
                page.select(Page.Selection.ANCHOR, pressDocX, pressDocY);
            }
            page.select(Page.Selection.EXTEND, dx, dy);
            SwingUtilities.invokeLater(this::refresh);
        });
    }

    private void onRelease(int x, int y) {
        final int dx = docX(x);
        final int dy = docY(y);
        io.submit(() -> {
            String href = page.mouseUp();
            Page.Media media = href == null ? page.mediaAt(dx, dy) : null;
            boolean editable = href == null && page.hasFocusedEditable();
            SwingUtilities.invokeLater(() -> {
                caretActive = editable;
                if (href != null) {
                    navigate(href);
                } else if (media != null) {
                    fire(l -> l.mediaRequested(media));
                } else {
                    refresh();
                }
            });
        });
    }

    /**
     * Control zooms and Shift scrolls sideways; fractional touchpad deltas
     * accumulate until they add up to a whole CSS pixel. Whatever scrolls
     * under the pointer gets first refusal, and only an unconsumed delta
     * scrolls the page.
     */
    private void onWheel(MouseWheelEvent e) {
        double notches = e.getPreciseWheelRotation();
        if (e.isControlDown()) {
            if (notches < 0) {
                zoomIn();
            } else if (notches > 0) {
                zoomOut();
            }
            return;
        }
        if (e.isShiftDown()) {
            wheelPendingX += notches * LINE_SCROLL;
        } else {
            wheelPendingY += notches * LINE_SCROLL;
        }
        final int deltaX = (int) wheelPendingX;
        final int deltaY = (int) wheelPendingY;
        wheelPendingX -= deltaX;
        wheelPendingY -= deltaY;
        if ((deltaX == 0 && deltaY == 0) || url() == null || loading) {
            return;
        }
        final int dx = docX(e.getX());
        final int dy = docY(e.getY());
        io.submit(() -> {
            boolean consumed = page.scrollAt(dx, dy, deltaX, deltaY);
            SwingUtilities.invokeLater(() -> {
                if (consumed) {
                    refresh();
                    return;
                }
                if (deltaX != 0) {
                    setScrollX(scrollX + deltaX);
                }
                if (deltaY != 0) {
                    setScrollY(scrollY + deltaY);
                }
            });
        });
    }

    private void openLinkInNewWindow(int x, int y) {
        if (url() == null || loading) {
            return;
        }
        final int dx = docX(x);
        final int dy = docY(y);
        io.submit(() -> {
            String link = page.linkAt(dx, dy);
            if (link != null) {
                SwingUtilities.invokeLater(() -> fire(l -> l.newWindowRequested(link)));
            }
        });
    }

    private void askContextMenu(int x, int y) {
        if (url() == null) {
            return;
        }
        canvas.requestFocusInWindow();
        final int dx = docX(x);
        final int dy = docY(y);
        io.submit(() -> {
            Page.ContextMenu target = page.contextMenu(dx, dy);
            final String link = target.handledByPage() ? null : page.linkAt(dx, dy);
            SwingUtilities.invokeLater(() -> {
                if (target.editable() && !target.handledByPage()) {
                    caretActive = true;
                }
                refresh();
                if (!target.handledByPage()) {
                    Point at = SwingUtilities.convertPoint(canvas, x, y, this);
                    fire(l -> l.contextMenuRequested(at.x, at.y, link, target.editable()));
                }
            });
        });
    }

    private void requestHover(int dx, int dy) {
        if (loading || url() == null) {
            return;
        }
        if (hoverBusy) {
            hoverPendingX = dx;
            hoverPendingY = dy;
            hoverPending = true;
            return;
        }
        hoverBusy = true;
        io.submit(() -> {
            Page.Hover hover;
            try {
                hover = page.hover(dx, dy);
            } catch (RuntimeException ex) {
                SwingUtilities.invokeLater(() -> hoverBusy = false);
                return;
            }
            SwingUtilities.invokeLater(() -> {
                hoverBusy = false;
                applyHover(hover);
                if (hover.changed()) {
                    refresh();
                }
                if (hoverPending) {
                    hoverPending = false;
                    requestHover(hoverPendingX, hoverPendingY);
                }
            });
        });
    }

    private void applyHover(Page.Hover hover) {
        if (hover.link() != null && !hover.link().isEmpty()) {
            hoverLink = hover.link();
            status(hover.link());
            canvas.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
            return;
        }
        if (hoverLink != null) {
            hoverLink = null;
            status("");
        }
        canvas.setCursor(cursorFor(hover.cursor()));
    }

    private static Cursor cursorFor(String css) {
        int shape = switch (css == null ? "" : css) {
            case "pointer" -> Cursor.HAND_CURSOR;
            case "text" -> Cursor.TEXT_CURSOR;
            case "wait", "progress" -> Cursor.WAIT_CURSOR;
            case "crosshair" -> Cursor.CROSSHAIR_CURSOR;
            case "move" -> Cursor.MOVE_CURSOR;
            case "ew-resize" -> Cursor.E_RESIZE_CURSOR;
            case "ns-resize" -> Cursor.N_RESIZE_CURSOR;
            default -> Cursor.DEFAULT_CURSOR;
        };
        return Cursor.getPredefinedCursor(shape);
    }

    /**
     * Text a key produced that {@code keyPressed} did not already hand to
     * the page. The engine inserts a printable character on keydown, so an
     * ordinary key is done by the time its {@code keyTyped} arrives; only
     * an input method, a dead key or a compose sequence, whose character
     * surfaces here without a printable keydown, still needs inserting.
     */
    private void onKeyTyped(KeyEvent e) {
        if (suppressTextInsert) {
            suppressTextInsert = false;
            return;
        }
        char c = e.getKeyChar();
        if (c == KeyEvent.CHAR_UNDEFINED || Character.isISOControl(c) || !producesText(e)) {
            return;
        }
        final String text = String.valueOf(c);
        final int shift = e.isShiftDown() ? 1 : 0;
        io.submit(() -> {
            page.key(Page.KeyKind.PRESS, text, "", 0, shift);
            Page.KeyResult result = page.key(Page.KeyKind.TEXT, text, "", 0, 0);
            SwingUtilities.invokeLater(() -> afterKey(result.navigation()));
        });
    }

    private void onKeyPressed(KeyEvent e) {
        suppressTextInsert = false;
        for (Listener listener : listeners) {
            if (listener.keyIntercepted(e)) {
                e.consume();
                return;
            }
        }
        if (isWindowShortcut(e)) {
            return;
        }
        int vk = e.getKeyCode();
        String edit = editCommand(e);
        if (edit != null) {
            e.consume();
            suppressTextInsert = true;
            runEditCommand(edit, printableKey(e), printableCode(e), vk, modifiers(e));
            return;
        }
        String name = keyName(vk);
        if (name != null) {
            e.consume();
            pressNamedKey(vk, name, modifiers(e));
            return;
        }
        if (vk == KeyEvent.VK_SPACE) {
            e.consume();
            suppressTextInsert = true;
            pressSpace(modifiers(e), e.isShiftDown());
            return;
        }
        final String key = printableForPress(e);
        if (key == null) {
            return;
        }
        final String code = printableCode(e);
        final boolean text = producesText(e);
        final int mods = text ? (e.isShiftDown() ? 1 : 0) : modifiers(e);
        heldKeys.put(vk, new String[]{key, code, Integer.toString(vk)});
        suppressTextInsert = text;
        io.submit(() -> {
            String navigation = page.key(Page.KeyKind.DOWN, key, code, vk, mods).navigation();
            if (text && navigation == null) {
                navigation = page.key(Page.KeyKind.PRESS, key, code, vk, mods).navigation();
            }
            final String target = navigation;
            SwingUtilities.invokeLater(() -> afterKey(target));
        });
    }

    private void onKeyReleased(KeyEvent e) {
        String[] held = heldKeys.remove(e.getKeyCode());
        if (held == null) {
            return;
        }
        final int mods = modifiers(e);
        io.submit(() -> {
            Page.KeyResult result = page.key(Page.KeyKind.UP, held[0], held[1],
                                             Integer.parseInt(held[2]), mods);
            SwingUtilities.invokeLater(() -> afterKey(result.navigation()));
        });
    }

    private void afterKey(String navigation) {
        if (navigation != null) {
            navigate(navigation);
        } else {
            refresh();
        }
    }

    /**
     * A named key goes to the page first. When the page leaves it alone
     * and no text field has the focus, the navigation keys scroll the
     * page; in a field they move the caret instead. Tab, Enter and Escape
     * are how the focus enters and leaves a field without a click, so they
     * ask again where it went.
     */
    private void pressNamedKey(int vk, String name, int mods) {
        final int code = keyCode(name);
        heldKeys.put(vk, new String[]{name, name, Integer.toString(code)});
        final boolean movesFocus = name.equals("Tab") || name.equals("Enter")
            || name.equals("Escape");
        final boolean probe = movesFocus || scrollStep(name) != null;
        io.submit(() -> {
            Page.KeyResult result = page.key(Page.KeyKind.DOWN, name, name, code, mods);
            final boolean editable = probe && result.navigation() == null
                && page.hasFocusedEditable();
            SwingUtilities.invokeLater(() -> {
                if (movesFocus) {
                    caretActive = editable;
                }
                if (result.navigation() != null) {
                    navigate(result.navigation());
                } else if (!result.prevented() && !editable && scrollStep(name) != null) {
                    scrollForKey(name);
                } else {
                    refresh();
                }
            });
        });
    }

    private int[] scrollStep(String key) {
        return switch (key) {
            case "ArrowDown" -> new int[]{0, LINE_SCROLL};
            case "ArrowUp" -> new int[]{0, -LINE_SCROLL};
            case "ArrowRight" -> new int[]{LINE_SCROLL, 0};
            case "ArrowLeft" -> new int[]{-LINE_SCROLL, 0};
            case "PageDown" -> new int[]{0, pageStep()};
            case "PageUp" -> new int[]{0, -pageStep()};
            case "Home" -> new int[]{0, -scrollY};
            case "End" -> new int[]{0, maxScrollY() - scrollY};
            default -> null;
        };
    }

    private void scrollForKey(String key) {
        int[] step = scrollStep(key);
        if (step[0] != 0) {
            setScrollX(scrollX + step[0]);
        } else {
            setScrollY(scrollY + step[1]);
        }
    }

    /**
     * Space types a space into a text field, presses a focused button, and
     * otherwise pages the document down, or up with Shift.
     */
    private void pressSpace(int mods, boolean shift) {
        heldKeys.put(KeyEvent.VK_SPACE, new String[]{" ", "Space", "32"});
        io.submit(() -> {
            Page.KeyResult down = page.key(Page.KeyKind.DOWN, " ", "Space", 32, mods);
            Page.KeyResult press = down.navigation() == null
                ? page.key(Page.KeyKind.PRESS, " ", "Space", 32, mods) : down;
            final String navigation = press.navigation();
            final boolean prevented = down.prevented() || press.prevented();
            final boolean editable = navigation == null && page.hasFocusedEditable();
            SwingUtilities.invokeLater(() -> {
                if (navigation != null) {
                    navigate(navigation);
                } else if (!prevented && !editable && (mods & ~1) == 0) {
                    setScrollY(scrollY + (shift ? -pageStep() : pageStep()));
                } else {
                    refresh();
                }
            });
        });
    }

    /**
     * The edit a clipboard shortcut stands for: the platform's menu key
     * with X, C, V or A, Shift+Insert and Control+Insert; else null.
     */
    private static String editCommand(KeyEvent e) {
        int all = InputEvent.SHIFT_DOWN_MASK | InputEvent.CTRL_DOWN_MASK
                | InputEvent.ALT_DOWN_MASK | InputEvent.META_DOWN_MASK;
        int mods = e.getModifiersEx() & all;
        int vk = e.getKeyCode();
        if (vk == KeyEvent.VK_INSERT) {
            return mods == InputEvent.SHIFT_DOWN_MASK ? "paste"
                : mods == InputEvent.CTRL_DOWN_MASK ? "copy" : null;
        }
        int primary = Toolkit.getDefaultToolkit().getMenuShortcutKeyMaskEx();
        if ((mods & primary) == 0 || (mods & ~(primary | InputEvent.SHIFT_DOWN_MASK)) != 0) {
            return null;
        }
        return switch (vk) {
            case KeyEvent.VK_X -> "cut";
            case KeyEvent.VK_C -> "copy";
            case KeyEvent.VK_V -> "paste";
            case KeyEvent.VK_A -> "selectAll";
            default -> null;
        };
    }

    /**
     * The page sees the shortcut's keydown first and may cancel it;
     * otherwise the edit runs against the focused field or the selection.
     */
    private void runEditCommand(String command, String key, String code, int keyCode,
                                int mods) {
        final String pasteText = command.equals("paste") ? clipboardText() : null;
        if (command.equals("paste") && pasteText == null) {
            return;
        }
        if (command.equals("selectAll")) {
            hasSelection = true;
        }
        io.submit(() -> {
            if (key != null && page.key(Page.KeyKind.DOWN, key, code, keyCode, mods).prevented()) {
                SwingUtilities.invokeLater(this::refresh);
                return;
            }
            switch (command) {
                case "copy" -> {
                    String text = page.select(Page.Selection.COPY, 0, 0);
                    SwingUtilities.invokeLater(() -> copied(text, "Copied selection"));
                }
                case "cut" -> {
                    String text = page.select(Page.Selection.CUT, 0, 0);
                    SwingUtilities.invokeLater(() -> {
                        copied(text, "Cut selection");
                        refresh();
                    });
                }
                case "paste" -> {
                    String navigation = page.paste(pasteText).navigation();
                    SwingUtilities.invokeLater(() -> afterKey(navigation));
                }
                default -> {
                    page.select(Page.Selection.ALL, 0, 0);
                    SwingUtilities.invokeLater(this::refresh);
                }
            }
        });
    }

    private void copied(String text, String message) {
        if (text != null && !text.isEmpty()) {
            setClipboard(text);
            status(message);
        }
    }

    /**
     * Whether the window this view sits in has bound the key itself, as
     * for Alt+Left or Control+L: those belong to the window, so the page
     * must neither see nor swallow them.
     */
    private boolean isWindowShortcut(KeyEvent e) {
        JRootPane root = SwingUtilities.getRootPane(this);
        return root != null && root.getInputMap(JComponent.WHEN_IN_FOCUSED_WINDOW)
            .get(KeyStroke.getKeyStrokeForEvent(e)) != null;
    }

    /**
     * Whether a key event types text rather than issuing a shortcut: no
     * Control, Alt or Meta, or AltGr (which Windows reports as Control+Alt)
     * producing a character, as for {@code @} on many European layouts.
     */
    private static boolean producesText(KeyEvent e) {
        if (!e.isControlDown() && !e.isAltDown() && !e.isMetaDown()) {
            return true;
        }
        char ch = e.getKeyChar();
        boolean altGr = e.isAltGraphDown() || (e.isControlDown() && e.isAltDown());
        return altGr && !e.isMetaDown() && ch != KeyEvent.CHAR_UNDEFINED
            && !Character.isISOControl(ch);
    }

    private static String keyName(int vk) {
        return switch (vk) {
            case KeyEvent.VK_BACK_SPACE -> "Backspace";
            case KeyEvent.VK_DELETE -> "Delete";
            case KeyEvent.VK_ENTER -> "Enter";
            case KeyEvent.VK_TAB -> "Tab";
            case KeyEvent.VK_ESCAPE -> "Escape";
            case KeyEvent.VK_LEFT -> "ArrowLeft";
            case KeyEvent.VK_RIGHT -> "ArrowRight";
            case KeyEvent.VK_UP -> "ArrowUp";
            case KeyEvent.VK_DOWN -> "ArrowDown";
            case KeyEvent.VK_HOME -> "Home";
            case KeyEvent.VK_END -> "End";
            case KeyEvent.VK_PAGE_UP -> "PageUp";
            case KeyEvent.VK_PAGE_DOWN -> "PageDown";
            default -> null;
        };
    }

    private static int keyCode(String name) {
        return switch (name) {
            case "Backspace" -> 8;
            case "Tab" -> 9;
            case "Enter" -> 13;
            case "Escape" -> 27;
            case "PageUp" -> 33;
            case "PageDown" -> 34;
            case "End" -> 35;
            case "Home" -> 36;
            case "ArrowLeft" -> 37;
            case "ArrowUp" -> 38;
            case "ArrowRight" -> 39;
            case "ArrowDown" -> 40;
            case "Delete" -> 46;
            default -> 0;
        };
    }

    /**
     * The character a printable key produces at {@code keyPressed} time.
     * The event's own character respects Shift and the keyboard layout, as
     * on the shifted number row; a letter or digit from the key code is
     * the fallback.
     */
    private static String printableForPress(KeyEvent e) {
        char ch = e.getKeyChar();
        if (ch != KeyEvent.CHAR_UNDEFINED && !Character.isISOControl(ch)) {
            return String.valueOf(ch);
        }
        return printableKey(e);
    }

    private static String printableKey(KeyEvent e) {
        int vk = e.getKeyCode();
        if (vk >= KeyEvent.VK_A && vk <= KeyEvent.VK_Z) {
            char c = (char) ('a' + (vk - KeyEvent.VK_A));
            return String.valueOf(e.isShiftDown() ? Character.toUpperCase(c) : c);
        }
        if (vk >= KeyEvent.VK_0 && vk <= KeyEvent.VK_9) {
            return String.valueOf((char) ('0' + (vk - KeyEvent.VK_0)));
        }
        char ch = e.getKeyChar();
        return ch != KeyEvent.CHAR_UNDEFINED && !Character.isISOControl(ch)
            ? String.valueOf(ch) : null;
    }

    private static String printableCode(KeyEvent e) {
        int vk = e.getKeyCode();
        if (vk >= KeyEvent.VK_A && vk <= KeyEvent.VK_Z) {
            return "Key" + (char) ('A' + (vk - KeyEvent.VK_A));
        }
        if (vk >= KeyEvent.VK_0 && vk <= KeyEvent.VK_9) {
            return "Digit" + (char) ('0' + (vk - KeyEvent.VK_0));
        }
        return "";
    }

    private static void setClipboard(String text) {
        try {
            Toolkit.getDefaultToolkit().getSystemClipboard()
                .setContents(new StringSelection(text), null);
        } catch (IllegalStateException | java.awt.HeadlessException ex) {
            return;
        }
    }

    private static String clipboardText() {
        try {
            Object data = Toolkit.getDefaultToolkit().getSystemClipboard()
                .getData(DataFlavor.stringFlavor);
            return data instanceof String text && !text.isEmpty() ? text : null;
        } catch (UnsupportedFlavorException | IOException | IllegalStateException
                 | java.awt.HeadlessException e) {
            return null;
        }
    }

    /**
     * Shows the last rendered frame. A frame rasterised at {@code raster}
     * device pixels per component pixel is drawn back at component size,
     * which on a HiDPI screen maps it one to one onto the display.
     */
    private static final class Canvas extends JComponent {
        private static final long serialVersionUID = 1L;
        private transient BufferedImage image;
        private double raster = 1.0;

        void setImage(BufferedImage image, double raster) {
            this.image = image;
            this.raster = raster > 0 ? raster : 1.0;
            repaint();
        }

        @Override
        protected void paintComponent(Graphics g) {
            g.setColor(Color.WHITE);
            g.fillRect(0, 0, getWidth(), getHeight());
            if (image == null) {
                return;
            }
            if (raster == 1.0) {
                g.drawImage(image, 0, 0, null);
                return;
            }
            Graphics2D g2 = (Graphics2D) g.create();
            g2.setRenderingHint(RenderingHints.KEY_INTERPOLATION,
                                RenderingHints.VALUE_INTERPOLATION_BILINEAR);
            g2.drawImage(image, 0, 0, (int) Math.round(image.getWidth() / raster),
                         (int) Math.round(image.getHeight() / raster), null);
            g2.dispose();
        }
    }
}

/* Nordstjernen — official Java browser (Swing chrome around a BrowserView).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.app;

import org.nordstjernen.AudioHelper;
import org.nordstjernen.browser.BrowserContext;
import org.nordstjernen.browser.CapabilityRequest;
import org.nordstjernen.browser.ContextOptions;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.browser.Page;
import org.nordstjernen.browser.PdfOptions;
import org.nordstjernen.browser.Security;
import org.nordstjernen.swing.BrowserView;

import javax.swing.AbstractAction;
import javax.swing.BorderFactory;
import javax.swing.ImageIcon;
import javax.swing.JButton;
import javax.swing.JComponent;
import javax.swing.JDialog;
import javax.swing.JFileChooser;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JMenu;
import javax.swing.JMenuItem;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JPopupMenu;
import javax.swing.JScrollPane;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.JToolBar;
import javax.swing.KeyStroke;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import javax.swing.UIManager;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Component;
import java.awt.Cursor;
import java.awt.Desktop;
import java.awt.FlowLayout;
import java.awt.Font;
import java.awt.GraphicsDevice;
import java.awt.Image;
import java.awt.Insets;
import java.awt.Toolkit;
import java.awt.datatransfer.DataFlavor;
import java.awt.datatransfer.StringSelection;
import java.awt.datatransfer.UnsupportedFlavorException;
import java.awt.event.ActionEvent;
import java.awt.event.FocusAdapter;
import java.awt.event.FocusEvent;
import java.awt.event.KeyEvent;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URI;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * A small, standalone Java web browser: back, forward, reload, home, an
 * address bar and a status bar around a {@link BrowserView}. The page runs
 * in a separate {@code nordstjernen-renderer} process; no engine code is
 * loaded into the JVM.
 *
 * <p>The view does everything that happens inside the page: painting,
 * scrolling, zoom, the mouse and the keyboard, text selection, history and
 * following navigations. This class is the window around it: the toolbar
 * and its shortcuts, find-in-page ({@code Ctrl+F}), the context and overflow
 * menus, downloads, permission prompts, the windows a page opens, a
 * JavaScript console and page dumps ({@code F12}), saving a page as PDF or
 * PNG, page fullscreen, sound through the {@code nordstjernen-audio} helper,
 * and private windows.
 *
 * <p>Keyboard: {@code Alt+Left}/{@code Alt+Right} navigate history,
 * {@code Ctrl+L} or {@code Alt+D} focuses the address bar,
 * {@code Ctrl+R}/{@code F5} reloads, {@code Alt+Home} goes home,
 * {@code Ctrl+N}/{@code Ctrl+T} open a new window, {@code Ctrl+Shift+N} a
 * private one, {@code Ctrl+W} closes one and {@code Ctrl+Q} quits.
 *
 * <p>Point at the renderer binary with {@code -Dnordstjernen.renderer=…} or
 * the {@code NORDSTJERNEN_RENDERER} environment variable.
 */
public final class Browser {

    private static final String HOME_URL = "about:start";
    private static final int CONSOLE_POLL_MS = 250;
    private static final int DOWNLOAD_POLL_MS = 250;
    private static final int FRAME_MAX_W = 6144;
    private static final int FRAME_MAX_H = 3456;
    private static final String SEARCH_URL = "https://duckduckgo.com/?q=";
    private static final String VERSION = resolveVersion();
    private static final String TITLE_SUFFIX = " (Java " + VERSION + ")";

    private static final Set<Browser> WINDOWS = new LinkedHashSet<>();
    private static final Map<String, PopupLink> POPUP_LINKS = new HashMap<>();
    private static Engine sharedEngine;

    private final boolean privateMode;
    private final BrowserContext ownContext;
    private final Page page;
    private final BrowserView view;
    private final Executor io;
    private final AudioHelper audio = new AudioHelper();

    private final JFrame frame = new JFrame("Nordstjernen Web Browser" + TITLE_SUFFIX);
    private final JToolBar toolbar = new JToolBar();
    private final JPanel south = new JPanel(new BorderLayout());
    private final JButton back = navButton("back", "◀", "Back (Alt+Left)");
    private final JButton forward = navButton("forward", "▶", "Forward (Alt+Right)");
    private final JButton reload = navButton("reload", "↻", "Reload (Ctrl+R)");
    private final JButton home = navButton("home", "⌂", "Home (Alt+Home)");
    private final JTextField address = new JTextField();
    private final JLabel securityBadge = new JLabel();
    private final JLabel status = new JLabel(" ");
    private final Image logoImage = loadLogo();

    private final JPanel findBar = new JPanel(new BorderLayout(4, 0));
    private final JTextField findField = new JTextField();
    private final JLabel findCount = new JLabel(" ");

    private final Set<Long> activeDownloads = new HashSet<>();
    private final Map<String, Boolean> capabilityAnswers = new HashMap<>();
    private boolean pageFullscreen;
    private boolean closed;

    private JDialog console;
    private JTextArea consoleOut;
    private JTextField consoleIn;
    private Timer consolePoll;

    static {
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            List<Browser> open;
            synchronized (WINDOWS) {
                open = new ArrayList<>(WINDOWS);
            }
            for (Browser b : open) {
                b.audio.close();
                b.page.close();
                if (b.ownContext != null) {
                    b.ownContext.close();
                }
            }
        }, "ns-shutdown"));
    }

    /** A popup and the window that opened it, keyed by the engine's token. */
    private static final class PopupLink {
        final String token;
        Browser opener;
        final Browser popup;

        PopupLink(String token, Browser opener, Browser popup) {
            this.token = token;
            this.opener = opener;
            this.popup = popup;
        }
    }

    /**
     * The engine every window takes its page from, sized so a maximised
     * window on a high-density screen still renders at full resolution.
     */
    private static synchronized Engine engine() {
        if (sharedEngine == null || sharedEngine.isClosed()) {
            sharedEngine = Engine.launch(
                Engine.Options.defaults().withFrameSize(FRAME_MAX_W, FRAME_MAX_H));
        }
        return sharedEngine;
    }

    private Browser(String startUrl, boolean privateMode, String openerToken,
                    String openerOrigin) {
        this.privateMode = privateMode;
        Engine shared = engine();
        this.ownContext = privateMode ? shared.newContext(ContextOptions.inMemory()) : null;
        this.page = (privateMode ? ownContext : shared.userContext()).newPage();
        this.page.setPopupsSupported(true);
        if (openerToken != null) {
            this.page.setOpener(openerToken, openerOrigin);
        }
        this.view = new BrowserView(page);
        this.io = view.pageExecutor();
        this.view.addListener(new PageSide());
        buildUi();
        view.navigate(startUrl);
    }

    public static void main(String[] args) {
        try {
            UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
        } catch (ReflectiveOperationException | javax.swing.UnsupportedLookAndFeelException e) {
            System.err.println("nordstjernen: keeping the default look and feel: " + e);
        }
        String start = args.length > 0 ? normalize(args[0]) : HOME_URL;
        SwingUtilities.invokeLater(() -> openWindow(start, false));
    }

    /**
     * Open another browser window on {@code url}, with its own renderer
     * process. A private window's renderer keeps no cookies, cache or
     * history, and none of it outlives the window.
     */
    private static Browser openWindow(String url, boolean privateMode) {
        return openWindow(url, privateMode, null, null);
    }

    /**
     * Open a window for a page another page opened with
     * {@code window.open()}: {@code openerToken} and {@code openerOrigin},
     * from the opener's request, give the new page its
     * {@code window.opener}.
     */
    private static Browser openWindow(String url, boolean privateMode, String openerToken,
                                      String openerOrigin) {
        Browser b = new Browser(url == null || url.isEmpty() ? HOME_URL : url,
                                privateMode, openerToken, openerOrigin);
        synchronized (WINDOWS) {
            WINDOWS.add(b);
        }
        return b;
    }

    private void closeWindow() {
        if (closed) {
            return;
        }
        unlinkPopups();
        boolean last;
        synchronized (WINDOWS) {
            WINDOWS.remove(this);
            last = WINDOWS.isEmpty();
        }
        closed = true;
        frame.dispose();
        if (consolePoll != null) {
            consolePoll.stop();
        }
        final List<Long> unfinished = new ArrayList<>(activeDownloads);
        activeDownloads.clear();
        io.execute(() -> {
            audio.close();
            for (long id : unfinished) {
                try {
                    page.cancelDownload(id);
                } catch (RuntimeException ex) {
                    break;
                }
            }
        });
        view.close();
        io.execute(() -> {
            if (ownContext != null) {
                ownContext.close();
            }
            if (last) {
                System.exit(0);
            }
        });
    }

    /** Quit every window; the shutdown hook stops their renderer processes. */
    private static void quitAll() {
        System.exit(0);
    }

    /** What the view reports from the page, shown or decided by this window. */
    private final class PageSide implements BrowserView.Listener {

        @Override
        public void navigationStarted(String url) {
            closeFind();
            leavePageFullscreen(false);
            audio.stop();
        }

        @Override
        public void urlChanged(String url) {
            address.setText(url);
        }

        @Override
        public void titleChanged(String title) {
            frame.setTitle((title.isEmpty() ? "Untitled" : title)
                + " — Nordstjernen Web Browser" + (privateMode ? " (Private)" : "")
                + TITLE_SUFFIX);
        }

        @Override
        public void historyChanged() {
            back.setEnabled(view.canGoBack());
            forward.setEnabled(view.canGoForward());
        }

        @Override
        public void securityChanged(Security security, String serverAddress) {
            showSecurity(security, serverAddress);
        }

        @Override
        public void faviconChanged(BufferedImage icon) {
            if (icon != null) {
                frame.setIconImage(icon);
            } else if (logoImage != null) {
                frame.setIconImage(logoImage);
            }
        }

        @Override
        public void statusChanged(String text) {
            setStatus(text);
        }

        @Override
        public void findResult(String query, int current, int total) {
            if (!findBar.isVisible()) {
                return;
            }
            findCount.setText(total > 0 ? current + "/" + total
                                        : query.isEmpty() ? " " : "No results");
        }

        @Override
        public void newWindowRequested(String url) {
            openWindow(url, privateMode);
        }

        @Override
        public void popupOperations(List<Map<String, String>> operations) {
            handlePopupOps(operations);
        }

        @Override
        public void downloadRequested(String url, String suggestedName) {
            startDownload(url, suggestedName);
        }

        @Override
        public void capabilitiesRequested(List<CapabilityRequest> requests) {
            promptCapabilities(requests);
        }

        @Override
        public void contextMenuRequested(int x, int y, String link, boolean editable) {
            showContextMenu(x, y, link, editable);
        }

        @Override
        public void mediaRequested(Page.Media media) {
            launchMedia(media);
        }

        @Override
        public void windowActionRequested(String action) {
            if ("fullscreen-enter".equals(action)) {
                enterPageFullscreen();
            } else if ("fullscreen-exit".equals(action)) {
                leavePageFullscreen(false);
            }
        }

        @Override
        public void audioCommands(List<String> commands, String pageUrl) {
            audio.send(commands, pageUrl);
        }

        @Override
        public boolean keyIntercepted(KeyEvent event) {
            int vk = event.getKeyCode();
            if (pageFullscreen && (vk == KeyEvent.VK_ESCAPE || vk == KeyEvent.VK_F11)) {
                leavePageFullscreen(true);
                return true;
            }
            if (vk == KeyEvent.VK_ESCAPE && findBar.isVisible()) {
                closeFind();
                return true;
            }
            return false;
        }
    }

    private void buildUi() {
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                closeWindow();
            }
        });
        frame.setSize(1100, 820);
        if (logoImage != null) {
            frame.setIconImage(logoImage);
        }

        toolbar.setFloatable(false);
        for (JButton b : new JButton[]{back, forward, reload, home}) {
            toolbar.add(b);
        }
        securityBadge.setBorder(BorderFactory.createEmptyBorder(0, 6, 0, 2));
        toolbar.add(securityBadge);
        toolbar.add(address);
        JButton go = navButton("go", "Go", "Go");
        toolbar.add(go);
        toolbar.addSeparator();
        JButton menu = new JButton("⋮");
        menu.setFocusable(false);
        menu.setToolTipText("Menu");
        menu.setFont(menu.getFont().deriveFont(Font.BOLD, 22f));
        menu.setMargin(new Insets(2, 10, 2, 10));
        menu.addActionListener(e -> showMenu(menu));
        toolbar.add(menu);
        if (logoImage != null) {
            JLabel brand = new JLabel(new ImageIcon(logoImage));
            brand.setBorder(BorderFactory.createEmptyBorder(0, 6, 0, 6));
            brand.setToolTipText("Nordstjernen Web Browser — nordstjernen.org");
            brand.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
            brand.addMouseListener(new MouseAdapter() {
                @Override
                public void mouseClicked(MouseEvent e) {
                    view.navigate("https://nordstjernen.org");
                }
            });
            toolbar.add(brand);
        }

        back.setEnabled(false);
        forward.setEnabled(false);
        back.addActionListener(e -> view.back());
        forward.addActionListener(e -> view.forward());
        reload.addActionListener(e -> view.reload());
        home.addActionListener(e -> view.navigate(HOME_URL));
        go.addActionListener(e -> view.navigate(normalize(address.getText())));
        address.addActionListener(e -> view.navigate(normalize(address.getText())));
        address.addFocusListener(new FocusAdapter() {
            @Override
            public void focusGained(FocusEvent e) {
                address.selectAll();
            }
        });

        buildFindBar();
        south.add(findBar, BorderLayout.NORTH);
        south.add(status, BorderLayout.SOUTH);

        frame.add(toolbar, BorderLayout.NORTH);
        frame.add(view, BorderLayout.CENTER);
        frame.add(south, BorderLayout.SOUTH);

        installShortcuts();
        frame.setVisible(true);
    }

    private void installShortcuts() {
        JComponent root = frame.getRootPane();
        bindWindow(root, "alt LEFT", "back", view::back);
        bindWindow(root, "alt RIGHT", "forward", view::forward);
        bindWindow(root, "F5", "reload", view::reload);
        bindWindow(root, "control R", "reload2", view::reload);
        bindWindow(root, "alt HOME", "home", () -> view.navigate(HOME_URL));
        bindWindow(root, "control L", "focusUrl", this::focusAddress);
        bindWindow(root, "alt D", "focusUrl2", this::focusAddress);
        bindWindow(root, "control N", "newWindow", () -> openWindow(HOME_URL, false));
        bindWindow(root, "control T", "newWindow2", () -> openWindow(HOME_URL, false));
        bindWindow(root, "control shift N", "newPrivate", () -> openWindow(HOME_URL, true));
        bindWindow(root, "control W", "close", this::closeWindow);
        bindWindow(root, "control Q", "quit", Browser::quitAll);
        bindWindow(root, "control F", "find", this::openFind);
        bindWindow(root, "control G", "findNext", () -> runFind(Page.FindDirection.NEXT));
        bindWindow(root, "control shift G", "findPrev",
                   () -> runFind(Page.FindDirection.PREVIOUS));
        bindWindow(root, "control EQUALS", "zoomIn", view::zoomIn);
        bindWindow(root, "control PLUS", "zoomIn2", view::zoomIn);
        bindWindow(root, "control ADD", "zoomIn3", view::zoomIn);
        bindWindow(root, "control MINUS", "zoomOut", view::zoomOut);
        bindWindow(root, "control SUBTRACT", "zoomOut2", view::zoomOut);
        bindWindow(root, "control 0", "zoomReset", () -> view.setZoom(1.0));
        bindWindow(root, "control P", "savePdf", () -> savePage(true));
        bindWindow(root, "F12", "console", this::toggleConsole);
        bindWindow(root, "control shift J", "console2", this::toggleConsole);
    }

    private static void bindWindow(JComponent c, String keys, String name, Runnable action) {
        c.getInputMap(JComponent.WHEN_IN_FOCUSED_WINDOW).put(KeyStroke.getKeyStroke(keys), name);
        c.getActionMap().put(name, asAction(action));
    }

    private static AbstractAction asAction(Runnable action) {
        return new AbstractAction() {
            private static final long serialVersionUID = 1L;

            @Override
            public void actionPerformed(ActionEvent e) {
                action.run();
            }
        };
    }

    private void focusAddress() {
        address.requestFocusInWindow();
        address.selectAll();
    }

    private void setStatus(String text) {
        status.setText(text == null || text.isEmpty() ? " " : text);
    }

    private void setStatusLater(String text) {
        SwingUtilities.invokeLater(() -> setStatus(text));
    }

    /**
     * Show how the page was fetched: a lock for a validated HTTPS chain, a
     * warning for an untrusted certificate or plain HTTP, nothing for
     * {@code about:} and local pages.
     */
    private void showSecurity(Security security, String serverAddress) {
        String where = serverAddress == null ? "" : " (" + serverAddress + ")";
        switch (security) {
            case SECURE -> {
                securityBadge.setText("🔒");
                securityBadge.setForeground(new Color(0x1B7F3B));
                securityBadge.setToolTipText("Encrypted connection, validated certificate" + where);
            }
            case UNTRUSTED -> {
                securityBadge.setText("⚠");
                securityBadge.setForeground(new Color(0xB03A2E));
                securityBadge.setToolTipText("Not secure — the certificate is not trusted" + where);
            }
            case INSECURE -> {
                securityBadge.setText("⚠");
                securityBadge.setForeground(new Color(0xB03A2E));
                securityBadge.setToolTipText(
                    "Not encrypted — this page travelled in the clear" + where);
            }
            default -> {
                securityBadge.setText("");
                securityBadge.setToolTipText(null);
            }
        }
    }

    private void buildFindBar() {
        findBar.setVisible(false);
        findBar.setBorder(BorderFactory.createEmptyBorder(2, 6, 2, 6));
        findField.addActionListener(e -> runFind(Page.FindDirection.NEXT));
        findField.getDocument().addDocumentListener(new DocumentListener() {
            @Override
            public void insertUpdate(DocumentEvent e) {
                runFind(Page.FindDirection.FROM);
            }

            @Override
            public void removeUpdate(DocumentEvent e) {
                runFind(Page.FindDirection.FROM);
            }

            @Override
            public void changedUpdate(DocumentEvent e) {
                runFind(Page.FindDirection.FROM);
            }
        });
        findField.getInputMap().put(KeyStroke.getKeyStroke("ESCAPE"), "closeFind");
        findField.getInputMap().put(KeyStroke.getKeyStroke("shift ENTER"), "findPrev");
        findField.getActionMap().put("closeFind", asAction(this::closeFind));
        findField.getActionMap().put("findPrev",
                                     asAction(() -> runFind(Page.FindDirection.PREVIOUS)));

        JButton prev = new JButton("▲");
        JButton next = new JButton("▼");
        JButton close = new JButton("✕");
        for (JButton b : new JButton[]{prev, next, close}) {
            b.setFocusable(false);
        }
        prev.setToolTipText("Previous match (Shift+Enter)");
        next.setToolTipText("Next match (Enter)");
        close.setToolTipText("Close (Esc)");
        prev.addActionListener(e -> runFind(Page.FindDirection.PREVIOUS));
        next.addActionListener(e -> runFind(Page.FindDirection.NEXT));
        close.addActionListener(e -> closeFind());

        JPanel right = new JPanel(new FlowLayout(FlowLayout.RIGHT, 4, 0));
        right.add(findCount);
        right.add(prev);
        right.add(next);
        right.add(close);

        findBar.add(new JLabel("Find: "), BorderLayout.WEST);
        findBar.add(findField, BorderLayout.CENTER);
        findBar.add(right, BorderLayout.EAST);
    }

    private void openFind() {
        if (view.url() == null) {
            return;
        }
        findBar.setVisible(true);
        findBar.getParent().revalidate();
        findField.requestFocusInWindow();
        findField.selectAll();
        if (!findField.getText().isEmpty()) {
            runFind(Page.FindDirection.FROM);
        }
    }

    private void closeFind() {
        if (!findBar.isVisible()) {
            return;
        }
        findBar.setVisible(false);
        findCount.setText(" ");
        findBar.getParent().revalidate();
        view.find("", Page.FindDirection.FROM);
        view.focusPage();
    }

    private void runFind(Page.FindDirection direction) {
        if (findBar.isVisible()) {
            view.find(findField.getText(), direction);
        }
    }

    /**
     * The native menu, which the view asks for only when the page did not
     * put up its own.
     */
    private void showContextMenu(int x, int y, String link, boolean editable) {
        JPopupMenu menu = new JPopupMenu();
        if (link != null && !link.isEmpty()) {
            menu.add(item("Open Link", () -> view.navigate(link)));
            menu.add(item("Open Link in New Window", () -> openWindow(link, privateMode)));
            menu.add(item("Copy Link Address", () -> copyText(link, "Copied link address")));
            menu.addSeparator();
        }
        if (editable) {
            menu.add(item("Cut", view::cut));
            menu.add(item("Copy", view::copy));
            JMenuItem paste = item("Paste", view::paste);
            paste.setEnabled(clipboardHasText());
            menu.add(paste);
            menu.addSeparator();
        } else if (view.hasSelection()) {
            menu.add(item("Copy", view::copy));
            menu.addSeparator();
        }
        JMenuItem goBack = item("Back", view::back);
        goBack.setEnabled(view.canGoBack());
        JMenuItem goForward = item("Forward", view::forward);
        goForward.setEnabled(view.canGoForward());
        menu.add(goBack);
        menu.add(goForward);
        menu.add(item("Reload", view::reload));
        menu.addSeparator();
        menu.add(item("Select All", view::selectAll));
        menu.add(item("Copy Page Address", () -> copyText(view.url(), "Copied page address")));
        menu.add(item("Save Page as PDF…", () -> savePage(true)));
        menu.add(item("Save Page as Image…", () -> savePage(false)));
        menu.show(view, x, y);
    }

    private static JMenuItem item(String label, Runnable action) {
        JMenuItem item = new JMenuItem(label);
        item.addActionListener(e -> action.run());
        return item;
    }

    private static JMenuItem item(String label, String accelerator, Runnable action) {
        JMenuItem item = item(label, action);
        item.setAccelerator(KeyStroke.getKeyStroke(accelerator));
        return item;
    }

    private void showMenu(Component anchor) {
        boolean hasPage = view.url() != null;
        JPopupMenu menu = new JPopupMenu();
        menu.add(item("New Window", "control N", () -> openWindow(HOME_URL, false)));
        menu.add(item("New Private Window", "control shift N", () -> openWindow(HOME_URL, true)));
        menu.add(enabled(item("Reload", "control R", view::reload), hasPage));
        menu.add(enabled(item("Find in Page…", "control F", this::openFind), hasPage));

        JMenu zoom = new JMenu("Zoom (" + Math.round(view.zoom() * 100) + "%)");
        zoom.add(item("Zoom In", "control PLUS", view::zoomIn));
        zoom.add(item("Zoom Out", "control MINUS", view::zoomOut));
        zoom.add(item("Reset Zoom", "control 0", () -> view.setZoom(1.0)));
        zoom.setEnabled(hasPage);
        menu.add(zoom);
        menu.addSeparator();

        menu.add(enabled(item("Save Page as PDF…", "control P", () -> savePage(true)), hasPage));
        menu.add(enabled(item("Save Page as Image…", () -> savePage(false)), hasPage));
        menu.add(enabled(item("Copy Page Address",
                              () -> copyText(view.url(), "Copied page address")), hasPage));
        menu.addSeparator();

        menu.add(item("History", () -> view.navigate("about:history")));
        menu.add(item("Settings", () -> view.navigate("about:settings")));
        menu.addSeparator();

        menu.add(item("JavaScript Console", "F12", this::toggleConsole));
        JMenu inspect = new JMenu("Inspect Page");
        inspect.setEnabled(hasPage);
        inspect.add(item("View Page Source", () -> showDump(Page.Dump.DOM, "View Page Source")));
        inspect.add(item("Layout Tree", () -> showDump(Page.Dump.LAYOUT, "Layout Tree")));
        inspect.add(item("Extracted Text", () -> showDump(Page.Dump.TEXT, "Extracted Text")));
        inspect.add(item("Network Log", () -> showDump(Page.Dump.NETWORK, "Network Log")));
        inspect.add(item("Performance", () -> showDump(Page.Dump.PERFORMANCE, "Performance")));
        menu.add(inspect);
        menu.addSeparator();

        menu.add(item("Visit nordstjernen.org", () -> view.navigate("https://nordstjernen.org")));
        menu.add(item("Licenses", () -> view.navigate("about:license")));
        menu.add(item("About Nordstjernen Web Browser",
                      () -> view.navigate("about:nordstjernen")));
        menu.show(anchor, anchor.getWidth() - menu.getPreferredSize().width, anchor.getHeight());
    }

    private static JMenuItem enabled(JMenuItem item, boolean enabled) {
        item.setEnabled(enabled);
        return item;
    }

    private void showDump(Page.Dump kind, String label) {
        if (view.url() == null) {
            return;
        }
        setStatus("Collecting " + label.toLowerCase(Locale.ROOT) + " …");
        io.execute(() -> {
            String text;
            try {
                text = page.dump(kind);
            } catch (RuntimeException ex) {
                text = null;
            }
            final String body = text == null || text.isEmpty() ? "(nothing to show)" : text;
            SwingUtilities.invokeLater(() -> {
                setStatus("");
                JTextArea area = new JTextArea(body);
                area.setEditable(false);
                area.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
                area.setCaretPosition(0);
                JDialog dialog = new JDialog(frame, label + " — " + view.url(), false);
                dialog.add(new JScrollPane(area));
                dialog.setSize(900, 640);
                dialog.setLocationRelativeTo(frame);
                dialog.setVisible(true);
            });
        });
    }

    private void savePage(boolean pdf) {
        if (view.url() == null) {
            return;
        }
        JFileChooser chooser = new JFileChooser();
        String name = view.title().isEmpty() ? "page"
            : view.title().replaceAll("[\\\\/:*?\"<>|]", "_");
        chooser.setSelectedFile(new File(name + (pdf ? ".pdf" : ".png")));
        chooser.setDialogTitle(pdf ? "Save page as PDF" : "Save page as PNG");
        if (chooser.showSaveDialog(frame) != JFileChooser.APPROVE_OPTION) {
            return;
        }
        final Path dest = chooser.getSelectedFile().toPath();
        setStatus("Saving…");
        io.execute(() -> {
            String outcome;
            try {
                Files.write(dest, pdf ? page.pdf(PdfOptions.a4()) : page.screenshot());
                outcome = "Saved " + dest;
            } catch (RuntimeException | IOException ex) {
                outcome = "Could not save page: " + ex.getMessage();
            }
            setStatusLater(outcome);
        });
    }

    /**
     * Save a download the page asked for. The file chooser starts in the
     * user's Downloads folder with the page's suggested name, or the last
     * segment of the URL.
     */
    private void startDownload(String url, String suggestedName) {
        String current = view.url();
        if (url.startsWith("file:") && (current == null || !current.startsWith("file:"))) {
            return;
        }
        JFileChooser chooser = new JFileChooser(downloadsDir());
        chooser.setSelectedFile(new File(chooser.getCurrentDirectory(),
                                         downloadFileName(url, suggestedName)));
        chooser.setDialogTitle("Save download");
        if (chooser.showSaveDialog(frame) != JFileChooser.APPROVE_OPTION) {
            return;
        }
        final Path dest;
        try {
            dest = chooser.getSelectedFile().toPath();
        } catch (InvalidPathException ex) {
            setStatus("Cannot save to " + chooser.getSelectedFile());
            return;
        }
        final String name = dest.getFileName().toString();
        setStatus("Downloading " + name + " …");
        io.execute(() -> {
            long id;
            try {
                id = page.startDownload(url);
            } catch (RuntimeException ex) {
                id = 0;
            }
            final long started = id;
            SwingUtilities.invokeLater(() -> {
                if (started <= 0) {
                    setStatus("Download failed: " + name);
                    return;
                }
                activeDownloads.add(started);
                pollDownloadLater(started, dest, name);
            });
        });
    }

    /**
     * The renderer fetches a download while its event loop runs, and the
     * loop runs only inside a request, so each poll first ticks the page.
     */
    private void pollDownloadLater(long id, Path dest, String name) {
        Timer timer = new Timer(DOWNLOAD_POLL_MS, e -> {
            if (closed) {
                return;
            }
            io.execute(() -> {
                Page.Download polled;
                try {
                    page.tick();
                    polled = page.pollDownload(id);
                } catch (RuntimeException ex) {
                    polled = null;
                }
                final Page.Download state = polled;
                SwingUtilities.invokeLater(() -> onDownloadPolled(id, dest, name, state));
            });
        });
        timer.setRepeats(false);
        timer.start();
    }

    private void onDownloadPolled(long id, Path dest, String name, Page.Download d) {
        if (closed) {
            return;
        }
        if (d != null && d.state() == Page.Download.State.RUNNING) {
            setStatus("Downloading " + name + " — " + formatBytes(d.bytes())
                + (d.total() > 0 ? " / " + formatBytes(d.total()) : ""));
            pollDownloadLater(id, dest, name);
            return;
        }
        activeDownloads.remove(id);
        if (d == null || d.state() != Page.Download.State.DONE || d.stagedFile() == null) {
            setStatus("Download failed: " + name
                + (d != null && d.error() != null ? " (" + d.error() + ")" : ""));
            return;
        }
        Thread mover = new Thread(() -> {
            String outcome;
            try {
                page.saveDownload(d, dest);
                outcome = "Downloaded " + dest;
            } catch (RuntimeException ex) {
                outcome = "Download failed: " + ex.getMessage();
            }
            setStatusLater(outcome);
        }, "ns-download-save");
        mover.setDaemon(true);
        mover.start();
    }

    private static String formatBytes(long bytes) {
        if (bytes < 1024) {
            return bytes + " B";
        }
        double value = bytes;
        String[] units = {"kB", "MB", "GB", "TB"};
        int unit = -1;
        do {
            value /= 1024.0;
            unit++;
        } while (value >= 1024.0 && unit < units.length - 1);
        return String.format("%.1f %s", value, units[unit]);
    }

    private static File downloadsDir() {
        File dir = new File(System.getProperty("user.home", "."), "Downloads");
        return dir.isDirectory() ? dir : new File(System.getProperty("user.home", "."));
    }

    /**
     * A safe file name: the page's suggestion without any directory part,
     * else the URL's last path segment, with characters the platform cannot
     * put in a file name replaced.
     */
    static String downloadFileName(String url, String suggestedName) {
        String name = suggestedName == null ? "" : suggestedName.strip();
        name = name.substring(Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
        if (name.isEmpty() || name.equals(".") || name.equals("..")) {
            String path;
            try {
                path = URI.create(url).getPath();
            } catch (IllegalArgumentException ex) {
                path = "";
            }
            path = path == null ? "" : path;
            name = path.substring(path.lastIndexOf('/') + 1);
        }
        name = name.replaceAll("[\\x00-\\x1f<>:\"|?*]", "_");
        try {
            Path.of(name);
        } catch (InvalidPathException ex) {
            name = name.replaceAll("[^\\x20-\\x7e]", "_");
        }
        return name.isEmpty() || name.equals(".") || name.equals("..") ? "download" : name;
    }

    private boolean askCamera(String origin) {
        int choice = JOptionPane.showOptionDialog(frame,
            "This page wants to use your camera on\n" + origin
            + ".\n\nAllowing lets the site capture video from this device for "
            + "the rest of the session.",
            "Share your camera with " + origin + "?",
            JOptionPane.YES_NO_OPTION, JOptionPane.WARNING_MESSAGE, null,
            new String[]{"Allow this site", "Block"}, "Block");
        return choice == 0;
    }

    /**
     * Answer the capability requests a page is suspended on. Each kind and
     * origin is asked about once per window; the same answer is repeated to
     * the engine when the page asks again.
     */
    private void promptCapabilities(List<CapabilityRequest> requests) {
        for (CapabilityRequest request : requests) {
            String key = request.kind() + " " + request.origin();
            Boolean known = capabilityAnswers.get(key);
            boolean allow = known != null ? known
                : request.kind().equals("camera") ? askCamera(request.origin())
                : askCapability(request.kind(), request.origin());
            capabilityAnswers.put(key, allow);
            view.resolveCapability(request, allow);
        }
    }

    private boolean askCapability(String kind, String origin) {
        String what = switch (kind) {
            case "popups" -> "open a new window";
            case "fullscreen" -> "take the whole screen";
            case "pointer-lock" -> "capture the mouse pointer";
            case "downloads" -> "download a file";
            case "webgl" -> "draw with WebGL";
            default -> "use " + kind;
        };
        return JOptionPane.showConfirmDialog(frame,
            origin + "\nwants to " + what + ".", "Allow this site?",
            JOptionPane.YES_NO_OPTION, JOptionPane.QUESTION_MESSAGE) == JOptionPane.YES_OPTION;
    }

    /**
     * Open the window an {@code open} asks for, and carry every other
     * operation to the window at the far end of its link, checking that
     * this window is the end allowed to send it.
     */
    private void handlePopupOps(List<Map<String, String>> ops) {
        for (Map<String, String> op : ops) {
            String kind = op.getOrDefault("op", "");
            if (kind.equals("open")) {
                openPopup(op);
                continue;
            }
            PopupLink link = POPUP_LINKS.get(op.get("token"));
            if (link == null || (link.opener != this && link.popup != this)) {
                continue;
            }
            boolean fromPopup = link.popup == this;
            boolean toPopup = "popup".equals(op.get("to"));
            Browser target = switch (kind) {
                case "post" -> toPopup == fromPopup ? null : toPopup ? link.popup : link.opener;
                case "call" -> fromPopup ? link.opener : null;
                case "port" -> fromPopup ? link.opener : link.popup;
                default -> null;
            };
            if (kind.equals("close")) {
                link.popup.closeWindow();
            } else if (target != null && !target.closed) {
                target.view.deliverPopupOperation(kind.equals("post") ? "message" : kind, op);
            }
        }
    }

    private void openPopup(Map<String, String> op) {
        String url = op.get("url");
        String token = op.get("token");
        if (url == null || url.isEmpty()) {
            return;
        }
        boolean linked = token != null && !token.isEmpty() && !POPUP_LINKS.containsKey(token);
        Browser popup = openWindow(url, privateMode, linked ? token : null,
                                   linked ? op.get("origin") : null);
        if (linked) {
            POPUP_LINKS.put(token, new PopupLink(token, this, popup));
        }
    }

    /** Tell openers their popup is gone, and forget the links this window was part of. */
    private void unlinkPopups() {
        for (var it = POPUP_LINKS.values().iterator(); it.hasNext();) {
            PopupLink link = it.next();
            if (link.popup == this) {
                if (link.opener != null && !link.opener.closed) {
                    link.opener.view.deliverPopupOperation("closed",
                                                           Map.of("token", link.token));
                }
                it.remove();
            } else if (link.opener == this) {
                link.opener = null;
            }
        }
    }

    private void launchMedia(Page.Media media) {
        for (String player : new String[]{"mpv", "vlc"}) {
            try {
                new ProcessBuilder(player, media.url()).inheritIO().start();
                setStatus("Opening " + (media.video() ? "video" : "audio")
                    + " in external player…");
                return;
            } catch (IOException notInstalled) {
                continue;
            }
        }
        try {
            if (Desktop.isDesktopSupported()
                    && Desktop.getDesktop().isSupported(Desktop.Action.BROWSE)) {
                Desktop.getDesktop().browse(URI.create(media.url()));
                setStatus("Opened media in the system handler");
                return;
            }
        } catch (IOException | RuntimeException ex) {
            setStatus("Could not open the media: " + ex.getMessage());
            return;
        }
        setStatus("No external media player found (install mpv or vlc)");
    }

    private void toggleConsole() {
        if (console == null) {
            buildConsole();
        }
        if (console.isVisible()) {
            console.setVisible(false);
            consolePoll.stop();
            return;
        }
        console.setVisible(true);
        consoleIn.requestFocusInWindow();
        consolePoll.start();
    }

    private void buildConsole() {
        console = new JDialog(frame, "JavaScript Console", false);
        console.setSize(640, 320);
        console.setLocationRelativeTo(frame);
        consoleOut = new JTextArea();
        consoleOut.setEditable(false);
        consoleOut.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
        consoleOut.setLineWrap(true);
        consoleOut.setWrapStyleWord(true);
        consoleIn = new JTextField();
        consoleIn.setToolTipText("Evaluate JavaScript and press Enter");
        consoleIn.addActionListener(e -> evaluateInConsole());
        JButton clear = new JButton("Clear");
        clear.setFocusable(false);
        clear.addActionListener(e -> consoleOut.setText(""));
        JPanel header = new JPanel(new FlowLayout(FlowLayout.RIGHT, 4, 2));
        header.add(clear);
        console.add(header, BorderLayout.NORTH);
        console.add(new JScrollPane(consoleOut), BorderLayout.CENTER);
        console.add(consoleIn, BorderLayout.SOUTH);
        consolePoll = new Timer(CONSOLE_POLL_MS, e -> pollConsole());
    }

    private void evaluateInConsole() {
        String source = consoleIn.getText();
        if (source.isEmpty() || view.url() == null) {
            return;
        }
        appendConsole("> " + source + "\n");
        consoleIn.setText("");
        io.execute(() -> {
            String result;
            try {
                result = page.evaluate(source);
            } catch (RuntimeException ex) {
                result = "error: " + ex.getMessage();
            }
            final String shown = result == null ? "undefined" : result;
            SwingUtilities.invokeLater(() -> {
                appendConsole(shown + "\n");
                view.refresh();
            });
        });
    }

    private void pollConsole() {
        if (!console.isVisible() || view.url() == null || closed) {
            return;
        }
        io.execute(() -> {
            String log;
            try {
                log = page.consoleOutput();
            } catch (RuntimeException ex) {
                return;
            }
            if (log != null) {
                SwingUtilities.invokeLater(() -> appendConsole(log));
            }
        });
    }

    private void appendConsole(String text) {
        if (text == null || text.isEmpty()) {
            return;
        }
        consoleOut.append(text);
        consoleOut.setCaretPosition(consoleOut.getDocument().getLength());
    }

    private void copyText(String text, String message) {
        if (text == null) {
            return;
        }
        Toolkit.getDefaultToolkit().getSystemClipboard()
            .setContents(new StringSelection(text), null);
        setStatus(message);
    }

    private static boolean clipboardHasText() {
        try {
            Object data = Toolkit.getDefaultToolkit().getSystemClipboard()
                .getData(DataFlavor.stringFlavor);
            return data instanceof String text && !text.isEmpty();
        } catch (UnsupportedFlavorException | IOException | IllegalStateException e) {
            return false;
        }
    }

    /**
     * Follow a page's fullscreen request: hide the toolbar, find bar and
     * status bar and take the whole screen.
     */
    private void enterPageFullscreen() {
        if (pageFullscreen) {
            return;
        }
        pageFullscreen = true;
        closeFind();
        toolbar.setVisible(false);
        south.setVisible(false);
        GraphicsDevice screen = frame.getGraphicsConfiguration().getDevice();
        if (screen.isFullScreenSupported()) {
            screen.setFullScreenWindow(frame);
        } else {
            frame.setExtendedState(frame.getExtendedState() | JFrame.MAXIMIZED_BOTH);
        }
        view.focusPage();
        setStatus("Full screen — press Esc to exit");
    }

    /**
     * Restore the window from page fullscreen; {@code tellPage} also asks
     * the page to leave it, as when the user pressed {@code Esc}.
     */
    private void leavePageFullscreen(boolean tellPage) {
        if (!pageFullscreen) {
            return;
        }
        pageFullscreen = false;
        GraphicsDevice screen = frame.getGraphicsConfiguration().getDevice();
        if (screen.getFullScreenWindow() == frame) {
            screen.setFullScreenWindow(null);
        }
        toolbar.setVisible(true);
        south.setVisible(true);
        frame.getRootPane().revalidate();
        if (tellPage) {
            view.exitPageFullscreen();
        }
    }

    private static String resolveVersion() {
        String version = Browser.class.getPackage().getImplementationVersion();
        if (version != null && !version.isBlank()) {
            return version;
        }
        try (InputStream in = Browser.class.getResourceAsStream("version.properties")) {
            Properties props = new Properties();
            if (in != null) {
                props.load(in);
            }
            String fromFile = props.getProperty("version", "");
            return fromFile.isBlank() ? "dev" : fromFile;
        } catch (IOException e) {
            return "dev";
        }
    }

    private static Image loadLogo() {
        ImageIcon logo = icon("logo");
        return logo != null ? logo.getImage() : null;
    }

    private static ImageIcon icon(String name) {
        URL resource = Browser.class.getResource("icons/" + name + ".png");
        return resource != null ? new ImageIcon(resource) : null;
    }

    private static JButton navButton(String iconName, String fallbackText, String tooltip) {
        ImageIcon image = icon(iconName);
        JButton button = image != null ? new JButton(image) : new JButton(fallbackText);
        button.setToolTipText(tooltip);
        button.setFocusable(false);
        return button;
    }

    /**
     * Turn what was typed in the address bar (or given on the command line)
     * into a URL, the way the GTK shell does: URLs pass through, an existing
     * local path becomes a {@code file:} URL, anything with a space or no
     * dot or colon is a search, and the rest is a host name reached over
     * HTTPS.
     */
    static String normalize(String input) {
        String s = input.strip();
        if (s.isEmpty() || s.contains("://") || s.startsWith("about:") || s.startsWith("data:")
                || s.startsWith("file:") || s.startsWith("view-source:")) {
            return s;
        }
        String local = localFileUrl(s);
        if (local != null) {
            return local;
        }
        if (s.indexOf(' ') >= 0 || s.indexOf('\t') >= 0 || s.indexOf('　') >= 0) {
            return searchUrl(s);
        }
        boolean localhost = s.startsWith("localhost")
            && (s.length() == 9 || ":/".indexOf(s.charAt(9)) >= 0);
        if (!localhost && s.indexOf('.') < 0 && s.indexOf(':') < 0) {
            return searchUrl(s);
        }
        return "https://" + s;
    }

    private static String searchUrl(String query) {
        return SEARCH_URL + URLEncoder.encode(query, StandardCharsets.UTF_8).replace("+", "%20");
    }

    private static String localFileUrl(String path) {
        try {
            Path p = Path.of(path);
            return Files.exists(p) ? p.toAbsolutePath().normalize().toUri().toString() : null;
        } catch (InvalidPathException | SecurityException e) {
            return null;
        }
    }
}

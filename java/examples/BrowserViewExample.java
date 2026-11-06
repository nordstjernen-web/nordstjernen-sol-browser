/* Nordstjernen — a web page embedded in a Swing window.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

import org.nordstjernen.browser.BrowserContext;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.swing.BrowserView;

import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JTextField;
import javax.swing.SwingUtilities;
import java.awt.BorderLayout;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;

/**
 * An address field, a {@link BrowserView} and a status line: the least an
 * application needs around a page. Run it with a URL, or without one for
 * {@code https://example.com}.
 */
public final class BrowserViewExample {

    private BrowserViewExample() {
    }

    public static void main(String[] args) {
        String start = args.length > 0 ? args[0] : "https://example.com";
        SwingUtilities.invokeLater(() -> open(start));
    }

    private static void open(String start) {
        Engine engine = Engine.launch();
        BrowserContext context = engine.newContext();
        BrowserView view = new BrowserView(context.newPage());

        JFrame frame = new JFrame("BrowserView");
        JTextField address = new JTextField(start);
        JLabel status = new JLabel(" ");
        address.addActionListener(e -> view.navigate(address.getText()));
        view.addListener(new BrowserView.Listener() {
            @Override
            public void titleChanged(String title) {
                frame.setTitle(title.isEmpty() ? "BrowserView" : title);
            }

            @Override
            public void urlChanged(String url) {
                address.setText(url);
            }

            @Override
            public void statusChanged(String text) {
                status.setText(text.isEmpty() ? " " : text);
            }
        });

        frame.add(address, BorderLayout.NORTH);
        frame.add(view, BorderLayout.CENTER);
        frame.add(status, BorderLayout.SOUTH);
        frame.setSize(1000, 720);
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                view.close();
                view.pageExecutor().execute(() -> {
                    context.close();
                    engine.close();
                });
                frame.dispose();
            }
        });
        frame.setVisible(true);
        view.navigate(start);
    }
}

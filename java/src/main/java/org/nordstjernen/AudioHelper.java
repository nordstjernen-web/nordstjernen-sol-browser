/* Nordstjernen — plays a renderer session's audio through nordstjernen-audio.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen;

import org.nordstjernen.internal.EngineDirs;
import org.nordstjernen.internal.RendererProcess;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

/**
 * Plays a page's sound the way the GTK shell does. The renderer reports media
 * commands ({@code open}, {@code play}, {@code pause}, {@code seek},
 * {@code volume}, …) on {@link RemoteBrowser.Frame#audio} and
 * {@link RemoteBrowser.Tick#audio}; {@link #send} forwards them, one per line,
 * to the {@code nordstjernen-audio} helper that ships beside the renderer,
 * which decodes and outputs the sound itself. The helper starts on the first
 * command and {@link #stop} silences and ends it.
 *
 * <p>An {@code open} is forwarded only for an {@code http(s):} or
 * {@code data:} source, or a {@code file:} source when the page itself is
 * local or the renderer staged the stream in its {@code msaudio} cache.
 * Commands for the video helper are ignored. Not thread-safe.
 */
public final class AudioHelper implements AutoCloseable {

    private static final int LINE_MAX = 4096;

    private Process process;
    private OutputStream toHelper;
    private boolean unavailable;

    /**
     * Forward the commands in a {@code Frame.audio} / {@code Tick.audio} value
     * for a page at {@code pageUrl}. Returns false when there is no helper to
     * play them.
     */
    public boolean send(String audio, String pageUrl) {
        return send(splitCommands(audio), pageUrl);
    }

    /**
     * Forward the media commands a page raised ({@code PageEvents.audio()})
     * for a page at {@code pageUrl}. Returns false when there is no helper to
     * play them.
     */
    public boolean send(java.util.List<String> commands, String pageUrl) {
        boolean sent = false;
        for (String cmd : commands) {
            if (cmd.startsWith("video ") || !commandAllowed(cmd, pageUrl)) {
                continue;
            }
            if (!ensureStarted()) {
                return false;
            }
            try {
                toHelper.write((cmd + "\n").getBytes(StandardCharsets.UTF_8));
                sent = true;
            } catch (IOException e) {
                stop();
                return false;
            }
        }
        if (sent) {
            try {
                toHelper.flush();
            } catch (IOException e) {
                stop();
                return false;
            }
        }
        return true;
    }

    private static java.util.List<String> splitCommands(String audio) {
        java.util.List<String> out = new java.util.ArrayList<>();
        if (audio != null) {
            for (String command : audio.split("001f")) {
                if (!command.isEmpty()) {
                    out.add(command);
                }
            }
        }
        return out;
    }

    /** Silence the page and end the helper; the next command starts a fresh one. */
    public void stop() {
        if (process == null) {
            return;
        }
        try {
            toHelper.write("quit\n".getBytes(StandardCharsets.US_ASCII));
            toHelper.flush();
        } catch (IOException ignored) {
        }
        try {
            toHelper.close();
        } catch (IOException ignored) {
        }
        process.destroy();
        process = null;
        toHelper = null;
    }

    @Override
    public void close() {
        stop();
    }

    private boolean ensureStarted() {
        if (process != null && process.isAlive()) {
            return true;
        }
        process = null;
        if (unavailable) {
            return false;
        }
        String exe = RendererProcess.locateHelper("nordstjernen-audio");
        if (exe == null) {
            unavailable = true;
            return false;
        }
        ProcessBuilder pb = new ProcessBuilder(exe);
        pb.redirectError(ProcessBuilder.Redirect.DISCARD);
        try {
            process = pb.start();
        } catch (IOException e) {
            unavailable = true;
            return false;
        }
        toHelper = process.getOutputStream();
        Thread feedback = new Thread(() -> drainFeedback(process),
                                     "ns-audio-feedback");
        feedback.setDaemon(true);
        feedback.start();
        return true;
    }

    /** Keep the helper's status pipe flowing, surfacing only its errors. */
    private static void drainFeedback(Process helper) {
        try (BufferedReader in = new BufferedReader(new InputStreamReader(
                helper.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                if (line.startsWith("error ")) {
                    System.err.println("[audio-helper] " + line);
                }
            }
        } catch (IOException ignored) {
        }
    }

    static boolean commandAllowed(String cmd, String pageUrl) {
        if (cmd.length() >= LINE_MAX - 1 || cmd.indexOf('\r') >= 0
            || cmd.indexOf('\n') >= 0) {
            return false;
        }
        String url = openedUrl(cmd);
        return url == null || urlAllowed(url, pageUrl);
    }

    /** The source URL of an {@code open <id> <url>} / {@code reload <id> <url>} command, else null. */
    private static String openedUrl(String cmd) {
        String[] parts = cmd.strip().split("[ \t]+", 3);
        if (!parts[0].equals("open") && !parts[0].equals("reload")) {
            return null;
        }
        return parts.length < 3 ? "" : parts[2];
    }

    private static boolean urlAllowed(String url, String pageUrl) {
        if (url.startsWith("http://") || url.startsWith("https://")
            || url.startsWith("data:")) {
            return true;
        }
        if (!url.startsWith("file://")) {
            return false;
        }
        if (pageUrl != null && pageUrl.startsWith("file:")) {
            return true;
        }
        try {
            Path stream = Path.of(URI.create(url)).toAbsolutePath().normalize();
            for (Path staging : EngineDirs.cacheSubdirs("msaudio")) {
                if (stream.startsWith(staging)) {
                    return true;
                }
            }
            return false;
        } catch (RuntimeException e) {
            return false;
        }
    }
}

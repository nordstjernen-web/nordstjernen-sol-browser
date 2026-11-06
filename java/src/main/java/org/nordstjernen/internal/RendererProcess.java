/* Nordstjernen — pure-Java client of the renderer process protocol.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.internal;

import org.nordstjernen.NordstjernenException;
import org.nordstjernen.RendererTimeoutException;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URISyntaxException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.security.CodeSource;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Spawns and drives a {@code nordstjernen-renderer} child process over its
 * stdio control channel, speaking the same HTTP/JSON protocol the GTK shell
 * uses. All rendering happens in the separate renderer process, so the JVM
 * never loads the native engine.
 *
 * <p>The renderer is checked with the protocol handshake when it starts, and
 * every request carries a deadline: a renderer that does not answer in time is
 * stopped and the request fails with {@link RendererTimeoutException}, so a
 * wedged renderer can never hang its caller. Requests are serialised, so an
 * instance may be shared between threads.
 */
public final class RendererProcess implements AutoCloseable {

    /** The engine protocol version this client speaks ({@code src/engine_protocol.h}). */
    public static final int PROTOCOL_VERSION = 1;

    /** The request deadline when none is given: the {@code nordstjernen.timeout.ms} property, else two minutes. */
    public static final long DEFAULT_TIMEOUT_MS =
        Math.max(1000L, Long.getLong("nordstjernen.timeout.ms", 120_000L));

    private static final long HELLO_TIMEOUT_MS = 30_000L;
    private static final long QUIT_TIMEOUT_MS = 2_000L;
    private static final long EXIT_GRACE_MS = 2_000L;
    private static final int MAX_HEADER_LINE = 1 << 20;
    private static final int MAX_HEADERS = 256;
    private static final long REPLY_SLACK_BYTES = 16L << 20;
    private static final int SANDBOX_UNAVAILABLE_EXIT = 78;
    private static final int WINDOWS_DLL_NOT_FOUND_EXIT = 0xC0000135;

    private static final ScheduledThreadPoolExecutor WATCHDOG = newWatchdog();
    private static final Set<Process> LIVE = ConcurrentHashMap.newKeySet();

    static {
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            for (Process p : LIVE) {
                p.destroyForcibly();
            }
        }, "ns-renderer-reaper"));
    }

    /** A parsed renderer response: status code, headers (lower-cased keys), body. */
    public static final class Response {
        public final int status;
        public final Map<String, String> headers;
        public final byte[] body;

        Response(int status, Map<String, String> headers, byte[] body) {
            this.status = status;
            this.headers = headers;
            this.body = body;
        }

        public String header(String name) {
            return headers.get(name.toLowerCase(Locale.ROOT));
        }

        public String text() {
            return new String(body, StandardCharsets.UTF_8);
        }
    }

    private final String executable;
    private final Process process;
    private final OutputStream toRenderer;
    private final InputStream fromRenderer;
    private final long maxReplyBytes;
    private volatile long timeoutMs = DEFAULT_TIMEOUT_MS;
    private volatile boolean dead;
    private String engineVersion = "";

    public RendererProcess(int maxWidth, int maxHeight) {
        this(maxWidth, maxHeight, false);
    }

    /**
     * @param privateMode launch the renderer in private/incognito mode, where
     *                    cookies, cache and storage are ephemeral
     * @throws NordstjernenException when no renderer is found, it does not
     *                               start, or it speaks another protocol version
     */
    public RendererProcess(int maxWidth, int maxHeight, boolean privateMode) {
        this(null, maxWidth, maxHeight, privateMode, Map.of());
    }

    /**
     * @param executable  the renderer to run, or null to {@linkplain #locateRenderer() look it up}
     * @param environment variables to add to the renderer's environment, such
     *                    as the {@code XDG_*_HOME} directories that select a profile
     */
    public RendererProcess(String executable, int maxWidth, int maxHeight, boolean privateMode,
                           Map<String, String> environment) {
        this.executable = executable == null || executable.isEmpty()
            ? locateRenderer() : new File(executable).getAbsolutePath();
        this.maxReplyBytes = Math.min(Integer.MAX_VALUE - 8L,
            (long) maxWidth * (long) maxHeight * 4L + REPLY_SLACK_BYTES);
        List<String> argv = new ArrayList<>(List.of(
            this.executable, Integer.toString(maxWidth), Integer.toString(maxHeight), "stdio"));
        if (privateMode) {
            argv.add("private");
        }
        ProcessBuilder pb = new ProcessBuilder(argv);
        pb.environment().putAll(environment);
        pb.redirectError(ProcessBuilder.Redirect.INHERIT);
        try {
            this.process = pb.start();
        } catch (IOException e) {
            throw new NordstjernenException("failed to start renderer: " + this.executable, e);
        }
        LIVE.add(process);
        this.toRenderer = process.getOutputStream();
        this.fromRenderer = new BufferedInputStream(process.getInputStream());
        try {
            hello();
        } catch (RuntimeException e) {
            close();
            throw e;
        }
    }

    /** The renderer executable this instance runs, as an absolute path. */
    public String executable() {
        return executable;
    }

    /** The engine version the renderer reported in the handshake, e.g. {@code 1.0.45}. */
    public String engineVersion() {
        return engineVersion;
    }

    /** Whether the renderer is still running and has not failed a request. */
    public boolean isAlive() {
        return !dead && process.isAlive();
    }

    /** The deadline of requests sent without an explicit one, in milliseconds. */
    public long timeoutMs() {
        return timeoutMs;
    }

    public void setTimeoutMs(long timeoutMs) {
        this.timeoutMs = Math.max(1L, timeoutMs);
    }

    /**
     * The renderer executable: the {@code nordstjernen.renderer} system
     * property, else {@code NORDSTJERNEN_RENDERER}, else the first
     * {@code nordstjernen-renderer} beside this library's jar, in a
     * {@code builddir/src/} of a source checkout the jar was built in, or in a
     * directory on {@code PATH}. The working directory is never searched, so
     * a renderer planted next to a document cannot be picked up.
     *
     * @throws NordstjernenException when there is none
     */
    public static String locateRenderer() {
        String configured = System.getProperty("nordstjernen.renderer",
            System.getenv("NORDSTJERNEN_RENDERER"));
        if (configured != null && !configured.isEmpty()) {
            return new File(configured).getAbsolutePath();
        }
        String name = executableName("nordstjernen-renderer");
        Set<File> dirs = searchDirs();
        for (File dir : dirs) {
            File candidate = new File(dir, name);
            if (candidate.isFile()) {
                return candidate.getAbsolutePath();
            }
        }
        throw new NordstjernenException(name + " not found; set -Dnordstjernen.renderer=<path>"
            + " or NORDSTJERNEN_RENDERER, or put it on PATH (searched " + dirs.size()
            + " directories)");
    }

    /**
     * A helper executable shipped beside the renderer (such as
     * {@code nordstjernen-audio}), or null when the renderer's directory does
     * not hold one.
     */
    public static String locateHelper(String baseName) {
        File dir;
        try {
            dir = new File(locateRenderer()).getParentFile();
        } catch (NordstjernenException e) {
            return null;
        }
        if (dir == null) {
            return null;
        }
        File helper = new File(dir, executableName(baseName));
        return helper.isFile() && helper.canExecute() ? helper.getAbsolutePath() : null;
    }

    private static Set<File> searchDirs() {
        Set<File> dirs = new LinkedHashSet<>();
        File home = codeSourceDir();
        if (home != null) {
            dirs.add(home);
            File up = home;
            for (int i = 0; i < 6 && up != null; i++, up = up.getParentFile()) {
                dirs.add(new File(up, "builddir" + File.separator + "src"));
            }
        }
        String path = System.getenv("PATH");
        if (path != null) {
            for (String entry : path.split(File.pathSeparator)) {
                File dir = new File(entry);
                if (!entry.isEmpty() && dir.isAbsolute()) {
                    dirs.add(dir);
                }
            }
        }
        return dirs;
    }

    private static File codeSourceDir() {
        try {
            CodeSource source = RendererProcess.class.getProtectionDomain().getCodeSource();
            URL location = source == null ? null : source.getLocation();
            if (location == null || !"file".equals(location.getProtocol())) {
                return null;
            }
            File file = Path.of(location.toURI()).toFile().getAbsoluteFile();
            return file.isDirectory() ? file : file.getParentFile();
        } catch (URISyntaxException | RuntimeException e) {
            return null;
        }
    }

    private static String executableName(String baseName) {
        boolean windows = System.getProperty("os.name", "")
            .toLowerCase(Locale.ROOT).startsWith("windows");
        return windows ? baseName + ".exe" : baseName;
    }

    /**
     * Send a request and read the full response, within the default deadline.
     *
     * @throws RendererTimeoutException when the renderer did not answer in time
     * @throws NordstjernenException    when the renderer is gone, the reply is
     *                                  malformed, or the status is not 200
     */
    public Response request(String method, String path, String jsonBody) {
        return request(path, jsonBody, timeoutMs);
    }

    /** {@link #request(String, String, String)} with an explicit deadline in milliseconds. */
    public Response request(String path, String jsonBody, long deadlineMs) {
        Response response = exchange(path, jsonBody, deadlineMs);
        if (response.status != 200) {
            throw new NordstjernenException("renderer answered " + response.status
                + " to " + path + (response.status == 404
                    ? " (engine " + engineVersion + " does not know this request)" : ""));
        }
        return response;
    }

    /**
     * Send a request and return the response whatever its status, so a caller
     * can probe for a request a renderer may not know (it answers 404).
     */
    public synchronized Response exchange(String path, String jsonBody, long deadlineMs) {
        if (dead || !process.isAlive()) {
            dead = true;
            throw new NordstjernenException("renderer " + executable + " is not running"
                + exitDetail());
        }
        byte[] body = jsonBody == null
            ? new byte[0] : jsonBody.getBytes(StandardCharsets.UTF_8);
        String head = "POST " + path + " HTTP/1.1\r\n"
            + "Content-Type: application/json\r\n"
            + "Content-Length: " + body.length + "\r\n\r\n";
        AtomicBoolean expired = new AtomicBoolean();
        ScheduledFuture<?> deadline = WATCHDOG.schedule(() -> {
            expired.set(true);
            process.destroyForcibly();
        }, Math.max(1L, deadlineMs), TimeUnit.MILLISECONDS);
        try {
            toRenderer.write(head.getBytes(StandardCharsets.US_ASCII));
            if (body.length > 0) {
                toRenderer.write(body);
            }
            toRenderer.flush();
            return readResponse();
        } catch (IOException e) {
            dead = true;
            if (expired.get()) {
                throw new RendererTimeoutException("renderer did not answer " + path
                    + " within " + deadlineMs + " ms and was stopped", e);
            }
            String detail = exitDetail();
            process.destroyForcibly();
            throw new NordstjernenException("renderer IPC failed on " + path
                + ": " + e.getMessage() + detail, e);
        } finally {
            deadline.cancel(false);
            if (expired.get()) {
                dead = true;
            }
        }
    }

    private void hello() {
        Response response = exchange("/hello", "", HELLO_TIMEOUT_MS);
        String json = response.text();
        int protocol = response.status == 200 ? Json.integer(json, "protocol", -1) : -1;
        if (protocol != PROTOCOL_VERSION) {
            throw new NordstjernenException("renderer " + executable + " speaks engine protocol "
                + (protocol < 0 ? "(none: it did not answer /hello)" : Integer.toString(protocol))
                + ", this library needs " + PROTOCOL_VERSION
                + "; use the renderer and the jar of the same release");
        }
        engineVersion = Json.string(json, "version");
    }

    private Response readResponse() throws IOException {
        String statusLine = readLine();
        if (statusLine == null) {
            throw new IOException("the renderer closed the connection");
        }
        String[] parts = statusLine.split(" ");
        int status;
        try {
            status = parts.length >= 2 && parts[0].startsWith("HTTP/")
                ? Integer.parseInt(parts[1]) : -1;
        } catch (NumberFormatException e) {
            status = -1;
        }
        if (status < 100 || status > 599) {
            throw new IOException("malformed status line from the renderer");
        }
        Map<String, String> headers = new HashMap<>();
        String line;
        while ((line = readLine()) != null && !line.isEmpty()) {
            if (headers.size() >= MAX_HEADERS) {
                throw new IOException("too many headers from the renderer");
            }
            int colon = line.indexOf(':');
            if (colon > 0) {
                headers.put(line.substring(0, colon).trim().toLowerCase(Locale.ROOT),
                            line.substring(colon + 1).trim());
            }
        }
        if (line == null) {
            throw new IOException("the renderer closed the connection mid-response");
        }
        long length = 0;
        String declared = headers.get("content-length");
        if (declared != null) {
            try {
                length = Long.parseLong(declared);
            } catch (NumberFormatException e) {
                length = -1;
            }
            if (length < 0 || length > maxReplyBytes) {
                throw new IOException("the renderer declared a body of " + declared
                    + " bytes; at most " + maxReplyBytes + " are accepted");
            }
        }
        byte[] body = new byte[(int) length];
        int off = 0;
        while (off < body.length) {
            int n = fromRenderer.read(body, off, body.length - off);
            if (n < 0) {
                throw new IOException("the renderer truncated the response body");
            }
            off += n;
        }
        return new Response(status, headers, body);
    }

    /**
     * One CRLF- or LF-terminated header line, decoded as UTF-8: side-channel
     * headers such as {@code X-Download} carry page-supplied file names.
     */
    private String readLine() throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream(64);
        int c;
        while ((c = fromRenderer.read()) != -1) {
            if (c == '\r') {
                int next = fromRenderer.read();
                if (next == '\n' || next == -1) {
                    return line.toString(StandardCharsets.UTF_8);
                }
                line.write('\r');
                line.write(next);
            } else if (c == '\n') {
                return line.toString(StandardCharsets.UTF_8);
            } else {
                line.write(c);
            }
            if (line.size() > MAX_HEADER_LINE) {
                throw new IOException("header line from the renderer exceeds "
                    + MAX_HEADER_LINE + " bytes");
            }
        }
        return line.size() == 0 ? null : line.toString(StandardCharsets.UTF_8);
    }

    private String exitDetail() {
        try {
            if (!process.waitFor(500, TimeUnit.MILLISECONDS)) {
                return "";
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "";
        }
        int code = process.exitValue();
        if (code == SANDBOX_UNAVAILABLE_EXIT) {
            return " (exit code 78: the renderer could not enter its sandbox; on Linux it"
                + " needs Landlock, kernel 5.13 or newer, or NS_NO_SANDBOX=1 to run without)";
        }
        if (code == WINDOWS_DLL_NOT_FOUND_EXIT) {
            return " (exit code 0xC0000135: a DLL the renderer needs was not found; its"
                + " shared libraries must be beside it or on PATH)";
        }
        return " (exit code " + code + ")";
    }

    /**
     * Ask the renderer to quit and wait briefly for it; one that does not
     * comply is killed. Never blocks for more than a few seconds.
     */
    @Override
    public synchronized void close() {
        if (!dead && process.isAlive()) {
            try {
                exchange("/quit", "", QUIT_TIMEOUT_MS);
            } catch (RuntimeException ignored) {
                dead = true;
            }
        }
        dead = true;
        closeQuietly(toRenderer);
        closeQuietly(fromRenderer);
        try {
            if (!process.waitFor(EXIT_GRACE_MS, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                process.waitFor(EXIT_GRACE_MS, TimeUnit.MILLISECONDS);
            }
        } catch (InterruptedException e) {
            process.destroyForcibly();
            Thread.currentThread().interrupt();
        }
        LIVE.remove(process);
    }

    private static void closeQuietly(Closeable stream) {
        try {
            stream.close();
        } catch (IOException ignored) {
            return;
        }
    }

    private static ScheduledThreadPoolExecutor newWatchdog() {
        ScheduledThreadPoolExecutor pool = new ScheduledThreadPoolExecutor(1, r -> {
            Thread t = new Thread(r, "ns-renderer-watchdog");
            t.setDaemon(true);
            return t;
        });
        pool.setRemoveOnCancelPolicy(true);
        return pool;
    }
}

/* Nordstjernen — a Model Context Protocol server over stdio for the agent browser.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.agent;

import org.nordstjernen.browser.ContextOptions;
import org.nordstjernen.browser.Engine;
import org.nordstjernen.browser.ResourcePolicy;
import org.nordstjernen.internal.Json;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;

/**
 * Serves {@link BrowserTools} to an MCP client: JSON-RPC 2.0 messages, one
 * per line, on standard input and output. Run it as
 *
 * <pre>
 * java -cp nordstjernen.jar org.nordstjernen.agent.McpServer [options]
 * </pre>
 *
 * and point a client's server configuration at that command. The browser it
 * drives is headless and its tabs' renderer processes start with the first
 * navigation. {@code --help} lists the options.
 */
public final class McpServer {

    private static final List<String> PROTOCOL_VERSIONS =
        List.of("2025-06-18", "2025-03-26", "2024-11-05");
    private static final int MAX_LINE = 16 * 1024 * 1024;

    private static final String USAGE = """
        Usage: java -cp nordstjernen.jar org.nordstjernen.agent.McpServer [options]

        A Model Context Protocol server on stdin/stdout that drives a headless
        Nordstjernen browser.

          --profile DIR        keep cookies, storage and cache in DIR between runs
          --user-profile       use this user's own Nordstjernen profile
          --in-memory          keep nothing on disk (the default is a profile that
                               is deleted on exit)
          --viewport WxH       viewport of the tabs in CSS pixels (default 1280x800)
          --downloads DIR      save files pages offer, and PDFs, in DIR
          --policy NAME        all (default), public-web (no local files, no
                               private or loopback addresses) or none (no network)
          --allow-host HOST    only reach hosts matching HOST; repeatable;
                               *.example.com matches subdomains
          --deny-host HOST     never reach hosts matching HOST; repeatable
          --user-agent TEXT    the User-Agent to send
          --proxy URL          send HTTP and HTTPS through this proxy
          --renderer PATH      the nordstjernen-renderer executable to run
          --help               show this text
        """;

    private final BrowserTools tools;
    private final BufferedReader in;
    private final PrintStream out;

    public McpServer(BrowserTools tools, InputStream in, OutputStream out) {
        this.tools = tools;
        this.in = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
        this.out = new PrintStream(out, false, StandardCharsets.UTF_8);
    }

    /** Answer requests until the input ends. */
    public void run() throws IOException {
        for (String line = in.readLine(); line != null; line = in.readLine()) {
            if (line.isBlank()) {
                continue;
            }
            String reply = line.length() > MAX_LINE ? error(null, -32600, "request too large")
                                                    : handle(line);
            if (reply != null) {
                out.print(reply);
                out.print('\n');
                out.flush();
            }
        }
    }

    private String handle(String line) {
        Object parsed;
        try {
            parsed = Json.parse(line);
        } catch (IllegalArgumentException e) {
            return error(null, -32700, "parse error");
        }
        if (!(parsed instanceof Map<?, ?> message)) {
            return error(null, -32600, "a request must be an object");
        }
        Object id = message.get("id");
        if (!(message.get("method") instanceof String method) || !message.containsKey("id")) {
            return null;
        }
        Map<?, ?> params = message.get("params") instanceof Map<?, ?> map ? map : Map.of();
        return switch (method) {
            case "initialize" -> result(id, initialize(params));
            case "ping" -> result(id, Map.of());
            case "tools/list" -> result(id, Map.of("tools", toolList()));
            case "tools/call" -> callTool(id, params);
            case "resources/list" -> result(id, Map.of("resources", List.of()));
            case "prompts/list" -> result(id, Map.of("prompts", List.of()));
            default -> error(id, -32601, "method not found");
        };
    }

    private Map<String, Object> initialize(Map<?, ?> params) {
        Object asked = params.get("protocolVersion");
        String version = PROTOCOL_VERSIONS.contains(asked) ? (String) asked
                                                           : PROTOCOL_VERSIONS.get(0);
        Map<String, Object> info = new LinkedHashMap<>();
        info.put("name", "nordstjernen-java");
        info.put("title", "Nordstjernen Web Browser (headless)");
        info.put("version", version());
        Map<String, Object> reply = new LinkedHashMap<>();
        reply.put("protocolVersion", version);
        reply.put("capabilities", Map.of("tools", Map.of("listChanged", false)));
        reply.put("serverInfo", info);
        reply.put("instructions", BrowserTools.INSTRUCTIONS);
        return reply;
    }

    private List<Object> toolList() {
        List<Object> list = new ArrayList<>();
        for (BrowserTool tool : tools.tools()) {
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("name", tool.name());
            entry.put("title", tool.title());
            entry.put("description", tool.description());
            entry.put("inputSchema", Json.parse(tool.inputSchema()));
            list.add(entry);
        }
        return list;
    }

    private String callTool(Object id, Map<?, ?> params) {
        if (!(params.get("name") instanceof String name)) {
            return error(id, -32602, "a tool name is needed");
        }
        Map<String, Object> arguments = new LinkedHashMap<>();
        if (params.get("arguments") instanceof Map<?, ?> given) {
            given.forEach((key, value) -> arguments.put(String.valueOf(key), value));
        }
        BrowserTool.Result outcome = tools.call(name, arguments);
        List<Object> content = new ArrayList<>();
        if (outcome.png() != null) {
            Map<String, Object> image = new LinkedHashMap<>();
            image.put("type", "image");
            image.put("mimeType", "image/png");
            image.put("data", Base64.getEncoder().encodeToString(outcome.png()));
            content.add(image);
        }
        Map<String, Object> text = new LinkedHashMap<>();
        text.put("type", "text");
        text.put("text", outcome.text() == null || outcome.text().isEmpty() ? "Done."
                                                                            : outcome.text());
        content.add(text);
        Map<String, Object> reply = new LinkedHashMap<>();
        reply.put("content", content);
        reply.put("isError", outcome.error());
        return result(id, reply);
    }

    private static String result(Object id, Object value) {
        Map<String, Object> reply = new LinkedHashMap<>();
        reply.put("jsonrpc", "2.0");
        reply.put("id", id);
        reply.put("result", value);
        return Json.write(reply);
    }

    private static String error(Object id, int code, String message) {
        Map<String, Object> failure = new LinkedHashMap<>();
        failure.put("code", code);
        failure.put("message", message);
        Map<String, Object> reply = new LinkedHashMap<>();
        reply.put("jsonrpc", "2.0");
        reply.put("id", id);
        reply.put("error", failure);
        return Json.write(reply);
    }

    private static String version() {
        try (InputStream in = McpServer.class.getResourceAsStream(
                "/org/nordstjernen/app/version.properties")) {
            Properties properties = new Properties();
            if (in != null) {
                properties.load(in);
            }
            String version = properties.getProperty("version", "");
            return version.isEmpty() || version.startsWith("$") ? "dev" : version;
        } catch (IOException e) {
            return "dev";
        }
    }

    /**
     * The browser options a command line asks for.
     *
     * @throws IllegalArgumentException when an option is unknown or malformed
     */
    static AgentBrowser.Options options(String[] args) {
        AgentBrowser.Options options = AgentBrowser.Options.defaults();
        ContextOptions context = ContextOptions.ephemeral();
        ResourcePolicy policy = ResourcePolicy.allowAll();
        List<String> allowed = new ArrayList<>();
        List<String> denied = new ArrayList<>();
        String userAgent = null;
        String proxy = null;
        for (int i = 0; i < args.length; i++) {
            String option = args[i];
            switch (option) {
                case "--user-profile" -> context = ContextOptions.userProfile();
                case "--in-memory" -> context = ContextOptions.inMemory();
                case "--profile" -> context = ContextOptions.persistent(Path.of(value(args, ++i)));
                case "--viewport" -> {
                    String[] size = value(args, ++i).toLowerCase(java.util.Locale.ROOT).split("x");
                    if (size.length != 2) {
                        throw new IllegalArgumentException("--viewport takes WIDTHxHEIGHT");
                    }
                    options = options.withViewport(Integer.parseInt(size[0].trim()),
                                                   Integer.parseInt(size[1].trim()));
                }
                case "--downloads" -> options = options.withDownloads(Path.of(value(args, ++i)));
                case "--policy" -> policy = switch (value(args, ++i)) {
                    case "all" -> ResourcePolicy.allowAll();
                    case "public-web" -> ResourcePolicy.publicWeb();
                    case "none" -> ResourcePolicy.none();
                    default -> throw new IllegalArgumentException(
                        "--policy takes all, public-web or none");
                };
                case "--allow-host" -> allowed.add(value(args, ++i));
                case "--deny-host" -> denied.add(value(args, ++i));
                case "--user-agent" -> userAgent = value(args, ++i);
                case "--proxy" -> proxy = value(args, ++i);
                case "--renderer" -> options = options.withEngine(
                    options.engine().withRenderer(Path.of(value(args, ++i))));
                default -> throw new IllegalArgumentException("unknown option " + option);
            }
        }
        if (!allowed.isEmpty()) {
            policy = policy.allowHosts(allowed.toArray(String[]::new));
        }
        if (!denied.isEmpty()) {
            policy = policy.denyHosts(denied.toArray(String[]::new));
        }
        return options.withContext(context.withResourcePolicy(policy).withUserAgent(userAgent)
                                          .withProxy(proxy));
    }

    private static String value(String[] args, int at) {
        if (at >= args.length) {
            throw new IllegalArgumentException(args[at - 1] + " needs a value");
        }
        return args[at];
    }

    public static void main(String[] args) throws IOException {
        if (List.of(args).contains("--help") || List.of(args).contains("-h")) {
            System.out.print(USAGE);
            return;
        }
        AgentBrowser.Options options;
        try {
            options = options(args);
        } catch (IllegalArgumentException e) {
            System.err.println("nordstjernen mcp: " + e.getMessage());
            System.err.print(USAGE);
            System.exit(2);
            return;
        }
        InputStream requests = System.in;
        PrintStream replies = System.out;
        System.setOut(System.err);
        try (AgentBrowser browser = AgentBrowser.launch(options)) {
            new McpServer(new BrowserTools(browser), requests, replies).run();
        }
    }
}

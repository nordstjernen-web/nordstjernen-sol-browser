/* Nordstjernen — how a browser context is set up.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * How a {@link BrowserContext} is set up: where it keeps its cookies, cache,
 * storage and history, what its pages may load, and how they present
 * themselves to the sites they visit.
 *
 * <p>Instances are immutable; start from {@link #ephemeral()},
 * {@link #persistent(Path)}, {@link #userProfile()} or {@link #inMemory()}
 * and refine with the {@code with} methods.
 */
public final class ContextOptions {

    /** The kinds of profile a context can have. */
    public enum Profile {
        /** A directory of its own, created for the context and deleted with it. */
        EPHEMERAL,
        /** A directory the caller names, kept between runs. */
        PERSISTENT,
        /** This user's own Nordstjernen profile, shared with the desktop browser. */
        USER,
        /**
         * Nothing on disk at all: each page keeps its cookies and storage in
         * memory, so pages of the context do not share them.
         */
        IN_MEMORY
    }

    /** What {@code prefers-color-scheme} answers. */
    public enum ColorScheme {
        LIGHT,
        DARK
    }

    private final Profile profile;
    private final Path directory;
    private final ResourcePolicy resourcePolicy;
    private final String userAgent;
    private final String acceptLanguage;
    private final String proxy;
    private final String proxyBypass;
    private final boolean javaScript;
    private final boolean images;
    private final ColorScheme colorScheme;

    private ContextOptions(Profile profile, Path directory, ResourcePolicy resourcePolicy,
                           String userAgent, String acceptLanguage, String proxy,
                           String proxyBypass, boolean javaScript, boolean images,
                           ColorScheme colorScheme) {
        this.profile = profile;
        this.directory = directory;
        this.resourcePolicy = resourcePolicy;
        this.userAgent = userAgent;
        this.acceptLanguage = acceptLanguage;
        this.proxy = proxy;
        this.proxyBypass = proxyBypass;
        this.javaScript = javaScript;
        this.images = images;
        this.colorScheme = colorScheme;
    }

    private static ContextOptions of(Profile profile, Path directory) {
        return new ContextOptions(profile, directory, ResourcePolicy.allowAll(), null, null,
                                  null, null, true, true, ColorScheme.LIGHT);
    }

    public static ContextOptions ephemeral() {
        return of(Profile.EPHEMERAL, null);
    }

    public static ContextOptions persistent(Path directory) {
        if (directory == null) {
            throw new IllegalArgumentException("a persistent profile needs a directory");
        }
        return of(Profile.PERSISTENT, directory);
    }

    public static ContextOptions userProfile() {
        return of(Profile.USER, null);
    }

    public static ContextOptions inMemory() {
        return of(Profile.IN_MEMORY, null);
    }

    /** What the pages may load; a page can be given a policy of its own later. */
    public ContextOptions withResourcePolicy(ResourcePolicy policy) {
        if (policy == null) {
            throw new IllegalArgumentException("policy must not be null");
        }
        return new ContextOptions(profile, directory, policy, userAgent, acceptLanguage, proxy,
                                  proxyBypass, javaScript, images, colorScheme);
    }

    /**
     * The {@code User-Agent} header and {@code navigator.userAgent}; null for
     * the engine's own.
     */
    public ContextOptions withUserAgent(String userAgent) {
        return new ContextOptions(profile, directory, resourcePolicy, header(userAgent),
                                  acceptLanguage, proxy, proxyBypass, javaScript, images,
                                  colorScheme);
    }

    /**
     * The {@code Accept-Language} header, such as {@code "nb-NO,nb;q=0.9,en;q=0.8"};
     * null for the operating system's language.
     */
    public ContextOptions withAcceptLanguage(String acceptLanguage) {
        return new ContextOptions(profile, directory, resourcePolicy, userAgent,
                                  header(acceptLanguage), proxy, proxyBypass, javaScript,
                                  images, colorScheme);
    }

    /**
     * Send HTTP and HTTPS through this proxy, such as
     * {@code "http://proxy.example:3128"} or {@code "socks5h://127.0.0.1:1080"};
     * null for a direct connection.
     */
    public ContextOptions withProxy(String proxy) {
        return new ContextOptions(profile, directory, resourcePolicy, userAgent, acceptLanguage,
                                  header(proxy), proxyBypass, javaScript, images, colorScheme);
    }

    /** Hosts that bypass the proxy, comma-separated, such as {@code "localhost,.internal"}. */
    public ContextOptions withProxyBypass(String hosts) {
        return new ContextOptions(profile, directory, resourcePolicy, userAgent, acceptLanguage,
                                  proxy, header(hosts), javaScript, images, colorScheme);
    }

    /** Whether pages run scripts. */
    public ContextOptions withJavaScript(boolean enabled) {
        return new ContextOptions(profile, directory, resourcePolicy, userAgent, acceptLanguage,
                                  proxy, proxyBypass, enabled, images, colorScheme);
    }

    /** Whether pages load images. */
    public ContextOptions withImages(boolean enabled) {
        return new ContextOptions(profile, directory, resourcePolicy, userAgent, acceptLanguage,
                                  proxy, proxyBypass, javaScript, enabled, colorScheme);
    }

    /** What {@code prefers-color-scheme} answers in the pages; light unless set. */
    public ContextOptions withColorScheme(ColorScheme scheme) {
        if (scheme == null) {
            throw new IllegalArgumentException("scheme must not be null");
        }
        return new ContextOptions(profile, directory, resourcePolicy, userAgent, acceptLanguage,
                                  proxy, proxyBypass, javaScript, images, scheme);
    }

    public Profile profile() {
        return profile;
    }

    /** The directory of a {@link Profile#PERSISTENT} context, otherwise null. */
    public Path directory() {
        return directory;
    }

    public ResourcePolicy resourcePolicy() {
        return resourcePolicy;
    }

    public String userAgent() {
        return userAgent;
    }

    public String acceptLanguage() {
        return acceptLanguage;
    }

    public String proxy() {
        return proxy;
    }

    public String proxyBypass() {
        return proxyBypass;
    }

    public boolean javaScript() {
        return javaScript;
    }

    public boolean images() {
        return images;
    }

    public ColorScheme colorScheme() {
        return colorScheme;
    }

    /** The variables that hand these settings to a renderer process. */
    Map<String, String> environment() {
        Map<String, String> env = new LinkedHashMap<>();
        if (userAgent != null) {
            env.put("NS_USER_AGENT", userAgent);
        }
        if (acceptLanguage != null) {
            env.put("NS_ACCEPT_LANGUAGE", acceptLanguage);
        }
        if (proxy != null) {
            env.put("NS_HTTP_PROXY", proxy);
            env.put("NS_HTTPS_PROXY", proxy);
        }
        if (proxyBypass != null) {
            env.put("NS_NO_PROXY", proxyBypass);
        }
        if (!javaScript) {
            env.put("NS_NO_JAVASCRIPT", "1");
        }
        if (!images) {
            env.put("NS_NO_IMAGES", "1");
        }
        return env;
    }

    private static String header(String value) {
        if (value == null || value.isBlank()) {
            return null;
        }
        if (value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0 || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("the value must be a single line");
        }
        return value.trim();
    }
}

/* Nordstjernen — what a page may load.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import org.nordstjernen.internal.Json;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * What a page may load: which hosts it may reach, and whether it may read
 * local files or reach private and loopback addresses. The engine applies
 * the policy to every request the page causes: navigations, redirects,
 * subresources, {@code fetch}, {@code XMLHttpRequest}, WebSocket and
 * EventSource connections. {@code data:}, {@code blob:} and {@code about:}
 * URLs are never network requests and always load.
 *
 * <p>A host pattern is a host name ({@code example.com}), a name with a
 * leading wildcard label ({@code *.example.com}, every subdomain but not
 * {@code example.com} itself) or {@code *} for every host. Ports and schemes
 * are not part of a pattern.
 *
 * <p>Instances are immutable; every method returns a new policy.
 */
public final class ResourcePolicy {

    private final boolean offline;
    private final boolean files;
    private final boolean privateNetwork;
    private final List<String> allowedHosts;
    private final List<String> deniedHosts;

    private ResourcePolicy(boolean offline, boolean files, boolean privateNetwork,
                           List<String> allowedHosts, List<String> deniedHosts) {
        this.offline = offline;
        this.files = files;
        this.privateNetwork = privateNetwork;
        this.allowedHosts = allowedHosts;
        this.deniedHosts = deniedHosts;
    }

    /**
     * No restriction beyond the engine's own rules, as in the desktop
     * browser: a remote page cannot read local files, and a page from a
     * public address cannot reach a private one.
     */
    public static ResourcePolicy allowAll() {
        return new ResourcePolicy(false, true, true, null, List.of());
    }

    /**
     * Nothing from outside the document: no network and no local files. For
     * rendering HTML that carries its styles and images inline.
     */
    public static ResourcePolicy none() {
        return new ResourcePolicy(true, false, false, null, List.of());
    }

    /**
     * The public web only: no local files, and no private, link-local or
     * loopback address, whether the URL names one or a public name resolves
     * to one. For rendering pages or HTML the caller does not trust, on a
     * machine that can reach internal services.
     */
    public static ResourcePolicy publicWeb() {
        return new ResourcePolicy(false, false, false, null, List.of());
    }

    /** Only the hosts matching {@code patterns}, replacing an earlier list. */
    public ResourcePolicy allowHosts(String... patterns) {
        return new ResourcePolicy(offline, files, privateNetwork, hosts(patterns), deniedHosts);
    }

    /** Never the hosts matching {@code patterns}, added to those denied already. */
    public ResourcePolicy denyHosts(String... patterns) {
        List<String> denied = new ArrayList<>(deniedHosts);
        denied.addAll(hosts(patterns));
        return new ResourcePolicy(offline, files, privateNetwork, allowedHosts,
                                  List.copyOf(denied));
    }

    /**
     * Whether {@code file:} URLs load. When they do, the engine's own rule
     * still holds: a document with a {@code file:} URL reads files in its own
     * directory and below, and no other document reads any.
     */
    public ResourcePolicy files(boolean allowed) {
        return new ResourcePolicy(offline, allowed, privateNetwork, allowedHosts, deniedHosts);
    }

    /**
     * Whether private, link-local and loopback addresses can be reached. The
     * address check is made on the connection itself; through a proxy the
     * engine only sees the host name, and judges by that.
     */
    public ResourcePolicy privateNetwork(boolean allowed) {
        return new ResourcePolicy(offline, files, allowed, allowedHosts, deniedHosts);
    }

    /** Whether the network is off altogether. */
    public ResourcePolicy offline(boolean off) {
        return new ResourcePolicy(off, files, privateNetwork, allowedHosts, deniedHosts);
    }

    public boolean isOffline() {
        return offline;
    }

    public boolean allowsFiles() {
        return files;
    }

    public boolean allowsPrivateNetwork() {
        return privateNetwork;
    }

    /** The allowed host patterns, or null when every host is allowed. */
    public List<String> allowedHosts() {
        return allowedHosts;
    }

    public List<String> deniedHosts() {
        return deniedHosts;
    }

    String toJson() {
        StringBuilder json = new StringBuilder("{\"offline\":").append(offline ? 1 : 0)
            .append(",\"block_file\":").append(files ? 0 : 1)
            .append(",\"block_private\":").append(privateNetwork ? 0 : 1)
            .append(",\"deny_hosts\":").append(Json.quote(String.join("\n", deniedHosts)));
        if (allowedHosts != null) {
            json.append(",\"allow_hosts\":").append(Json.quote(String.join("\n", allowedHosts)));
        }
        return json.append('}').toString();
    }

    private static List<String> hosts(String... patterns) {
        List<String> hosts = new ArrayList<>();
        for (String pattern : patterns) {
            String host = pattern == null ? "" : pattern.trim().toLowerCase(Locale.ROOT);
            boolean wildcard = host.startsWith("*.");
            String name = wildcard ? host.substring(2) : host;
            if (name.isEmpty() || (!host.equals("*") && !name.matches("[a-z0-9._\\[\\]:-]+"))) {
                throw new IllegalArgumentException("not a host pattern: " + pattern);
            }
            hosts.add(host);
        }
        return List.copyOf(hosts);
    }

    @Override
    public String toString() {
        return "ResourcePolicy" + toJson();
    }
}

/* Nordstjernen — native library loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;

/**
 * Loads {@code libnordstjernen} (the engine) and {@code libnordstjernenjni}
 * (this bridge). Resolution order:
 * <ol>
 *   <li>the directory in the {@code nordstjernen.native.dir} system property;</li>
 *   <li>libraries bundled in the jar under {@code /native/<os>-<arch>/}, unpacked
 *       once into a per-user cache directory named after their contents (the
 *       {@code nordstjernen.cache.dir} system property moves it), so every JVM
 *       that uses the same jar shares one copy and nothing is left behind in the
 *       temp directory;</li>
 *   <li>the platform loader ({@code java.library.path} / {@code LD_LIBRARY_PATH}).</li>
 * </ol>
 * The engine is loaded before the bridge: the bridge links against it, and on
 * Windows a library's own directory is not searched for what it depends on.
 */
final class NativeLoader {

    private static boolean loaded = false;

    private NativeLoader() {
    }

    static synchronized void load() {
        if (loaded) {
            return;
        }
        String dir = System.getProperty("nordstjernen.native.dir");
        if (dir != null && !dir.isEmpty()) {
            loadFromDir(Path.of(dir).toAbsolutePath().normalize());
        } else if (!loadFromResources()) {
            System.loadLibrary(isWindows() ? "libnordstjernen" : "nordstjernen");
            System.loadLibrary("nordstjernenjni");
        }
        loaded = true;
    }

    private static void loadFromDir(Path dir) {
        System.load(dir.resolve(engineLibrary()).toString());
        System.load(dir.resolve(bridgeLibrary()).toString());
    }

    private static boolean loadFromResources() {
        String base = "/native/" + platform() + "/";
        if (NativeLoader.class.getResource(base + bridgeLibrary()) == null
            || NativeLoader.class.getResource(base + engineLibrary()) == null) {
            return false;
        }
        try {
            loadFromDir(unpack(base));
            return true;
        } catch (IOException e) {
            throw new NordstjernenException("failed to unpack the native libraries: "
                + e.getMessage() + "; set -Dnordstjernen.cache.dir to a writable directory,"
                + " or -Dnordstjernen.native.dir to one that already holds them", e);
        }
    }

    /**
     * The cache directory holding this jar's libraries, unpacking them first
     * when it does not exist yet. The directory is filled under a temporary
     * name and renamed into place, so another JVM never sees it half-written.
     */
    private static Path unpack(String base) throws IOException {
        String[] names = { engineLibrary(), bridgeLibrary() };
        byte[][] contents = new byte[names.length][];
        MessageDigest digest = sha256();
        for (int i = 0; i < names.length; i++) {
            try (InputStream in = NativeLoader.class.getResourceAsStream(base + names[i])) {
                if (in == null) {
                    throw new IOException("missing resource " + base + names[i]);
                }
                contents[i] = in.readAllBytes();
            }
            digest.update(contents[i]);
        }
        String key = platform() + "-" + HexFormat.of().formatHex(digest.digest(), 0, 12);
        Path root = cacheRoot();
        Path target = root.resolve(key);
        if (complete(target, names, contents)) {
            return target;
        }
        Files.createDirectories(root);
        if (Files.exists(target)) {
            deleteTree(target, names);
        }
        Path staging = Files.createTempDirectory(root, key + ".");
        for (int i = 0; i < names.length; i++) {
            Files.write(staging.resolve(names[i]), contents[i]);
        }
        try {
            Files.move(staging, target, StandardCopyOption.ATOMIC_MOVE);
            return target;
        } catch (IOException e) {
            if (complete(target, names, contents)) {
                deleteTree(staging, names);
                return target;
            }
            return staging;
        }
    }

    private static boolean complete(Path dir, String[] names, byte[][] contents) {
        try {
            for (int i = 0; i < names.length; i++) {
                Path file = dir.resolve(names[i]);
                if (!Files.isRegularFile(file) || Files.size(file) != contents[i].length) {
                    return false;
                }
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static void deleteTree(Path dir, String[] names) {
        try {
            for (String name : names) {
                Files.deleteIfExists(dir.resolve(name));
            }
            Files.deleteIfExists(dir);
        } catch (IOException e) {
            dir.toFile().deleteOnExit();
        }
    }

    private static Path cacheRoot() {
        String configured = System.getProperty("nordstjernen.cache.dir");
        if (configured != null && !configured.isEmpty()) {
            return Path.of(configured).toAbsolutePath().resolve("native");
        }
        String home = System.getProperty("user.home", ".");
        Path base;
        if (isWindows()) {
            String local = System.getenv("LOCALAPPDATA");
            base = local != null && !local.isEmpty()
                ? Path.of(local) : Path.of(home, "AppData", "Local");
        } else if (osName().contains("mac") || osName().contains("darwin")) {
            base = Path.of(home, "Library", "Caches");
        } else {
            String xdg = System.getenv("XDG_CACHE_HOME");
            base = xdg != null && !xdg.isEmpty() ? Path.of(xdg) : Path.of(home, ".cache");
        }
        return base.resolve("nordstjernen").resolve("java").resolve("native");
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new NordstjernenException("SHA-256 is unavailable", e);
        }
    }

    private static String engineLibrary() {
        return isWindows() ? "libnordstjernen.dll" : System.mapLibraryName("nordstjernen");
    }

    private static String bridgeLibrary() {
        return System.mapLibraryName("nordstjernenjni");
    }

    private static String osName() {
        return System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
    }

    private static boolean isWindows() {
        return osName().startsWith("windows");
    }

    private static String platform() {
        String os = osName();
        String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        String o = isWindows() ? "windows"
                : (os.contains("mac") || os.contains("darwin")) ? "macos"
                : "linux";
        String a = switch (arch) {
            case "amd64", "x86_64" -> "x86_64";
            case "aarch64", "arm64" -> "aarch64";
            default -> arch;
        };
        return o + "-" + a;
    }
}

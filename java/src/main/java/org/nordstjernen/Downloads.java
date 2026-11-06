/* Nordstjernen — puts a download the renderer fetched where the user asked for it.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen;

import org.nordstjernen.internal.EngineDirs;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.UserDefinedFileAttributeView;
import java.util.List;
import java.util.Locale;

/**
 * The shell's half of a download. The renderer fetches the response, with the
 * page's cookies and its {@code blob:} URLs, into a staging file under its
 * cache directory and reports that path; the shell checks the path and moves
 * the file to its destination. The renderer is the untrusted side, so a
 * reported path outside the staging directory is refused rather than moved.
 */
public final class Downloads {

    private Downloads() {
    }

    /**
     * Whether {@code staged} is a regular file directly inside the renderer's
     * download staging directory, {@code <cache>/nordstjernen/downloads}.
     */
    public static boolean isStaged(Path staged) {
        return isStaged(staged, EngineDirs.cacheSubdirs("downloads"));
    }

    /**
     * Whether {@code staged} is a regular file directly inside one of
     * {@code stagingDirs}, for a renderer whose profile is not the default one.
     */
    public static boolean isStaged(Path staged, List<Path> stagingDirs) {
        if (staged == null || !staged.isAbsolute()
            || !Files.isRegularFile(staged, LinkOption.NOFOLLOW_LINKS)) {
            return false;
        }
        try {
            Path parent = staged.toRealPath(LinkOption.NOFOLLOW_LINKS).getParent();
            for (Path dir : stagingDirs) {
                if (Files.isDirectory(dir) && dir.toRealPath().equals(parent)) {
                    return true;
                }
            }
            return false;
        } catch (IOException e) {
            return false;
        }
    }

    /**
     * Move a finished download from the staging file the renderer reported to
     * {@code destination}, replacing a file already there, and record where it
     * came from the way the platform expects (the mark of the web on Windows,
     * the quarantine and origin attributes on macOS and Linux).
     *
     * @param originUrl the URL the bytes were fetched from, or null
     * @throws NordstjernenException when the reported path is not a staged
     *                               download or the file cannot be moved
     */
    public static void moveIntoPlace(Path staged, Path destination, String originUrl) {
        moveIntoPlace(staged, destination, originUrl, EngineDirs.cacheSubdirs("downloads"));
    }

    /**
     * {@link #moveIntoPlace(Path, Path, String)} for a renderer that stages its
     * downloads in one of {@code stagingDirs}.
     */
    public static void moveIntoPlace(Path staged, Path destination, String originUrl,
                                     List<Path> stagingDirs) {
        if (!isStaged(staged, stagingDirs)) {
            throw new NordstjernenException(
                "the renderer reported a download outside its staging directory: " + staged);
        }
        try {
            try {
                Files.move(staged, destination, StandardCopyOption.REPLACE_EXISTING);
            } catch (IOException crossVolume) {
                Files.copy(staged, destination, StandardCopyOption.REPLACE_EXISTING);
                Files.deleteIfExists(staged);
            }
        } catch (IOException e) {
            try {
                Files.deleteIfExists(staged);
            } catch (IOException ignored) {
                staged.toFile().deleteOnExit();
            }
            throw new NordstjernenException("cannot save the download to " + destination
                + ": " + e.getMessage(), e);
        }
        markOrigin(destination, originUrl);
    }

    private static void markOrigin(Path file, String originUrl) {
        String url = cleanOrigin(originUrl);
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        try {
            if (os.startsWith("windows")) {
                StringBuilder zone = new StringBuilder("[ZoneTransfer]\r\nZoneId=3\r\n");
                if (url != null) {
                    zone.append("HostUrl=").append(url).append("\r\n")
                        .append("ReferrerUrl=").append(url).append("\r\n");
                }
                try (FileOutputStream stream = new FileOutputStream(file + ":Zone.Identifier")) {
                    stream.write(zone.toString().getBytes(StandardCharsets.UTF_8));
                }
                return;
            }
            UserDefinedFileAttributeView attributes =
                Files.getFileAttributeView(file, UserDefinedFileAttributeView.class);
            if (attributes == null) {
                return;
            }
            if (os.contains("mac") || os.contains("darwin")) {
                String value = String.format("0001;%08x;Nordstjernen;",
                                             System.currentTimeMillis() / 1000L);
                attributes.write("com.apple.quarantine",
                                 StandardCharsets.US_ASCII.encode(value));
            } else if (url != null) {
                attributes.write("xdg.origin.url", StandardCharsets.UTF_8.encode(url));
            }
        } catch (IOException | RuntimeException e) {
            return;
        }
    }

    private static String cleanOrigin(String url) {
        if (url == null || !(url.startsWith("http://") || url.startsWith("https://"))) {
            return null;
        }
        for (int i = 0; i < url.length(); i++) {
            char c = url.charAt(i);
            if (c < 0x20 || c == 0x7f) {
                return null;
            }
        }
        return url;
    }
}

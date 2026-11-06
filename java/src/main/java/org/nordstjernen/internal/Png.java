/* Nordstjernen — PNG encoding of a rendered frame, without java.desktop.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.internal;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

/** Writes the opaque BGRA rows the renderer paints as an RGB PNG. */
public final class Png {

    private static final byte[] SIGNATURE = {
        (byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };

    private Png() {
    }

    /**
     * Encode {@code height} rows of {@code width} BGRA pixels, {@code stride}
     * bytes apart. The alpha byte is dropped: a rendered frame is opaque.
     */
    public static byte[] encodeBgra(byte[] pixels, int width, int height, int stride) {
        if (width <= 0 || height <= 0 || stride < width * 4
                || pixels.length < (long) stride * (height - 1) + width * 4L) {
            throw new IllegalArgumentException("the pixels do not fill " + width + "x" + height);
        }
        byte[] rows = new byte[(width * 3 + 1) * height];
        int out = 0;
        for (int y = 0; y < height; y++) {
            int in = y * stride;
            rows[out++] = 0;
            for (int x = 0; x < width; x++, in += 4) {
                rows[out++] = pixels[in + 2];
                rows[out++] = pixels[in + 1];
                rows[out++] = pixels[in];
            }
        }
        Deflater deflater = new Deflater(Deflater.BEST_SPEED);
        deflater.setInput(rows);
        deflater.finish();
        ByteArrayOutputStream data = new ByteArrayOutputStream(rows.length / 8 + 64);
        byte[] buffer = new byte[65536];
        while (!deflater.finished()) {
            data.write(buffer, 0, deflater.deflate(buffer));
        }
        deflater.end();

        ByteArrayOutputStream png = new ByteArrayOutputStream(data.size() + 64);
        png.writeBytes(SIGNATURE);
        chunk(png, "IHDR", ByteBuffer.allocate(13).putInt(width).putInt(height)
            .put((byte) 8).put((byte) 2).put((byte) 0).put((byte) 0).put((byte) 0).array());
        chunk(png, "IDAT", data.toByteArray());
        chunk(png, "IEND", new byte[0]);
        return png.toByteArray();
    }

    private static void chunk(ByteArrayOutputStream png, String type, byte[] data) {
        byte[] name = type.getBytes(StandardCharsets.US_ASCII);
        CRC32 crc = new CRC32();
        crc.update(name);
        crc.update(data);
        png.writeBytes(ByteBuffer.allocate(4).putInt(data.length).array());
        png.writeBytes(name);
        png.writeBytes(data);
        png.writeBytes(ByteBuffer.allocate(4).putInt((int) crc.getValue()).array());
    }
}

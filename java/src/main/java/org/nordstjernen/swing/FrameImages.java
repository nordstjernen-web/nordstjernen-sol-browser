/* Nordstjernen — turns the renderer's pixels into AWT images.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.swing;

import org.nordstjernen.browser.Frame;
import org.nordstjernen.browser.Page;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Conversions from the engine's premultiplied BGRA rows to {@link BufferedImage}. */
public final class FrameImages {

    private FrameImages() {
    }

    /** The frame as an image, or null when the frame carries no new pixels. */
    public static BufferedImage toImage(Frame frame) {
        return frame.pixels() == null ? null
            : toImage(frame.pixels(), frame.width(), frame.height(), frame.stride());
    }

    public static BufferedImage toImage(Page.Image image) {
        return image == null ? null
            : toImage(image.pixels(), image.width(), image.height(), image.stride());
    }

    /**
     * Wrap premultiplied BGRA rows as a {@code TYPE_INT_ARGB_PRE} image. The
     * four bytes of a pixel are a little-endian ARGB word, so the rows move
     * into the raster in bulk. Null when the geometry does not fit the data.
     */
    public static BufferedImage toImage(byte[] bgra, int width, int height, int stride) {
        if (bgra == null || width <= 0 || height <= 0 || stride < width * 4) {
            return null;
        }
        BufferedImage image = new BufferedImage(width, height, BufferedImage.TYPE_INT_ARGB_PRE);
        int[] data = ((DataBufferInt) image.getRaster().getDataBuffer()).getData();
        ByteBuffer buffer = ByteBuffer.wrap(bgra).order(ByteOrder.LITTLE_ENDIAN);
        int rows = Math.min(height, bgra.length / stride);
        if (stride == width * 4) {
            buffer.asIntBuffer().get(data, 0, rows * width);
            return image;
        }
        for (int y = 0; y < rows; y++) {
            buffer.position(y * stride).asIntBuffer().get(data, y * width, width);
        }
        return image;
    }
}

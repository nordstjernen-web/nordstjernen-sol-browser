/* Nordstjernen — how a page is printed to PDF.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

package org.nordstjernen.browser;

import org.nordstjernen.internal.Json;

import java.time.Instant;
import java.time.temporal.ChronoUnit;
import java.time.format.DateTimeFormatter;
import java.util.Locale;

/**
 * The sheet, margins and extras of a {@link Page#pdf(PdfOptions)}. Values
 * are immutable; every method returns a changed copy.
 *
 * <pre>{@code
 * byte[] pdf = page.pdf(PdfOptions.a4()
 *     .margins(15)
 *     .footer(null, "Page {page} of {pages}", null));
 * }</pre>
 *
 * <p>Lengths are millimetres. The document's own {@code @page} rule, when it
 * has one, sets the sheet size and margins unless
 * {@link #ignoreCssPageSize()} says otherwise. Header and footer texts are
 * drawn in the margins and may hold {@code {page}}, {@code {pages}},
 * {@code {title}}, {@code {url}} and {@code {date}}.
 */
public final class PdfOptions {

    private static final double PX_PER_MM = 96.0 / 25.4;

    private double widthMm = 210;
    private double heightMm = 297;
    private double[] marginsMm = { 12.7, 12.7, 12.7, 12.7 };
    private double scale = 1.0;
    private boolean landscape;
    private boolean ignoreCssPageSize;
    private boolean screenMedia;
    private String title;
    private String author;
    private String creationDate;
    private String pageRanges;
    private String[] header = new String[3];
    private String[] footer = new String[3];

    private PdfOptions() {
    }

    private PdfOptions copy() {
        PdfOptions o = new PdfOptions();
        o.widthMm = widthMm;
        o.heightMm = heightMm;
        o.marginsMm = marginsMm.clone();
        o.scale = scale;
        o.landscape = landscape;
        o.ignoreCssPageSize = ignoreCssPageSize;
        o.screenMedia = screenMedia;
        o.title = title;
        o.author = author;
        o.creationDate = creationDate;
        o.pageRanges = pageRanges;
        o.header = header.clone();
        o.footer = footer.clone();
        return o;
    }

    /** A4 portrait with half-inch margins. */
    public static PdfOptions a4() {
        return new PdfOptions();
    }

    /** US Letter portrait with half-inch margins. */
    public static PdfOptions letter() {
        return a4().paper(215.9, 279.4);
    }

    /** A sheet of the given size. */
    public PdfOptions paper(double widthMm, double heightMm) {
        if (!(widthMm > 0) || !(heightMm > 0)) {
            throw new IllegalArgumentException("the sheet must have a positive size");
        }
        PdfOptions o = copy();
        o.widthMm = widthMm;
        o.heightMm = heightMm;
        return o;
    }

    /** Turn the sheet on its side. */
    public PdfOptions landscape() {
        PdfOptions o = copy();
        o.landscape = true;
        return o;
    }

    /** The same margin on all four sides. */
    public PdfOptions margins(double mm) {
        return margins(mm, mm, mm, mm);
    }

    public PdfOptions margins(double topMm, double rightMm, double bottomMm, double leftMm) {
        if (topMm < 0 || rightMm < 0 || bottomMm < 0 || leftMm < 0) {
            throw new IllegalArgumentException("margins must not be negative");
        }
        PdfOptions o = copy();
        o.marginsMm = new double[] { topMm, rightMm, bottomMm, leftMm };
        return o;
    }

    /**
     * Shrink or enlarge the content on the sheet: at 0.5 twice as much of the
     * page fits across it. From 0.1 to 10.
     */
    public PdfOptions scale(double scale) {
        if (!(scale >= 0.1) || scale > 10) {
            throw new IllegalArgumentException("scale must be between 0.1 and 10");
        }
        PdfOptions o = copy();
        o.scale = scale;
        return o;
    }

    /** Print only some sheets: {@code "1-3,5"}, {@code "2-"}. */
    public PdfOptions pageRanges(String ranges) {
        PdfOptions o = copy();
        o.pageRanges = ranges;
        return o;
    }

    /** Keep this sheet size and these margins even when the document has an {@code @page} rule. */
    public PdfOptions ignoreCssPageSize() {
        PdfOptions o = copy();
        o.ignoreCssPageSize = true;
        return o;
    }

    /** Lay the page out with its screen styles instead of its {@code @media print} ones. */
    public PdfOptions screenMedia() {
        PdfOptions o = copy();
        o.screenMedia = true;
        return o;
    }

    /** The document title in the PDF's metadata; the page's own title when not set. */
    public PdfOptions title(String title) {
        PdfOptions o = copy();
        o.title = title;
        return o;
    }

    public PdfOptions author(String author) {
        PdfOptions o = copy();
        o.author = author;
        return o;
    }

    /**
     * The creation date in the PDF's metadata, and the {@code {date}} of a
     * header or footer. Fix it to get the same bytes for the same page twice.
     */
    public PdfOptions creationDate(Instant when) {
        PdfOptions o = copy();
        o.creationDate = when == null ? null
            : DateTimeFormatter.ISO_INSTANT.format(when.truncatedTo(ChronoUnit.SECONDS));
        return o;
    }

    /** Text for the left, centre and right of the top margin; null leaves a place empty. */
    public PdfOptions header(String left, String center, String right) {
        PdfOptions o = copy();
        o.header = new String[] { left, center, right };
        return o;
    }

    /** Text for the left, centre and right of the bottom margin; null leaves a place empty. */
    public PdfOptions footer(String left, String center, String right) {
        PdfOptions o = copy();
        o.footer = new String[] { left, center, right };
        return o;
    }

    String toJson() {
        StringBuilder json = new StringBuilder("{");
        number(json, "width", widthMm * PX_PER_MM);
        number(json, "height", heightMm * PX_PER_MM);
        number(json, "margin_top", marginsMm[0] * PX_PER_MM);
        number(json, "margin_right", marginsMm[1] * PX_PER_MM);
        number(json, "margin_bottom", marginsMm[2] * PX_PER_MM);
        number(json, "margin_left", marginsMm[3] * PX_PER_MM);
        number(json, "scale", scale);
        json.append("\"landscape\":").append(landscape ? 1 : 0)
            .append(",\"ignore_css_page_size\":").append(ignoreCssPageSize ? 1 : 0)
            .append(",\"screen_media\":").append(screenMedia ? 1 : 0);
        text(json, "title", title);
        text(json, "author", author);
        text(json, "creation_date", creationDate);
        text(json, "page_ranges", pageRanges);
        text(json, "header_left", header[0]);
        text(json, "header_center", header[1]);
        text(json, "header_right", header[2]);
        text(json, "footer_left", footer[0]);
        text(json, "footer_center", footer[1]);
        text(json, "footer_right", footer[2]);
        return json.append('}').toString();
    }

    private static void number(StringBuilder json, String key, double value) {
        json.append('"').append(key).append("\":")
            .append(String.format(Locale.ROOT, "%.3f", value)).append(',');
    }

    private static void text(StringBuilder json, String key, String value) {
        if (value != null && !value.isEmpty()) {
            json.append(",\"").append(key).append("\":").append(Json.quote(value));
        }
    }
}

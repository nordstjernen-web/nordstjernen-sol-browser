# Printing

Nordstjernen lays a page out for paper and hands the sheets to the
operating system's own print dialog. No printing UI is written per
platform and no dependency is added: the sheets go to
`GtkPrintOperation`, which is CUPS on Linux, the Win32 printer dialog on
Windows, and the Cocoa print panel on macOS.

## Printing a page

`Ctrl+P`, or **Print…** in the **☰** toolbar menu, opens the system
print dialog with the current page paginated behind it. Paper size,
orientation, printer and copies are the dialog's business; what
Nordstjernen decides is where each sheet begins and ends.

Printing works in every process mode. A cairo recording surface crosses
no process boundary, so the renderer rasterises
each sheet at `NS_PRINT_RASTER_SCALE` device pixels per CSS pixel, writes
it beside the page-export files in the runtime directory, and the shell
loads the sheets back and prints them. The pagination is identical either
way; only the sheets' resolution differs.

To get a file rather than a printer, from any mode and with no printer
configured:

```sh
nordstjernen --dump=print:out.pdf https://example.org/
```

That renders the same pagination to a multi-page PDF. It is the quickest
way to check what a page will do on paper.

## The CSS a printer reads

Pagination is ordinary CSS, so a page's own print stylesheet decides most
of the result.

**`@media print`** matches while laying out for paper. The media type is
the one being laid out for rather than a hardcoded `screen`, so a print
stylesheet applies — and only then.

**`@page`** sets the sheet. It takes a paper name (`A4`, `letter`,
`legal`, `ledger`, and the A and B series), one length for a square
sheet or two for width and height, or `portrait`/`landscape`, along with
the margins:

```css
@page {
  size: A4 portrait;
  margin: 18mm 14mm;
}
```

**Breaks.** `break-before`, `break-after` and `break-inside` decide where
a sheet may end, with the legacy `page-break-*` spellings mapping onto
them and `always` becoming `page`:

```css
h2            { break-after: avoid; }
figure, table { break-inside: avoid; }
.chapter      { break-before: page; }
```

A sheet is cut at a forced break when one falls before the sheet is full.
Otherwise the cut is pulled up above any box it would split: every leaf
box, every line of a paragraph, every table row, and anything asking for
`break-inside: avoid` that fits on a sheet. `orphans` and `widows` (two
lines each unless the page says otherwise) keep the first and last lines of
a paragraph from being left alone, a heading stays with what follows it, and
`break-after: avoid` / `break-before: avoid` glue two boxes together. A
forced break with nothing before it on the sheet, or nothing after it in the
document, makes no blank page. The on-screen layout is restored afterwards,
so printing leaves the page as it found it.

Not done: a table's header row is not repeated on later sheets, `@page`
selectors (`:first`, `:left`, `:right`, named pages) and margin boxes are
not read, and running headers, footnotes and cross-references to page
numbers are out of scope.

## PDF from a program

`ns_browser_render_pdf` in the C embedding API, `POST /pdf` in the engine
protocol and `Page.pdf(PdfOptions)` in Java all return the paged document
as bytes. The options are the sheet (A4 unless given, turned on its side
for landscape), the four margins, a scale for the content, a page-range
list such as `1-3,5`, whether an `@page` rule may override the sheet,
whether to lay out with screen styles instead of print ones, the title,
author and creation date of the document metadata, and up to three texts
each for the top and bottom margin. Those may hold `{page}`, `{pages}`,
`{title}`, `{url}` and `{date}`:

```java
byte[] pdf = page.pdf(PdfOptions.a4()
    .margins(15)
    .footer(null, "Page {page} of {pages}", null));
```

The page sees `beforeprint` before it is laid out for the sheet and
`afterprint` once the screen layout is back. In the document, text is real
text with embedded font subsets, links to `http(s):`, `mailto:` and `tel:`
targets stay clickable, and `h1` to `h6` become the outline. With a fixed
creation date the same page prints to the same bytes.

`--dump=pdf:FILE`, `/export` with a `.pdf` path and `ns_browser_render_image`
are a different thing: one page as tall as the document, laid out for the
screen.

## Checking the result

`data/render-tests/print-pagination.html` is the fixture the behaviour is
kept honest against:

```sh
./builddir/src/gtk/nordstjernen --dump=print:/tmp/pagination.pdf \
    data/render-tests/print-pagination.html
```

It comes out as three A4 sheets, with every card whole, the
`@media print` paragraph swapped in for the screen one, and the forced
break starting sheet three.

## See also

- [Controls.md](Controls.md) — the keyboard shortcuts.
- [CSS-compatibility.md](CSS-compatibility.md) — the paged-media rows.
- [Rendering.md](Rendering.md) — how the same box tree reaches the screen.
- [single-process-mode.md](single-process-mode.md) — what `--single-process` changes.

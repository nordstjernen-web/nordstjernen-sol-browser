#!/usr/bin/env python3
# pdf-smoke.py: open a generated one-page PDF headless through the PDF.js viewer and check its text comes out.
import pathlib
import subprocess
import sys
import tempfile

EXPECTED = "Hello from PDF.js"


def one_page_pdf():
    stream = ("BT /F1 28 Tf 40 100 Td (%s) Tj ET" % EXPECTED).encode()
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] "
        b"/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = b"%PDF-1.4\n"
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objects) + 1, xref)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: pdf-smoke.py BROWSER")
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
    with tempfile.TemporaryDirectory() as tmp:
        pdf = pathlib.Path(tmp) / "smoke.pdf"
        pdf.write_bytes(one_page_pdf())
        run = subprocess.run(
            [sys.argv[1], "--headless", "--settle-ms=4000", "--dump=text",
             pdf.resolve().as_uri()],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=180)
    print(run.stdout)
    if EXPECTED not in run.stdout:
        print(run.stderr, file=sys.stderr)
        sys.exit("PDF smoke test: the viewer did not show the PDF's text")


if __name__ == "__main__":
    main()

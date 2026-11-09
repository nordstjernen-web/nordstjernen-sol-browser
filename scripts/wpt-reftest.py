#!/usr/bin/env python3
"""Run web-platform-tests reftests through Nordstjernen's headless PNG dump.

Each test and each of its references is rendered at 800x600 with
`--dump=png`, and the two images are compared the way wptrunner does:
exact equality, or the ranges of a `<meta name="fuzzy">` (maximum
per-channel difference and number of differing pixels). A test passes when
it matches at least one `match` reference and differs from every `mismatch`
reference. References of references are not followed.

Tests are served from a static HTTP server rooted at the WPT checkout, so
`.sub.` files (server-side substitution), print and paged reftests are
skipped. Nordstjernen's PNG is as tall as the document; both images are
padded to 800x600 with the most common colour of their bottom row (the
PNG ends 32 px below the content, so that row is the page background)
before they are compared.

Usage:
  wpt-reftest.py --wpt-root=~/wpt css/CSS2/floats css/css-flexbox
  wpt-reftest.py --wpt-root=~/wpt --results=out.tsv --baseline=base.tsv css/css-grid

With --baseline the run exits 1 when a test that passed in the baseline
fails now, and lists the tests that changed in either direction.
"""

import argparse
import concurrent.futures
import functools
import hashlib
import html.parser
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

from PIL import Image, ImageChops

W, H = 800, 600
EXTS = (".html", ".htm", ".xht", ".xhtml", ".svg")
SKIP_DIRS = {"reference", "support", "resources", "tools"}


class Meta(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.refs = []
        self.fuzzy = []
        self.classes = ""
        self.root = None

    def handle_starttag(self, tag, attrs):
        a = {k.lower(): (v or "") for k, v in attrs}
        if tag == "link" or tag.endswith(":link"):
            rel = a.get("rel", "").lower().split()
            for kind in ("match", "mismatch"):
                if kind in rel and a.get("href"):
                    self.refs.append((kind, a["href"]))
        elif tag == "meta" and a.get("name", "").lower() == "fuzzy":
            self.fuzzy.append(a.get("content", ""))
        elif tag in ("html", "svg") and self.root is None:
            self.root = tag
            self.classes = a.get("class", "")

    handle_startendtag = handle_starttag


def parse_range(text):
    lo, _, hi = text.strip().partition("-")
    return (int(lo), int(hi if hi else lo))


def parse_fuzzy(content):
    key = None
    value = content
    head, sep, tail = content.rpartition(":")
    if sep and "=" not in head and ";" not in head:
        key, value = head.strip(), tail
    parts = value.split(";")
    if len(parts) != 2:
        raise ValueError(f"malformed fuzzy value {content!r}")
    named, positional = {}, []
    for part in parts:
        if "=" in part:
            name, rng = (p.strip() for p in part.split("=", 1))
            named[name] = parse_range(rng)
        else:
            positional.append(parse_range(part))
    md = named.get("maxDifference") or positional.pop(0)
    tp = named.get("totalPixels") or positional.pop(0)
    return key, (md, tp)


def read_meta(path):
    p = Meta()
    try:
        p.feed(path.read_text(encoding="utf-8", errors="replace"))
    except Exception:
        pass
    return p


def rel_parts(path, root):
    try:
        return path.relative_to(root).parts
    except ValueError:
        return None


def is_reftest(path, root):
    parts = rel_parts(path, root)
    if parts is None or any(part in SKIP_DIRS for part in parts):
        return False
    name = path.name
    if "-ref." in name or name.endswith("-ref" + path.suffix) or "-notref." in name:
        return False
    try:
        head = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    low = head.lower()
    return "rel=\"match\"" in low or "rel='match'" in low or "rel=match" in low \
        or "rel=\"mismatch\"" in low or "rel='mismatch'" in low or "rel=mismatch" in low


def collect(root, paths):
    out = []
    for rel in paths:
        p = Path(os.path.normpath(root / rel))
        if p.is_file():
            cands = [p]
        else:
            cands = sorted(q for q in p.rglob("*") if q.suffix in EXTS)
        out.extend(q for q in cands if is_reftest(q, root))
    return out


def skip_reason(path, meta, root):
    name = path.name
    if ".sub." in name:
        return "sub"
    if "-print." in name or "print" in (rel_parts(path, root) or ()):
        return "print"
    if "reftest-paged" in meta.classes:
        return "paged"
    if not meta.refs:
        return "noref"
    return None


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".xht": "application/xhtml+xml",
        ".xhtml": "application/xhtml+xml",
        ".svg": "image/svg+xml",
        ".js": "text/javascript",
        ".woff2": "font/woff2",
        ".ttf": "font/ttf",
    }

    def log_message(self, *a):
        pass


def start_server(root):
    handler = functools.partial(Handler, directory=str(root))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}"


class Renderer:
    def __init__(self, binary, outdir, settle_ms, wait_ms, timeout, chrome=None):
        self.binary = binary
        self.chrome = chrome
        self.outdir = outdir
        self.settle_ms = settle_ms
        self.wait_ms = wait_ms
        self.timeout = timeout
        self.cache = {}
        self.lock = threading.Lock()
        self.local = threading.local()
        self.shared_cache = Path(tempfile.mkdtemp(prefix="nsref-cache-"))
        self.profiles = []

    def profile(self):
        d = getattr(self.local, "dir", None)
        if d is None:
            d = Path(tempfile.mkdtemp(prefix="nsref-prof-"))
            self.local.dir = d
            with self.lock:
                self.profiles.append(d)
        return d

    def render(self, url, wait):
        with self.lock:
            fut = self.cache.get((url, wait))
            if fut is None:
                fut = concurrent.futures.Future()
                self.cache[(url, wait)] = fut
                mine = True
            else:
                mine = False
        if mine:
            try:
                fut.set_result(self._render(url, wait))
            except Exception as e:
                fut.set_result((None, str(e)))
            finally:
                if not fut.done():
                    fut.set_result((None, "interrupted"))
        path, err = fut.result()
        if path is None:
            return None, err
        with Image.open(path) as im:
            return im.convert("RGB"), None

    def _render(self, url, wait):
        out = self.outdir / (hashlib.sha1(f"{url} {wait}".encode()).hexdigest()[:16] + ".png")
        prof = self.profile()
        env = dict(os.environ,
                   XDG_CONFIG_HOME=str(prof / "c"),
                   XDG_DATA_HOME=str(prof / "d"),
                   XDG_CACHE_HOME=str(self.shared_cache),
                   NS_ALLOW_ROOT="1",
                   LC_ALL="en_US.UTF-8", LANG="en_US.UTF-8")
        settle = self.wait_ms if wait else self.settle_ms
        if self.chrome:
            cmd = [self.chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                   "--force-device-scale-factor=1", f"--window-size={W},{H}",
                   f"--user-data-dir={prof / 'chrome'}", "--no-first-run",
                   f"--virtual-time-budget={settle}", f"--screenshot={out}", url]
        else:
            cmd = [self.binary, "--headless", f"--viewport={W}x{H}",
                   f"--settle-ms={settle}", "--time-ms=0", f"--dump=png:{out}", url]
        try:
            r = subprocess.run(cmd, env=env, capture_output=True, timeout=self.timeout)
        except subprocess.TimeoutExpired:
            return None, "timeout"
        initial = out.with_name(out.stem + "-initial.png")
        if initial.exists():
            initial.unlink()
        if not out.exists():
            return None, f"no image (exit {r.returncode})"
        with Image.open(out) as im:
            img = im.convert("RGB")
        if img.size != (W, H):
            fill = page_background(img)
            canvas = Image.new("RGB", (W, H), fill)
            canvas.paste(img.crop((0, 0, min(W, img.width), min(H, img.height))), (0, 0))
            canvas.save(out)
        return out, None

    def cleanup(self):
        shutil.rmtree(self.shared_cache, ignore_errors=True)
        for d in self.profiles:
            shutil.rmtree(d, ignore_errors=True)


def page_background(img):
    if not img.width or not img.height:
        return (255, 255, 255)
    row = img.crop((0, img.height - 1, img.width, img.height))
    return max(row.getcolors(img.width + 1), key=lambda c: c[0])[1]


def differences(a, b):
    diff = ImageChops.difference(a, b)
    box = diff.getbbox()
    if not box:
        return 0, 0
    region = diff.crop(box)
    per_channel = max(hi for _, hi in region.getextrema())
    mask = region.convert("L").point(lambda v: 255 if v else 0)
    count = mask.histogram()[255]
    return per_channel, count


def equal(md, count, fuzzy):
    if not fuzzy or fuzzy == ((0, 0), (0, 0)):
        return md == 0 and count == 0
    (pc_lo, pc_hi), (px_lo, px_hi) = fuzzy
    return ((count == 0 and px_lo == 0) or (md == 0 and pc_lo == 0) or
            (pc_lo <= md <= pc_hi and px_lo <= count <= px_hi))


def fuzzy_for(meta, test_url, ref_url):
    chosen = None
    for content in meta.fuzzy:
        try:
            key, rng = parse_fuzzy(content)
        except (ValueError, IndexError):
            continue
        if key is None:
            chosen = chosen or rng
        elif urllib.parse.urljoin(test_url, key) == ref_url:
            return rng
    return chosen


def local_path(root, base, url):
    if not url.startswith(base + "/"):
        return None
    rel = urllib.parse.unquote(urllib.parse.urlsplit(url).path).lstrip("/")
    p = root / rel
    return p if p.is_file() else None


def compare_ref(root, base, renderer, meta, url, test_img, href):
    ref_url = urllib.parse.urljoin(url, href)
    ref_file = local_path(root, base, ref_url)
    ref_meta = read_meta(ref_file) if ref_file else Meta()
    ref_img, err = renderer.render(ref_url, "reftest-wait" in ref_meta.classes)
    if ref_img is None:
        return None, f"ref {err}"
    md, count = differences(test_img, ref_img)
    same = equal(md, count, fuzzy_for(meta, url, ref_url))
    return (same, md, count, ref_img), None


def run_test(path, root, base, renderer, diffdir):
    rel = path.relative_to(root).as_posix()
    meta = read_meta(path)
    reason = skip_reason(path, meta, root)
    if reason:
        return rel, "SKIP", 0, 0, "", reason
    url = f"{base}/{urllib.parse.quote(rel)}"
    test_img, err = renderer.render(url, "reftest-wait" in meta.classes)
    if test_img is None:
        return rel, "ERROR", 0, 0, "", err
    matches = [h for k, h in meta.refs if k == "match"]
    mismatches = [h for k, h in meta.refs if k == "mismatch"]
    for href in mismatches:
        res, err = compare_ref(root, base, renderer, meta, url, test_img, href)
        if res is None:
            return rel, "ERROR", 0, 0, href, err
        if res[0]:
            return rel, "FAIL", res[1], res[2], href, "matches mismatch ref"
    best = None
    for href in matches:
        res, err = compare_ref(root, base, renderer, meta, url, test_img, href)
        if res is None:
            return rel, "ERROR", 0, 0, href, err
        if res[0]:
            return rel, "PASS", res[1], res[2], href, ""
        if best is None or res[2] < best[2]:
            best = (href, res[1], res[2], res[3])
    if best is None:
        return rel, "PASS", 0, 0, mismatches[0] if mismatches else "", ""
    href, md, count, ref_img = best
    if diffdir is not None:
        save_diff(diffdir, rel, test_img, ref_img)
    return rel, "FAIL", md, count, href, ""


def tsv_line(row):
    return "\t".join(" ".join(str(c).split()) if isinstance(c, str) else str(c) for c in row) + "\n"


def safe_run_test(path, root, base, renderer, diffdir):
    try:
        return run_test(path, root, base, renderer, diffdir)
    except Exception as e:
        rel = rel_parts(path, root)
        name = "/".join(rel) if rel else str(path)
        return name, "ERROR", 0, 0, "", f"{type(e).__name__}: {e}"


def save_diff(diffdir, rel, a, b):
    name = rel.replace("/", "__")
    out = diffdir / (name + ".png")
    strip = Image.new("RGB", (W * 3, H), (255, 255, 255))
    strip.paste(a, (0, 0))
    strip.paste(b, (W, 0))
    strip.paste(ImageChops.difference(a, b).point(lambda v: 255 if v else 0), (W * 2, 0))
    strip.save(out)


def load_tsv(path):
    res = {}
    for line in Path(path).read_text().splitlines():
        cols = line.split("\t")
        if len(cols) >= 2 and not line.startswith("#"):
            res[cols[0]] = cols[1]
    return res


def summarize(rows):
    by_dir = {}
    for rel, status, *_ in rows:
        d = "/".join(rel.split("/")[:3])
        s = by_dir.setdefault(d, {"PASS": 0, "FAIL": 0, "ERROR": 0, "SKIP": 0})
        s[status] += 1
    lines = []
    for d in sorted(by_dir):
        s = by_dir[d]
        ran = s["PASS"] + s["FAIL"] + s["ERROR"]
        lines.append(f"{d:<40} {s['PASS']:>5}/{ran:<5} pass  {s['FAIL']:>4} fail  "
                     f"{s['ERROR']:>3} error  {s['SKIP']:>3} skip")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--from", dest="from_file", help="read test paths from this file, "
                    "one per line (the first column of a results TSV works)")
    ap.add_argument("--wpt-root", default=os.environ.get("NS_WPT_ROOT", str(Path.home() / "wpt")))
    ap.add_argument("--bin", default=os.environ.get("NS_BIN", str(
        Path(__file__).resolve().parent.parent / "builddir/src/gtk/nordstjernen")))
    ap.add_argument("--chrome", help="render with this Chrome binary instead, to check "
                    "which tests Chrome passes")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--settle-ms", type=int, default=100)
    ap.add_argument("--wait-ms", type=int, default=1500,
                    help="settle time for tests whose root has class reftest-wait")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--results", help="write one TSV line per test")
    ap.add_argument("--baseline", help="TSV from an earlier run to compare against")
    ap.add_argument("--diffs", help="directory for test|ref|diff images of failures")
    ap.add_argument("--resume", action="store_true",
                    help="keep the tests already in --results and run only the rest")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    root = Path(os.path.expanduser(args.wpt_root)).resolve()
    paths = list(args.paths)
    if args.from_file:
        paths += [l.split("\t")[0] for l in Path(args.from_file).read_text().splitlines() if l.strip()]
    if not paths:
        ap.error("give test paths or --from")
    tests = collect(root, paths)
    if args.list:
        for t in tests:
            print(t.relative_to(root).as_posix())
        return 0
    if not tests:
        print("no reftests found", file=sys.stderr)
        return 2
    binary = args.chrome or args.bin
    found = shutil.which(binary)
    if args.chrome and found:
        args.chrome = found
    if not found:
        print(f"browser binary not found or not executable: {binary}", file=sys.stderr)
        return 2

    srv, base = start_server(root)
    imgdir = Path(tempfile.mkdtemp(prefix="nsref-img-"))
    diffdir = Path(args.diffs) if args.diffs else None
    if diffdir:
        diffdir.mkdir(parents=True, exist_ok=True)
    renderer = Renderer(args.bin, imgdir, args.settle_ms, args.wait_ms, args.timeout,
                        chrome=args.chrome)
    before = load_tsv(args.baseline) if args.baseline else None
    t0 = time.time()
    rows = []
    if args.resume and args.results and Path(args.results).exists():
        for line in Path(args.results).read_text().splitlines():
            cols = line.split("\t")
            if len(cols) >= 6:
                rows.append(tuple(cols[:2]) + (int(cols[2]), int(cols[3])) + tuple(cols[4:6]))
        done = {r[0] for r in rows}
        tests = [t for t in tests if t.relative_to(root).as_posix() not in done]
        print(f"resuming: {len(done)} done, {len(tests)} to run", file=sys.stderr)
    fh = open(args.results, "a" if args.resume else "w") if args.results else None
    try:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            futs = [ex.submit(safe_run_test, t, root, base, renderer, diffdir) for t in tests]
            for i, f in enumerate(concurrent.futures.as_completed(futs), 1):
                row = f.result()
                rows.append(row)
                if fh:
                    fh.write(tsv_line(row))
                    fh.flush()
                if i % 200 == 0:
                    print(f"[{i}/{len(tests)}] {time.time() - t0:.0f}s", file=sys.stderr)
    finally:
        if fh:
            fh.close()
        srv.shutdown()
        renderer.cleanup()
        shutil.rmtree(imgdir, ignore_errors=True)
    rows.sort()
    if args.results:
        Path(args.results).write_text("".join(tsv_line(r) for r in rows))
    for line in summarize(rows):
        print(line)
    totals = {s: sum(1 for r in rows if r[1] == s) for s in ("PASS", "FAIL", "ERROR", "SKIP")}
    print(f"total: {totals['PASS']} pass, {totals['FAIL']} fail, {totals['ERROR']} error, "
          f"{totals['SKIP']} skip in {time.time() - t0:.0f}s")
    if before is None:
        return 0
    broke = [r[0] for r in rows if before.get(r[0]) == "PASS" and r[1] in ("FAIL", "ERROR")]
    fixed = [r[0] for r in rows if before.get(r[0]) in ("FAIL", "ERROR") and r[1] == "PASS"]
    print(f"against baseline: {len(fixed)} newly passing, {len(broke)} newly failing")
    for t in fixed:
        print(f"  + {t}")
    for t in broke:
        print(f"  - {t}")
    return 1 if broke else 0


if __name__ == "__main__":
    sys.exit(main())

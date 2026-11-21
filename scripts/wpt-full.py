#!/usr/bin/env python3
# wpt-full.py: runs every testharness.js test in a WPT checkout's MANIFEST.json through headless --wpt, in parallel and resumably.
import argparse
import collections
import datetime
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_binary():
    for name in ("nordstjernen.exe", "nordstjernen"):
        path = os.path.join(ROOT, "builddir", "src", "gtk", name)
        if os.path.exists(path):
            return path
    return os.path.join(ROOT, "builddir", "src", "gtk", "nordstjernen")


def walk_manifest(node, prefix, out):
    for name, value in node.items():
        path = prefix + "/" + name
        if isinstance(value, dict):
            walk_manifest(value, path, out)
        else:
            for item in value[1:]:
                url = item[0] if item[0] is not None else path
                out.append(("/" + url.lstrip("/"), item[1] if len(item) > 1 else {}))


def enumerate_tests(wpt_root, prefixes):
    with open(os.path.join(wpt_root, "MANIFEST.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    items = []
    walk_manifest(manifest["items"].get("testharness", {}), "", items)
    prefixes = tuple(p.strip("/") for p in prefixes)
    tests = []
    for url, extras in items:
        if url.endswith(".js"):
            continue
        if prefixes and not url.lstrip("/").startswith(prefixes):
            continue
        tests.append((url, extras.get("timeout") == "long"))
    tests.sort()
    return tests


def flags_of(url):
    name = url.split("?")[0].split("#")[0].rsplit("/", 1)[-1]
    return set(name.split(".")[1:-1])


def area_of(url):
    parts = url.lstrip("/").split("/")
    return parts[0] if len(parts) > 1 else "(root)"


def kill_tree(proc):
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            proc.kill()


def base_for(args, url):
    flags = flags_of(url)
    if "h2" in flags:
        return args.h2_base
    if "https" in flags:
        return args.https_base
    return args.base


def timeout_for(args, is_long):
    return args.long_timeout_ms if is_long else args.timeout_ms


def new_row(url, rc, secs, lines, killed):
    row = {"url": url, "area": area_of(url), "exit": 2 if killed else rc,
           "secs": round(secs, 2), "harness": None,
           "total": 0, "pass": 0, "fail": 0, "timeout": 0, "notrun": 0,
           "precondition_failed": 0, "subtests": None}
    for line in lines:
        if line.startswith("WPT HARNESS ") and row["harness"] is None:
            row["harness"] = line[len("WPT HARNESS "):].split()[0]
        elif line.startswith("WPT SUMMARY "):
            for kv in line[len("WPT SUMMARY "):].split():
                key, _, value = kv.partition("=")
                if key in row and value.isdigit():
                    row[key] = int(value)
        elif line.startswith("WPT JSON ") and row["subtests"] is None:
            try:
                result = json.loads(line[len("WPT JSON "):])
                row["subtests"] = [[s.get("name"), s.get("status")]
                                   for s in result.get("subtests", [])]
            except (ValueError, AttributeError):
                pass
    if row["harness"] is None:
        row["harness"] = "CRASH" if (rc not in (0, 1, 2) and not killed) else "NORESULT"
    return row


def popen_browser(cmd, stdin):
    kwargs = {}
    if os.name != "nt":
        kwargs["start_new_session"] = True
    return subprocess.Popen(cmd, stdin=stdin, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, **kwargs)


def run_single(args, url, is_long):
    timeout_ms = timeout_for(args, is_long)
    cmd = [args.binary, "--wpt", "--wpt-timeout-ms=%d" % timeout_ms,
           base_for(args, url) + url]
    started = time.monotonic()
    proc = popen_browser(cmd, subprocess.DEVNULL)
    killed = False
    try:
        out, _ = proc.communicate(timeout=timeout_ms / 1000 + args.grace)
    except subprocess.TimeoutExpired:
        killed = True
        kill_tree(proc)
        out, _ = proc.communicate()
    lines = out.decode("utf-8", "replace").splitlines()
    return new_row(url, proc.returncode, time.monotonic() - started, lines, killed)


class BatchBrowser:
    def __init__(self, args):
        self.args = args
        self.proc = None
        self.lines = None
        self.served = 0

    def start(self):
        self.proc = popen_browser([self.args.binary, "--wpt-batch"], subprocess.PIPE)
        self.lines = queue.Queue()
        threading.Thread(target=self.pump, args=(self.proc, self.lines),
                         daemon=True).start()
        self.served = 0

    @staticmethod
    def pump(proc, lines):
        for raw in proc.stdout:
            lines.put(raw.decode("utf-8", "replace").rstrip("\r\n"))
        lines.put(None)

    def stop(self):
        if self.proc is not None:
            kill_tree(self.proc)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
            self.proc = None

    def run(self, url, is_long):
        if self.proc is None or self.served >= self.args.batch_size:
            self.stop()
            self.start()
        timeout_ms = timeout_for(self.args, is_long)
        started = time.monotonic()
        deadline = started + timeout_ms / 1000 + self.args.grace
        try:
            self.proc.stdin.write(("%d %s%s\n" % (
                timeout_ms, base_for(self.args, url), url)).encode("utf-8"))
            self.proc.stdin.flush()
        except OSError:
            self.stop()
            return None
        self.served += 1
        lines = []
        while True:
            try:
                line = self.lines.get(timeout=max(0.05, deadline - time.monotonic()))
            except queue.Empty:
                self.stop()
                return new_row(url, 2, time.monotonic() - started, lines, True)
            if line is None:
                self.stop()
                return None
            if line.startswith("WPT END rc="):
                rc = int(line[len("WPT END rc="):] or 1)
                return new_row(url, rc, time.monotonic() - started, lines, False)
            if line.startswith("WPT BEGIN ") or line == "WPT BATCH READY":
                continue
            lines.append(line)


def worker_loop(args, jobs, results):
    browser = BatchBrowser(args) if args.batch else None
    while True:
        try:
            url, is_long = jobs.get_nowait()
        except queue.Empty:
            break
        row = browser.run(url, is_long) if browser else None
        if row is None:
            row = run_single(args, url, is_long)
        results.put(row)
    if browser:
        browser.stop()


def load_done(path):
    done = set()
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    done.add(json.loads(line)["url"])
                except (ValueError, KeyError):
                    pass
    return done


def summarize(results_path, out_tsv):
    rows = {}
    with open(results_path, encoding="utf-8") as f:
        for line in f:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            rows[row["url"]] = row
    areas = collections.OrderedDict()
    for url in sorted(rows):
        row = rows[url]
        a = areas.setdefault(row["area"], collections.Counter())
        a["files"] += 1
        a["ok"] += 1 if row["exit"] == 0 else 0
        a["harness_" + row["harness"]] += 1
        for key in ("total", "pass", "fail", "timeout", "notrun", "precondition_failed"):
            a[key] += row[key]
        a["secs"] += row["secs"]
    if out_tsv:
        with open(out_tsv, "w", encoding="utf-8", newline="\n") as f:
            f.write("test\tharness\ttotal\tpass\tfail\ttimeout\tnotrun\tprecondition_failed\n")
            for url in sorted(rows):
                r = rows[url]
                f.write("%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n" % (
                    url, r["harness"], r["total"], r["pass"], r["fail"],
                    r["timeout"], r["notrun"], r["precondition_failed"]))
    return rows, areas


def main():
    p = argparse.ArgumentParser(description="Run the whole WPT testharness suite in parallel.")
    p.add_argument("--wpt-root", required=True)
    p.add_argument("--binary", default=os.environ.get("NS_BIN") or default_binary())
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    p.add_argument("--timeout-ms", type=int, default=10000)
    p.add_argument("--long-timeout-ms", type=int, default=30000)
    p.add_argument("--no-batch", dest="batch", action="store_false")
    p.add_argument("--batch-size", type=int, default=200)
    p.add_argument("--grace", type=float, default=15.0)
    p.add_argument("--base", default="http://localhost:8000")
    p.add_argument("--https-base", default="https://localhost:8443")
    p.add_argument("--h2-base", default="https://localhost:9000")
    p.add_argument("--results", required=True)
    p.add_argument("--tsv")
    p.add_argument("--list", action="store_true")
    p.add_argument("--summary-only", action="store_true")
    p.add_argument("--paths-from")
    p.add_argument("paths", nargs="*")
    args = p.parse_args()
    if args.paths_from:
        with open(args.paths_from, encoding="utf-8") as f:
            args.paths += [line.strip() for line in f if line.strip()]
    os.environ.setdefault("NS_ALLOW_ROOT", "1")

    if args.summary_only:
        rows, areas = summarize(args.results, args.tsv)
        print(json.dumps({"files": len(rows), "areas": {k: dict(v) for k, v in areas.items()}}))
        return 0

    tests = enumerate_tests(args.wpt_root, args.paths)
    if args.list:
        for url, is_long in tests:
            print(url, "long" if is_long else "")
        return 0
    done = load_done(args.results)
    todo = [t for t in tests if t[0] not in done]
    print("wpt-full: %d testharness URLs, %d done, %d to run, %d jobs" % (
        len(tests), len(done), len(todo), args.jobs), file=sys.stderr, flush=True)

    jobs = queue.Queue()
    for item in todo:
        jobs.put(item)
    results = queue.Queue()
    workers = [threading.Thread(target=worker_loop, args=(args, jobs, results), daemon=True)
               for _ in range(args.jobs)]
    for w in workers:
        w.start()
    counts = collections.Counter()
    started = time.monotonic()
    with open(args.results, "a", encoding="utf-8", newline="\n") as out:
        for n in range(1, len(todo) + 1):
            row = results.get()
            out.write(json.dumps(row, ensure_ascii=False) + "\n")
            out.flush()
            counts["pass"] += row["pass"]
            counts["total"] += row["total"]
            if n % 200 == 0 or n == len(todo):
                rate = n / max(1e-6, time.monotonic() - started)
                eta = (len(todo) - n) / max(rate, 1e-6)
                print("[%s] %d/%d files, %d/%d subtests pass, %.1f files/s, eta %s" % (
                    datetime.datetime.now().strftime("%H:%M:%S"), n, len(todo),
                    counts["pass"], counts["total"], rate,
                    datetime.timedelta(seconds=int(eta))), file=sys.stderr, flush=True)
    if args.tsv:
        summarize(args.results, args.tsv)
    return 0


if __name__ == "__main__":
    sys.exit(main())

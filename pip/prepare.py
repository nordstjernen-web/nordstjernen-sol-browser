"""Prepares pip/ for a release build: sets the version to a published tag, records its source tarball's SHA-256 and copies the licence files in."""

import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
PACKAGE = HERE / "src" / "nordstjernen_browser"
SOURCE_URL = "https://github.com/nordstjernen-web/nordstjernen-sol-browser/archive/refs/tags/{tag}.tar.gz"


def fail(message):
    sys.exit(f"prepare.py: {message}")


def main():
    if len(sys.argv) != 2:
        fail("usage: prepare.py <release tag>")
    tag = sys.argv[1]
    if not re.fullmatch(r"\d+\.\d+\.\d+", tag):
        fail(f"{tag} is not a release tag")
    tagged = subprocess.run(["git", "show", f"refs/tags/{tag}:meson.build"], cwd=REPO,
                            capture_output=True, text=True, check=False)
    if tagged.returncode != 0:
        fail(f"there is no {tag} release tag; pip versions track published releases")
    built = re.search(r"^\s*version\s*:\s*'([^']+)'", tagged.stdout, re.M)
    if not built or built.group(1) != tag:
        fail(f"tag {tag} builds version {built.group(1) if built else 'unknown'}")

    init = PACKAGE / "__init__.py"
    text = init.read_text(encoding="utf-8")
    init.write_text(re.sub(r'__version__ = "[^"]*"', f'__version__ = "{tag}"', text),
                    encoding="utf-8", newline="\n")

    request = urllib.request.Request(SOURCE_URL.format(tag=tag),
                                     headers={"User-Agent": "nordstjernen-browser-prepare"})
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=120) as response:
        for block in iter(lambda: response.read(1 << 20), b""):
            digest.update(block)
    (PACKAGE / "source.json").write_text(
        json.dumps({"version": tag, "sha256": digest.hexdigest()}, indent=2) + "\n",
        encoding="utf-8", newline="\n")
    print(f"source tarball {tag}: sha256 {digest.hexdigest()}", file=sys.stderr)

    for name in ("License.md", "COPYING"):
        shutil.copyfile(REPO / name, HERE / name)


if __name__ == "__main__":
    main()

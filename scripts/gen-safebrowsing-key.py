#!/usr/bin/env python3
import os
import re
import sys


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: gen-safebrowsing-key.py <out>\n")
        return 2
    out = argv[1]
    key = os.environ.get("NS_SAFE_BROWSING_API_KEY", "").strip()
    if key and not re.fullmatch(r"[A-Za-z0-9_-]{20,128}", key):
        sys.stderr.write("NS_SAFE_BROWSING_API_KEY does not look like an API "
                         "key; building without Google Safe Browsing\n")
        key = ""
    text = '#define NS_SAFE_BROWSING_API_KEY "%s"\n' % key
    try:
        with open(out, encoding="ascii") as f:
            if f.read() == text:
                return 0
    except OSError:
        pass
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

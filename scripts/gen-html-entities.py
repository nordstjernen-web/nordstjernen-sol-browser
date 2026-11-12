#!/usr/bin/env python3
"""Generate src/html5_rs/entities.rs from WHATWG's entities.json.

Usage: gen-html-entities.py [ENTITIES_JSON] [OUT]

ENTITIES_JSON defaults to https://html.spec.whatwg.org/entities.json, fetched
over the network; pass a local copy to generate offline. The output is the
table of named character references the src/html5_rs tokenizer matches against,
sorted by name so it can be binary searched, and is run through rustfmt when
it is on the PATH.
"""
import json
import os
import shutil
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = sys.argv[1] if len(sys.argv) > 1 else \
    'https://html.spec.whatwg.org/entities.json'
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    os.path.join(ROOT, 'src', 'html5_rs', 'entities.rs')


def load(source):
    if source.startswith('http://') or source.startswith('https://'):
        with urllib.request.urlopen(source) as response:
            return json.loads(response.read().decode('utf-8'))
    with open(source, encoding='utf-8') as f:
        return json.load(f)


def rust_str(codepoints):
    return ''.join('\\u{%x}' % c for c in codepoints)


def main():
    entities = load(SOURCE)
    rows = []
    for name, entry in entities.items():
        if not name.startswith('&'):
            raise SystemExit('unexpected entity name %r' % name)
        rows.append((name[1:].encode('ascii'), entry['codepoints']))
    rows.sort()
    longest = max(len(name) for name, _ in rows)
    lines = [
        '//! Nordstjernen — the named character references of the HTML Standard, '
        'generated from WHATWG\'s entities.json by scripts/gen-html-entities.py.',
        '//! Copyright 2026 Andreas Røsdal',
        '//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later',
        '',
        'pub const LONGEST_NAME: usize = %d;' % longest,
        '',
        'pub static ENTITIES: [(&[u8], &str); %d] = [' % len(rows),
    ]
    for name, codepoints in rows:
        lines.append('    (b"%s", "%s"),' % (name.decode('ascii'),
                                             rust_str(codepoints)))
    lines.append('];')
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    rustfmt = shutil.which('rustfmt')
    if rustfmt:
        subprocess.run([rustfmt, '--edition', '2024', OUT], check=False)


if __name__ == '__main__':
    main()

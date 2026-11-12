#!/usr/bin/env python3
"""Generate src/url_rs/tables.rs from the Unicode Character Database.

Usage: gen-url-tables.py UCD_DIR [OUT]

UCD_DIR is one Unicode version laid out as on unicode.org: UnicodeData.txt,
DerivedNormalizationProps.txt, extracted/DerivedBidiClass.txt and
extracted/DerivedJoiningType.txt from Public/<version>/ucd/, plus
idna/IdnaMappingTable.txt from Public/idna/<version>/. The tables are what the
URL Standard's host parser needs for UTS #46 ToASCII: the IDNA mapping, NFC
(canonical combining classes, decompositions and compositions) and the
properties the validity criteria read (bidi class, joining type, marks).
The output is run through rustfmt when it is on the PATH.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UCD = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/unicode'
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    os.path.join(ROOT, 'src', 'url_rs', 'tables.rs')

MAX = 0x110000

STATUS = {'valid': 0, 'ignored': 1, 'mapped': 2, 'deviation': 3,
          'disallowed': 4}
BIDI = {'L': 1, 'R': 2, 'AL': 3, 'AN': 4, 'EN': 5, 'ES': 6, 'CS': 7,
        'ET': 8, 'ON': 9, 'BN': 10, 'NSM': 11}
BIDI_LONG = {'Left_To_Right': 'L', 'Right_To_Left': 'R',
             'Arabic_Letter': 'AL', 'European_Terminator': 'ET',
             'Boundary_Neutral': 'BN'}
JOIN = {'T': 1, 'L': 2, 'R': 3, 'D': 4}


def ucd_file(*names):
    for name in names:
        path = os.path.join(UCD, name)
        if os.path.exists(path):
            return path
    sys.exit('missing %s under %s' % (names[0], UCD))


def records(path, missing=False):
    for line in open(path, encoding='utf-8'):
        if missing:
            m = re.match(r'#\s*@missing:\s*([0-9A-F.]+)\s*;\s*(\w+)', line)
            if not m:
                continue
            fields = [m.group(1), m.group(2)]
        else:
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            fields = [x.strip() for x in line.split(';')]
        rng = fields[0]
        if '..' in rng:
            lo, hi = [int(x, 16) for x in rng.split('..')]
        else:
            lo = hi = int(rng, 16)
        yield lo, hi, fields[1:]


def version():
    for line in open(ucd_file('idna/IdnaMappingTable.txt',
                              'IdnaMappingTable.txt'), encoding='utf-8'):
        m = re.match(r'#\s*Version:\s*([0-9.]+)', line)
        if m:
            return m.group(1)
    return 'unknown'


def idna_table():
    pool = []
    pool_index = {}
    value = [STATUS['disallowed']] * MAX
    for lo, hi, fields in records(ucd_file('idna/IdnaMappingTable.txt',
                                           'IdnaMappingTable.txt')):
        status = STATUS[fields[0]]
        v = status
        if status == STATUS['mapped']:
            mapping = tuple(int(x, 16) for x in fields[1].split())
            if mapping not in pool_index:
                pool_index[mapping] = len(pool)
                pool.extend(mapping)
            v = status | len(mapping) << 3 | pool_index[mapping] << 8
        for cp in range(lo, hi + 1):
            value[cp] = v
    return value, pool


def nfc_tables():
    ccc = [0] * MAX
    decomp = {}
    marks = [False] * MAX
    for line in open(ucd_file('UnicodeData.txt'), encoding='utf-8'):
        f = line.rstrip('\n').split(';')
        cp = int(f[0], 16)
        if f[3] != '0':
            ccc[cp] = int(f[3])
        if f[2] in ('Mn', 'Mc', 'Me'):
            marks[cp] = True
        if f[5] and not f[5].startswith('<'):
            decomp[cp] = [int(x, 16) for x in f[5].split()]
    exclusions = set()
    for lo, hi, fields in records(ucd_file('DerivedNormalizationProps.txt')):
        if fields[0] == 'Full_Composition_Exclusion':
            exclusions.update(range(lo, hi + 1))
    compose = {}
    for cp, parts in decomp.items():
        if len(parts) == 2 and cp not in exclusions:
            compose[(parts[0], parts[1])] = cp
    return ccc, decomp, compose, marks


def props_table(ccc, marks):
    bidi = [BIDI['L']] * MAX
    path = ucd_file('extracted/DerivedBidiClass.txt', 'DerivedBidiClass.txt')
    for lo, hi, fields in records(path, missing=True):
        short = BIDI_LONG.get(fields[0], fields[0])
        for cp in range(lo, hi + 1):
            bidi[cp] = BIDI.get(short, 0)
    for lo, hi, fields in records(path):
        for cp in range(lo, hi + 1):
            bidi[cp] = BIDI.get(fields[0], 0)
    join = [0] * MAX
    path = ucd_file('extracted/DerivedJoiningType.txt',
                    'DerivedJoiningType.txt')
    for lo, hi, fields in records(path):
        if fields[0] in JOIN:
            for cp in range(lo, hi + 1):
                join[cp] = JOIN[fields[0]]
    return [ccc[cp] | bidi[cp] << 8 | join[cp] << 12 | marks[cp] << 15
            for cp in range(MAX)]


def ranges(values):
    starts, out = [], []
    previous = None
    for cp, v in enumerate(values):
        if v != previous:
            starts.append(cp)
            out.append(v)
            previous = v
    return starts, out


def array(name, kind, items, fmt):
    body = ', '.join(fmt(x) for x in items)
    return 'pub static %s: [%s; %d] = [%s];\n' % (name, kind, len(items),
                                                   body)


def main():
    idna_values, pool = idna_table()
    ccc, decomp, compose, marks = nfc_tables()
    props = props_table(ccc, marks)
    idna_starts, idna_out = ranges(idna_values)
    prop_starts, prop_out = ranges(props)
    decomp_keys = sorted(decomp)
    compose_keys = sorted(compose)
    hexa = lambda x: '0x%x' % x
    out = []
    out.append('//! Nordstjernen — the Unicode %s tables URL host parsing needs '
               '(the IDNA mapping, NFC and the bidi, joining and mark '
               'properties), generated by scripts/gen-url-tables.py.\n'
               % version())
    out.append('//! Copyright 2026 Andreas Røsdal\n')
    out.append('//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR '
               'GPL-3.0-or-later\n\n')
    out.append(array('IDNA_STARTS', 'u32', idna_starts, hexa))
    out.append(array('IDNA_VALUES', 'u32', idna_out, hexa))
    out.append(array('IDNA_POOL', 'u32', pool, hexa))
    out.append(array('PROP_STARTS', 'u32', prop_starts, hexa))
    out.append(array('PROP_VALUES', 'u16', prop_out, hexa))
    out.append(array('DECOMP_KEYS', 'u32', decomp_keys, hexa))
    out.append(array('DECOMP_FIRST', 'u32',
                     [decomp[k][0] for k in decomp_keys], hexa))
    out.append(array('DECOMP_SECOND', 'u32',
                     [decomp[k][1] if len(decomp[k]) > 1 else 0
                      for k in decomp_keys], hexa))
    out.append(array('COMPOSE_KEYS', 'u64',
                     [a << 21 | b for a, b in compose_keys], hexa))
    out.append(array('COMPOSE_VALUES', 'u32',
                     [compose[k] for k in compose_keys], hexa))
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(''.join(out))
    if shutil.which('rustfmt'):
        subprocess.run(['rustfmt', '--edition', '2024', OUT], check=True)


main()

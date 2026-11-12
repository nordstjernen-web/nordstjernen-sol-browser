//! Nordstjernen — character references: longest-match named references, the attribute-value rule and numeric references with the C1 remapping.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::entities::{ENTITIES, LONGEST_NAME};

const C1_REPLACEMENTS: [(u32, u32); 27] = [
    (0x80, 0x20AC),
    (0x82, 0x201A),
    (0x83, 0x0192),
    (0x84, 0x201E),
    (0x85, 0x2026),
    (0x86, 0x2020),
    (0x87, 0x2021),
    (0x88, 0x02C6),
    (0x89, 0x2030),
    (0x8A, 0x0160),
    (0x8B, 0x2039),
    (0x8C, 0x0152),
    (0x8E, 0x017D),
    (0x91, 0x2018),
    (0x92, 0x2019),
    (0x93, 0x201C),
    (0x94, 0x201D),
    (0x95, 0x2022),
    (0x96, 0x2013),
    (0x97, 0x2014),
    (0x98, 0x02DC),
    (0x99, 0x2122),
    (0x9A, 0x0161),
    (0x9B, 0x203A),
    (0x9C, 0x0153),
    (0x9E, 0x017E),
    (0x9F, 0x0178),
];

pub fn push_char(out: &mut Vec<u8>, ch: char) {
    let mut buf = [0u8; 4];
    out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
}

pub fn lookup(name: &[u8]) -> Option<&'static str> {
    ENTITIES
        .binary_search_by(|(n, _)| (*n).cmp(name))
        .ok()
        .map(|i| ENTITIES[i].1)
}

fn named(input: &[u8], pos: usize, in_attr: bool, out: &mut Vec<u8>) -> usize {
    let rest = &input[pos..];
    let mut alnum = 0;
    while alnum < rest.len() && alnum < LONGEST_NAME && rest[alnum].is_ascii_alphanumeric() {
        alnum += 1;
    }
    let candidate = if rest.get(alnum) == Some(&b';') {
        alnum + 1
    } else {
        alnum
    };
    for len in (1..=candidate).rev() {
        let Some(value) = lookup(&rest[..len]) else {
            continue;
        };
        let terminated = rest[len - 1] == b';';
        let next = rest.get(len).copied();
        if in_attr && !terminated && next.is_some_and(|c| c == b'=' || c.is_ascii_alphanumeric()) {
            out.push(b'&');
            out.extend_from_slice(&rest[..len]);
        } else {
            out.extend_from_slice(value.as_bytes());
        }
        return pos + len;
    }
    out.push(b'&');
    pos
}

fn numeric(input: &[u8], pos: usize, out: &mut Vec<u8>) -> usize {
    let mut p = pos + 1;
    let hex = matches!(input.get(p), Some(b'x' | b'X'));
    if hex {
        p += 1;
    }
    let digits_start = p;
    let mut code: u32 = 0;
    while let Some(&c) = input.get(p) {
        let digit = if hex {
            (c as char).to_digit(16)
        } else {
            (c as char).to_digit(10)
        };
        let Some(digit) = digit else { break };
        code = code
            .saturating_mul(if hex { 16 } else { 10 })
            .saturating_add(digit);
        p += 1;
    }
    if p == digits_start {
        out.push(b'&');
        out.extend_from_slice(&input[pos..digits_start]);
        return digits_start;
    }
    if input.get(p) == Some(&b';') {
        p += 1;
    }
    let code = match code {
        0 => 0xFFFD,
        c if c > 0x10FFFF || (0xD800..=0xDFFF).contains(&c) => 0xFFFD,
        c => C1_REPLACEMENTS
            .iter()
            .find(|(from, _)| *from == c)
            .map_or(c, |(_, to)| *to),
    };
    push_char(out, char::from_u32(code).unwrap_or('\u{FFFD}'));
    p
}

pub fn consume(input: &[u8], pos: usize, in_attr: bool, out: &mut Vec<u8>) -> usize {
    match input.get(pos) {
        Some(c) if c.is_ascii_alphanumeric() => named(input, pos, in_attr, out),
        Some(b'#') => numeric(input, pos, out),
        _ => {
            out.push(b'&');
            pos
        }
    }
}

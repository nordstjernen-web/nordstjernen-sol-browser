//! Nordstjernen — the URL Standard's percent-encode sets, UTF-8 percent-encoding and percent-decoding.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum EncodeSet {
    C0Control,
    Fragment,
    Query,
    SpecialQuery,
    Path,
    Userinfo,
}

fn in_c0_control(c: char) -> bool {
    !(' '..='~').contains(&c)
}

fn in_query(c: char) -> bool {
    in_c0_control(c) || matches!(c, ' ' | '"' | '#' | '<' | '>')
}

fn in_path(c: char) -> bool {
    in_query(c) || matches!(c, '?' | '^' | '`' | '{' | '}')
}

impl EncodeSet {
    pub fn contains(self, c: char) -> bool {
        match self {
            EncodeSet::C0Control => in_c0_control(c),
            EncodeSet::Fragment => in_c0_control(c) || matches!(c, ' ' | '"' | '<' | '>' | '`'),
            EncodeSet::Query => in_query(c),
            EncodeSet::SpecialQuery => in_query(c) || c == '\'',
            EncodeSet::Path => in_path(c),
            EncodeSet::Userinfo => {
                in_path(c) || matches!(c, '/' | ':' | ';' | '=' | '@' | '['..='^' | '|')
            }
        }
    }
}

const HEX: &[u8; 16] = b"0123456789ABCDEF";

pub fn needs_encoding(byte: u8, set: EncodeSet) -> bool {
    byte >= 0x80 || set.contains(char::from(byte))
}

fn hex_digits(byte: u8) -> [u8; 3] {
    [
        b'%',
        HEX[usize::from(byte >> 4)],
        HEX[usize::from(byte & 15)],
    ]
}

pub fn encode_byte(out: &mut String, byte: u8, set: EncodeSet) {
    if needs_encoding(byte, set) {
        out.extend(hex_digits(byte).map(char::from));
    } else {
        out.push(char::from(byte));
    }
}

pub fn encode_byte_into(out: &mut Vec<u8>, byte: u8, set: EncodeSet) {
    if needs_encoding(byte, set) {
        out.extend_from_slice(&hex_digits(byte));
    } else {
        out.push(byte);
    }
}

pub fn encode(input: &str, set: EncodeSet) -> String {
    let mut out = String::with_capacity(input.len());
    for &byte in input.as_bytes() {
        encode_byte(&mut out, byte, set);
    }
    out
}

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

pub fn decode(input: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len());
    let mut i = 0;
    while i < input.len() {
        if input[i] == b'%'
            && i + 2 < input.len()
            && let (Some(high), Some(low)) = (hex_value(input[i + 1]), hex_value(input[i + 2]))
        {
            out.push(high << 4 | low);
            i += 3;
            continue;
        }
        out.push(input[i]);
        i += 1;
    }
    out
}

//! Nordstjernen — the URL Standard's host parser and serializer: domains through IDNA, IPv4 and IPv6 addresses and opaque hosts.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use std::borrow::Cow;

use crate::idna;
use crate::percent::{self, EncodeSet};

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Host {
    Domain(String),
    Ipv4(u32),
    Ipv6([u16; 8]),
    Opaque(String),
    Empty,
}

pub fn is_forbidden_host_code_point(c: char) -> bool {
    matches!(
        c,
        '\0' | '\t'
            | '\n'
            | '\r'
            | ' '
            | '#'
            | '/'
            | ':'
            | '<'
            | '>'
            | '?'
            | '@'
            | '['
            | '\\'
            | ']'
            | '^'
            | '|'
    )
}

pub fn is_forbidden_domain_code_point(c: char) -> bool {
    is_forbidden_host_code_point(c) || c <= '\u{1f}' || c == '%' || c == '\u{7f}'
}

pub fn parse(input: &str, opaque: bool) -> Option<Host> {
    if let Some(rest) = input.strip_prefix('[') {
        return rest.strip_suffix(']').and_then(parse_ipv6).map(Host::Ipv6);
    }
    if opaque {
        return parse_opaque(input);
    }
    let decoded;
    let domain = if input.contains('%') {
        decoded = percent::decode(input.as_bytes());
        String::from_utf8_lossy(&decoded)
    } else {
        Cow::Borrowed(input)
    };
    let ascii = idna::domain_to_ascii(&domain)?;
    if ends_in_number(&ascii) {
        return parse_ipv4(&ascii).map(Host::Ipv4);
    }
    Some(Host::Domain(ascii))
}

fn ends_in_number(input: &str) -> bool {
    if input.is_empty() {
        return false;
    }
    let trimmed = input.strip_suffix('.').unwrap_or(input);
    let last = trimmed.rsplit('.').next().unwrap_or_default();
    if !last.is_empty() && last.bytes().all(|b| b.is_ascii_digit()) {
        return true;
    }
    parse_ipv4_number(last).is_some()
}

fn parse_ipv4_number(input: &str) -> Option<u64> {
    if input.is_empty() {
        return None;
    }
    let (digits, radix) =
        if input.len() >= 2 && (input.starts_with("0x") || input.starts_with("0X")) {
            (&input[2..], 16)
        } else if input.len() >= 2 && input.starts_with('0') {
            (&input[1..], 8)
        } else {
            (input, 10)
        };
    let mut value: u64 = 0;
    for c in digits.chars() {
        let digit = c.to_digit(radix)?;
        value = value
            .saturating_mul(u64::from(radix))
            .saturating_add(u64::from(digit));
    }
    Some(value)
}

fn parse_ipv4(input: &str) -> Option<u32> {
    let mut parts: Vec<&str> = input.split('.').collect();
    if parts.last() == Some(&"") && parts.len() > 1 {
        parts.pop();
    }
    if parts.len() > 4 {
        return None;
    }
    let mut numbers = Vec::with_capacity(parts.len());
    for part in parts {
        numbers.push(parse_ipv4_number(part)?);
    }
    let last = numbers.pop()?;
    if numbers.iter().any(|&n| n > 255) {
        return None;
    }
    if last >= 256u64.pow(4 - numbers.len() as u32) {
        return None;
    }
    let mut address = last;
    for (counter, n) in numbers.iter().enumerate() {
        address += n * 256u64.pow(3 - counter as u32);
    }
    u32::try_from(address).ok()
}

fn parse_ipv6(input: &str) -> Option<[u16; 8]> {
    let input: Vec<char> = input.chars().collect();
    let at = |i: usize| input.get(i).copied();
    let mut address = [0u16; 8];
    let mut piece = 0usize;
    let mut compress: Option<usize> = None;
    let mut pointer = 0usize;
    if at(0) == Some(':') {
        if at(1) != Some(':') {
            return None;
        }
        pointer += 2;
        piece += 1;
        compress = Some(piece);
    }
    while let Some(c) = at(pointer) {
        if piece == 8 {
            return None;
        }
        if c == ':' {
            if compress.is_some() {
                return None;
            }
            pointer += 1;
            piece += 1;
            compress = Some(piece);
            continue;
        }
        let mut value: u16 = 0;
        let mut length = 0;
        while length < 4
            && let Some(digit) = at(pointer).and_then(|c| c.to_digit(16))
        {
            value = value * 16 + digit as u16;
            pointer += 1;
            length += 1;
        }
        match at(pointer) {
            Some('.') => {
                if length == 0 {
                    return None;
                }
                pointer -= length;
                if piece > 6 {
                    return None;
                }
                let mut numbers_seen = 0;
                while at(pointer).is_some() {
                    let mut ipv4_piece: Option<u16> = None;
                    if numbers_seen > 0 {
                        if at(pointer) == Some('.') && numbers_seen < 4 {
                            pointer += 1;
                        } else {
                            return None;
                        }
                    }
                    if !at(pointer).is_some_and(|c| c.is_ascii_digit()) {
                        return None;
                    }
                    while let Some(number) = at(pointer).and_then(|c| c.to_digit(10)) {
                        ipv4_piece = match ipv4_piece {
                            None => Some(number as u16),
                            Some(0) => return None,
                            Some(n) => Some(n * 10 + number as u16),
                        };
                        if ipv4_piece > Some(255) {
                            return None;
                        }
                        pointer += 1;
                    }
                    address[piece] = address[piece] * 0x100 + ipv4_piece.unwrap_or(0);
                    numbers_seen += 1;
                    if numbers_seen == 2 || numbers_seen == 4 {
                        piece += 1;
                    }
                }
                if numbers_seen != 4 {
                    return None;
                }
                break;
            }
            Some(':') => {
                pointer += 1;
                at(pointer)?;
            }
            Some(_) => return None,
            None => {}
        }
        address[piece] = value;
        piece += 1;
    }
    match compress {
        Some(compress) => {
            let mut swaps = piece - compress;
            piece = 7;
            while piece != 0 && swaps > 0 {
                address.swap(piece, compress + swaps - 1);
                piece -= 1;
                swaps -= 1;
            }
        }
        None if piece != 8 => return None,
        None => {}
    }
    Some(address)
}

fn parse_opaque(input: &str) -> Option<Host> {
    if input.chars().any(is_forbidden_host_code_point) {
        return None;
    }
    if input.is_empty() {
        return Some(Host::Empty);
    }
    Some(Host::Opaque(percent::encode(input, EncodeSet::C0Control)))
}

fn write_ipv6(address: &[u16; 8], out: &mut String) {
    let mut compress: Option<usize> = None;
    let mut longest = 1;
    let mut i = 0;
    while i < 8 {
        if address[i] == 0 {
            let start = i;
            while i < 8 && address[i] == 0 {
                i += 1;
            }
            if i - start > longest {
                longest = i - start;
                compress = Some(start);
            }
        } else {
            i += 1;
        }
    }
    let mut ignore0 = false;
    for (index, piece) in address.iter().enumerate() {
        if ignore0 && *piece == 0 {
            continue;
        }
        ignore0 = false;
        if compress == Some(index) {
            out.push_str(if index == 0 { "::" } else { ":" });
            ignore0 = true;
            continue;
        }
        out.push_str(&format!("{piece:x}"));
        if index != 7 {
            out.push(':');
        }
    }
}

impl Host {
    pub fn write(&self, out: &mut String) {
        match self {
            Host::Domain(text) | Host::Opaque(text) => out.push_str(text),
            Host::Ipv4(address) => {
                let [a, b, c, d] = address.to_be_bytes();
                out.push_str(&format!("{a}.{b}.{c}.{d}"));
            }
            Host::Ipv6(address) => {
                out.push('[');
                write_ipv6(address, out);
                out.push(']');
            }
            Host::Empty => {}
        }
    }

    pub fn serialize(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }
}

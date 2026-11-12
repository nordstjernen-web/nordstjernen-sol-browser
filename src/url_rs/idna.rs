//! Nordstjernen — UTS #46 ToASCII with the flags the URL Standard's domain to ASCII sets: mapping, NFC, Punycode, the validity criteria, CONTEXTJ and the bidi rule.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::host::is_forbidden_domain_code_point;
use crate::nfc::{self, Bidi, Joining, range_index};
use crate::punycode;
use crate::tables;

const VALID: u32 = 0;
const IGNORED: u32 = 1;
const MAPPED: u32 = 2;
const DEVIATION: u32 = 3;

fn status(c: char) -> u32 {
    tables::IDNA_VALUES[range_index(&tables::IDNA_STARTS, c as u32)]
}

fn map(domain: &str, error: &mut bool) -> Vec<char> {
    let mut out = Vec::with_capacity(domain.len());
    for c in domain.chars() {
        let value = status(c);
        match value & 7 {
            VALID | DEVIATION => out.push(c),
            IGNORED => {}
            MAPPED => {
                let start = (value >> 8) as usize;
                let length = ((value >> 3) & 0x1f) as usize;
                out.extend(
                    tables::IDNA_POOL[start..start + length]
                        .iter()
                        .filter_map(|&cp| char::from_u32(cp)),
                );
            }
            _ => {
                *error = true;
                out.push(c);
            }
        }
    }
    out
}

fn joiners_ok(label: &[char]) -> bool {
    for (i, &c) in label.iter().enumerate() {
        if c != '\u{200c}' && c != '\u{200d}' {
            continue;
        }
        if i > 0 && nfc::combining_class(label[i - 1]) == 9 {
            continue;
        }
        if c == '\u{200d}' {
            return false;
        }
        let before = label[..i]
            .iter()
            .rev()
            .map(|&c| nfc::joining(c))
            .find(|&j| j != Joining::T);
        let after = label[i + 1..]
            .iter()
            .map(|&c| nfc::joining(c))
            .find(|&j| j != Joining::T);
        if !matches!(before, Some(Joining::L | Joining::D))
            || !matches!(after, Some(Joining::R | Joining::D))
        {
            return false;
        }
    }
    true
}

fn bidi_ok(label: &[char]) -> bool {
    let Some(&first) = label.first() else {
        return true;
    };
    let classes: Vec<Bidi> = label.iter().map(|&c| nfc::bidi(c)).collect();
    let last = classes.iter().rev().find(|&&b| b != Bidi::Nsm).copied();
    match nfc::bidi(first) {
        Bidi::R | Bidi::Al => {
            let allowed = classes.iter().all(|b| {
                matches!(
                    b,
                    Bidi::R
                        | Bidi::Al
                        | Bidi::An
                        | Bidi::En
                        | Bidi::Es
                        | Bidi::Cs
                        | Bidi::Et
                        | Bidi::On
                        | Bidi::Bn
                        | Bidi::Nsm
                )
            });
            let ends = matches!(last, Some(Bidi::R | Bidi::Al | Bidi::En | Bidi::An));
            let mixed_numbers = classes.contains(&Bidi::En) && classes.contains(&Bidi::An);
            allowed && ends && !mixed_numbers
        }
        Bidi::L => {
            let allowed = classes.iter().all(|b| {
                matches!(
                    b,
                    Bidi::L
                        | Bidi::En
                        | Bidi::Es
                        | Bidi::Cs
                        | Bidi::Et
                        | Bidi::On
                        | Bidi::Bn
                        | Bidi::Nsm
                )
            });
            allowed && matches!(last, Some(Bidi::L | Bidi::En))
        }
        _ => false,
    }
}

fn label_valid(label: &[char]) -> bool {
    if label.is_empty() {
        return true;
    }
    if nfc::nfc(label) != label {
        return false;
    }
    if label.starts_with(&['x', 'n', '-', '-']) {
        return false;
    }
    if label.contains(&'.') || nfc::is_mark(label[0]) {
        return false;
    }
    if !label
        .iter()
        .all(|&c| matches!(status(c) & 7, VALID | DEVIATION))
    {
        return false;
    }
    joiners_ok(label)
}

fn is_rtl(c: char) -> bool {
    matches!(nfc::bidi(c), Bidi::R | Bidi::Al | Bidi::An)
}

fn to_ascii(domain: &str) -> Option<String> {
    let mut error = false;
    let mapped = nfc::nfc(&map(domain, &mut error));
    let mut labels: Vec<Vec<char>> = Vec::new();
    for label in mapped.split(|&c| c == '.') {
        if label.starts_with(&['x', 'n', '-', '-']) {
            if !label.iter().all(char::is_ascii) {
                error = true;
                labels.push(label.to_vec());
                continue;
            }
            let ascii: String = label[4..].iter().collect();
            match punycode::decode(&ascii) {
                Some(decoded) => {
                    if decoded.is_empty() || decoded.iter().all(char::is_ascii) {
                        error = true;
                    }
                    if !label_valid(&decoded) {
                        error = true;
                    }
                    labels.push(decoded);
                }
                None => {
                    error = true;
                    labels.push(label.to_vec());
                }
            }
        } else {
            if !label_valid(label) {
                error = true;
            }
            labels.push(label.to_vec());
        }
    }
    if labels.iter().flatten().any(|&c| is_rtl(c)) && !labels.iter().all(|l| bidi_ok(l)) {
        error = true;
    }
    if error {
        return None;
    }
    let mut out = String::with_capacity(domain.len());
    for (i, label) in labels.iter().enumerate() {
        if i > 0 {
            out.push('.');
        }
        if label.iter().all(char::is_ascii) {
            out.extend(label.iter());
        } else {
            out.push_str("xn--");
            out.push_str(&punycode::encode(label)?);
        }
    }
    Some(out)
}

pub fn domain_to_ascii(domain: &str) -> Option<String> {
    let result = if domain.is_ascii() {
        domain.to_ascii_lowercase()
    } else {
        to_ascii(domain)?
    };
    if result.is_empty() || result.chars().any(is_forbidden_domain_code_point) {
        return None;
    }
    Some(result)
}

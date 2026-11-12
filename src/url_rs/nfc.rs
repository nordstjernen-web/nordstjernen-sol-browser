//! Nordstjernen — Unicode Normalization Form C and the per-code-point properties IDNA reads, over the generated tables.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::tables;

const S_BASE: u32 = 0xac00;
const L_BASE: u32 = 0x1100;
const V_BASE: u32 = 0x1161;
const T_BASE: u32 = 0x11a7;
const L_COUNT: u32 = 19;
const V_COUNT: u32 = 21;
const T_COUNT: u32 = 28;
const N_COUNT: u32 = V_COUNT * T_COUNT;
const S_COUNT: u32 = L_COUNT * N_COUNT;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Bidi {
    Other,
    L,
    R,
    Al,
    An,
    En,
    Es,
    Cs,
    Et,
    On,
    Bn,
    Nsm,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Joining {
    Other,
    T,
    L,
    R,
    D,
}

pub fn range_index(starts: &[u32], cp: u32) -> usize {
    starts.partition_point(|&start| start <= cp) - 1
}

fn props(c: char) -> u16 {
    tables::PROP_VALUES[range_index(&tables::PROP_STARTS, c as u32)]
}

pub fn combining_class(c: char) -> u8 {
    if c < '\u{300}' {
        0
    } else {
        (props(c) & 0xff) as u8
    }
}

pub fn bidi(c: char) -> Bidi {
    match (props(c) >> 8) & 0xf {
        1 => Bidi::L,
        2 => Bidi::R,
        3 => Bidi::Al,
        4 => Bidi::An,
        5 => Bidi::En,
        6 => Bidi::Es,
        7 => Bidi::Cs,
        8 => Bidi::Et,
        9 => Bidi::On,
        10 => Bidi::Bn,
        11 => Bidi::Nsm,
        _ => Bidi::Other,
    }
}

pub fn joining(c: char) -> Joining {
    match (props(c) >> 12) & 0x7 {
        1 => Joining::T,
        2 => Joining::L,
        3 => Joining::R,
        4 => Joining::D,
        _ => Joining::Other,
    }
}

pub fn is_mark(c: char) -> bool {
    props(c) & 0x8000 != 0
}

fn decompose(c: char, out: &mut Vec<char>) {
    let cp = c as u32;
    if (S_BASE..S_BASE + S_COUNT).contains(&cp) {
        let index = cp - S_BASE;
        let jamo = |v| char::from_u32(v).unwrap_or(c);
        out.push(jamo(L_BASE + index / N_COUNT));
        out.push(jamo(V_BASE + (index % N_COUNT) / T_COUNT));
        if !index.is_multiple_of(T_COUNT) {
            out.push(jamo(T_BASE + index % T_COUNT));
        }
        return;
    }
    match tables::DECOMP_KEYS.binary_search(&cp) {
        Ok(i) => {
            for part in [tables::DECOMP_FIRST[i], tables::DECOMP_SECOND[i]] {
                if let Some(p) = char::from_u32(part).filter(|_| part != 0) {
                    decompose(p, out);
                }
            }
        }
        Err(_) => out.push(c),
    }
}

fn compose(first: char, second: char) -> Option<char> {
    let (a, b) = (first as u32, second as u32);
    if (L_BASE..L_BASE + L_COUNT).contains(&a) && (V_BASE..V_BASE + V_COUNT).contains(&b) {
        return char::from_u32(S_BASE + ((a - L_BASE) * V_COUNT + (b - V_BASE)) * T_COUNT);
    }
    if (S_BASE..S_BASE + S_COUNT).contains(&a)
        && (a - S_BASE).is_multiple_of(T_COUNT)
        && (T_BASE + 1..T_BASE + T_COUNT).contains(&b)
    {
        return char::from_u32(a + (b - T_BASE));
    }
    let key = u64::from(a) << 21 | u64::from(b);
    let i = tables::COMPOSE_KEYS.binary_search(&key).ok()?;
    char::from_u32(tables::COMPOSE_VALUES[i])
}

pub fn nfd(input: &[char]) -> Vec<char> {
    let mut decomposed = Vec::with_capacity(input.len());
    for &c in input {
        decompose(c, &mut decomposed);
    }
    for i in 1..decomposed.len() {
        let mut j = i;
        while j > 0 {
            let class = combining_class(decomposed[j]);
            if class == 0 || combining_class(decomposed[j - 1]) <= class {
                break;
            }
            decomposed.swap(j - 1, j);
            j -= 1;
        }
    }
    decomposed
}

pub fn nfc(input: &[char]) -> Vec<char> {
    let decomposed = nfd(input);
    let mut out: Vec<char> = Vec::with_capacity(decomposed.len());
    let mut starter: Option<usize> = None;
    let mut last_class: i32 = -1;
    for c in decomposed {
        let class = i32::from(combining_class(c));
        if let Some(s) = starter
            && (last_class == -1 || (last_class != 0 && last_class < class))
            && let Some(composed) = compose(out[s], c)
        {
            out[s] = composed;
            continue;
        }
        if class == 0 {
            starter = Some(out.len());
            last_class = -1;
        } else {
            last_class = class;
        }
        out.push(c);
    }
    out
}

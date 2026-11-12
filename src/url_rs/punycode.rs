//! Nordstjernen — Punycode (RFC 3492) encoding and decoding of IDNA labels.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

const BASE: u32 = 36;
const TMIN: u32 = 1;
const TMAX: u32 = 26;
const SKEW: u32 = 38;
const DAMP: u32 = 700;
const INITIAL_BIAS: u32 = 72;
const INITIAL_N: u32 = 128;

fn adapt(delta: u32, points: u32, first: bool) -> u32 {
    let mut delta = if first { delta / DAMP } else { delta / 2 };
    delta += delta / points;
    let mut k = 0;
    while delta > ((BASE - TMIN) * TMAX) / 2 {
        delta /= BASE - TMIN;
        k += BASE;
    }
    k + (BASE - TMIN + 1) * delta / (delta + SKEW)
}

fn threshold(k: u32, bias: u32) -> u32 {
    if k <= bias {
        TMIN
    } else if k >= bias + TMAX {
        TMAX
    } else {
        k - bias
    }
}

fn digit_value(c: u8) -> Option<u32> {
    match c {
        b'a'..=b'z' => Some(u32::from(c - b'a')),
        b'A'..=b'Z' => Some(u32::from(c - b'A')),
        b'0'..=b'9' => Some(u32::from(c - b'0') + 26),
        _ => None,
    }
}

fn digit_char(d: u32) -> char {
    char::from(if d < 26 {
        b'a' + d as u8
    } else {
        b'0' + (d - 26) as u8
    })
}

pub fn decode(input: &str) -> Option<Vec<char>> {
    let bytes = input.as_bytes();
    if !input.is_ascii() {
        return None;
    }
    let (mut output, rest): (Vec<char>, &[u8]) = match bytes.iter().rposition(|&b| b == b'-') {
        Some(0) => (Vec::new(), bytes),
        Some(delimiter) => (
            bytes[..delimiter].iter().map(|&b| char::from(b)).collect(),
            &bytes[delimiter + 1..],
        ),
        None => (Vec::new(), bytes),
    };
    let mut n = INITIAL_N;
    let mut i: u32 = 0;
    let mut bias = INITIAL_BIAS;
    let mut pos = 0;
    while pos < rest.len() {
        let old_i = i;
        let mut w: u32 = 1;
        let mut k = BASE;
        loop {
            let digit = digit_value(*rest.get(pos)?)?;
            pos += 1;
            i = i.checked_add(digit.checked_mul(w)?)?;
            let t = threshold(k, bias);
            if digit < t {
                break;
            }
            w = w.checked_mul(BASE - t)?;
            k += BASE;
        }
        let length = output.len() as u32 + 1;
        bias = adapt(i - old_i, length, old_i == 0);
        n = n.checked_add(i / length)?;
        i %= length;
        output.insert(i as usize, char::from_u32(n)?);
        i += 1;
    }
    Some(output)
}

pub fn encode(input: &[char]) -> Option<String> {
    let mut output: String = input.iter().filter(|c| c.is_ascii()).collect();
    let basic = output.len() as u32;
    let mut handled = basic;
    if basic > 0 {
        output.push('-');
    }
    let mut n = INITIAL_N;
    let mut delta: u32 = 0;
    let mut bias = INITIAL_BIAS;
    let total = input.len() as u32;
    while handled < total {
        let m = input.iter().map(|&c| c as u32).filter(|&c| c >= n).min()?;
        delta = delta.checked_add((m - n).checked_mul(handled + 1)?)?;
        n = m;
        for &c in input {
            let c = c as u32;
            if c < n {
                delta = delta.checked_add(1)?;
            }
            if c == n {
                let mut q = delta;
                let mut k = BASE;
                loop {
                    let t = threshold(k, bias);
                    if q < t {
                        break;
                    }
                    output.push(digit_char(t + (q - t) % (BASE - t)));
                    q = (q - t) / (BASE - t);
                    k += BASE;
                }
                output.push(digit_char(q));
                bias = adapt(delta, handled + 1, handled == basic);
                delta = 0;
                handled += 1;
            }
        }
        delta = delta.checked_add(1)?;
        n += 1;
    }
    Some(output)
}

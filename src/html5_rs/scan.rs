//! Nordstjernen — word-at-a-time byte scans for the tokenizer: the first of a few bytes, and the newlines in a span.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

const ONES: u64 = 0x0101_0101_0101_0101;
const LOW7: u64 = 0x7F7F_7F7F_7F7F_7F7F;

fn lanes_equal(word: u64, byte: u8) -> u64 {
    let x = word ^ (ONES * u64::from(byte));
    !(((x & LOW7) + LOW7) | x | LOW7)
}

fn words(hay: &[u8]) -> (impl Iterator<Item = u64> + '_, &[u8]) {
    let (chunks, rest) = hay.as_chunks::<8>();
    (chunks.iter().map(|chunk| u64::from_le_bytes(*chunk)), rest)
}

pub fn find_any<const N: usize>(hay: &[u8], needles: [u8; N]) -> Option<usize> {
    if hay.first().is_some_and(|b| needles.contains(b)) {
        return Some(0);
    }
    let (words, rest) = words(hay);
    let mut offset = 0;
    for word in words {
        let mask = needles
            .iter()
            .fold(0, |mask, &needle| mask | lanes_equal(word, needle));
        if mask != 0 {
            return Some(offset + (mask.trailing_zeros() / 8) as usize);
        }
        offset += 8;
    }
    rest.iter()
        .position(|b| needles.contains(b))
        .map(|i| offset + i)
}

pub fn newlines(hay: &[u8]) -> (usize, Option<usize>) {
    let (words, rest) = words(hay);
    let mut count = 0;
    let mut last = None;
    let mut offset = 0;
    for word in words {
        let mask = lanes_equal(word, b'\n');
        if mask != 0 {
            count += mask.count_ones() as usize;
            last = Some(offset + 7 - (mask.leading_zeros() / 8) as usize);
        }
        offset += 8;
    }
    for (i, &c) in rest.iter().enumerate() {
        if c == b'\n' {
            count += 1;
            last = Some(offset + i);
        }
    }
    (count, last)
}

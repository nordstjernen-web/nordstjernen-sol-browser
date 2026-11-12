//! Nordstjernen — the URL Standard's basic URL parser: the state machine over the input's UTF-8 bytes, run against a base URL or from a setter's state override.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use std::borrow::Cow;

use crate::host::{self, Host};
use crate::percent::{EncodeSet, encode_byte, encode_byte_into, needs_encoding};
use crate::{Path, Scheme, Url};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum State {
    SchemeStart,
    Scheme,
    NoScheme,
    SpecialRelativeOrAuthority,
    PathOrAuthority,
    Relative,
    RelativeSlash,
    SpecialAuthoritySlashes,
    SpecialAuthorityIgnoreSlashes,
    Authority,
    Host,
    Hostname,
    Port,
    File,
    FileSlash,
    FileHost,
    PathStart,
    Path,
    OpaquePath,
    Query,
    Fragment,
}

fn is_windows_drive_letter(s: &[u8]) -> bool {
    s.len() == 2 && s[0].is_ascii_alphabetic() && matches!(s[1], b':' | b'|')
}

fn is_normalized_windows_drive_letter(s: &[u8]) -> bool {
    is_windows_drive_letter(s) && s[1] == b':'
}

fn starts_with_windows_drive_letter(input: &[u8]) -> bool {
    input.len() >= 2
        && is_windows_drive_letter(&input[..2])
        && (input.len() == 2 || matches!(input[2], b'/' | b'\\' | b'?' | b'#'))
}

fn is_single_dot(segment: &[u8]) -> bool {
    segment == b"." || segment.eq_ignore_ascii_case(b"%2e")
}

fn is_double_dot(segment: &[u8]) -> bool {
    match segment.len() {
        2 => segment == b"..",
        4 => segment.eq_ignore_ascii_case(b".%2e") || segment.eq_ignore_ascii_case(b"%2e."),
        6 => segment.eq_ignore_ascii_case(b"%2e%2e"),
        _ => false,
    }
}

fn first_segment(path: &Path) -> Option<&str> {
    match path {
        Path::List(path) if !path.is_empty() => path[1..].split('/').next(),
        _ => None,
    }
}

fn push_ascii(out: &mut String, bytes: &[u8]) {
    out.extend(bytes.iter().map(|&b| char::from(b)));
}

fn text(bytes: &[u8]) -> Cow<'_, str> {
    String::from_utf8_lossy(bytes)
}

struct Machine<'a> {
    url: &'a mut Url,
    base: Option<&'a Url>,
    input: &'a [u8],
    pointer: isize,
    state: State,
    state_override: Option<State>,
    buffer: Vec<u8>,
    at_sign_seen: bool,
    inside_brackets: bool,
    password_token_seen: bool,
}

impl<'a> Machine<'a> {
    fn at(&self, index: isize) -> Option<u8> {
        usize::try_from(index)
            .ok()
            .and_then(|i| self.input.get(i).copied())
    }

    fn rest(&self) -> &[u8] {
        let start = usize::try_from(self.pointer).unwrap_or(0);
        self.input.get(start..).unwrap_or(&[])
    }

    fn remaining_starts_with(&self, c: u8) -> bool {
        self.at(self.pointer + 1) == Some(c)
    }

    fn special(&self) -> bool {
        self.url.is_special()
    }

    fn push_segment(&mut self, segment: &[u8]) {
        if let Path::List(path) = &mut self.url.path {
            if path.capacity() == 0 {
                path.reserve(self.input.len());
            }
            path.push('/');
            push_ascii(path, segment);
        }
    }

    fn shorten_path(&mut self) {
        let file = self.url.kind == Scheme::File;
        if let Path::List(path) = &mut self.url.path
            && let Some(last) = path.rfind('/')
        {
            if file && last == 0 && is_normalized_windows_drive_letter(&path.as_bytes()[1..]) {
                return;
            }
            path.truncate(last);
        }
    }

    fn remaining_len(&self) -> usize {
        self.input
            .len()
            .saturating_sub(self.pointer.max(0) as usize)
    }

    fn start_query(&mut self) {
        self.url.query = Some(String::with_capacity(self.remaining_len()));
        self.state = State::Query;
    }

    fn start_fragment(&mut self) {
        self.url.fragment = Some(String::with_capacity(self.remaining_len()));
        self.state = State::Fragment;
    }

    fn ends_authority(&self, c: Option<u8>) -> bool {
        matches!(c, None | Some(b'/' | b'?' | b'#')) || (self.special() && c == Some(b'\\'))
    }

    fn take_run(&mut self, plain: impl Fn(u8) -> bool) -> &'a [u8] {
        let input = self.input;
        let start = usize::try_from(self.pointer + 1).unwrap_or(0);
        let rest = input.get(start..).unwrap_or(&[]);
        let length = rest.iter().position(|&b| !plain(b)).unwrap_or(rest.len());
        self.pointer += length as isize;
        &rest[..length]
    }

    fn copy_authority(&mut self, base: &Url) {
        self.url.username = base.username.clone();
        self.url.password = base.password.clone();
        self.url.host = base.host.clone();
        self.url.port = base.port;
    }

    fn run(mut self) -> bool {
        loop {
            let c = self.at(self.pointer);
            if let Some(result) = self.step(c) {
                return result;
            }
            if self.pointer >= self.input.len() as isize {
                return true;
            }
            self.pointer += 1;
        }
    }

    fn step(&mut self, c: Option<u8>) -> Option<bool> {
        match self.state {
            State::SchemeStart => match c {
                Some(c) if c.is_ascii_alphabetic() => {
                    self.buffer.push(c.to_ascii_lowercase());
                    self.state = State::Scheme;
                }
                _ if self.state_override.is_none() => {
                    self.state = State::NoScheme;
                    self.pointer -= 1;
                }
                _ => return Some(false),
            },
            State::Scheme => match c {
                Some(c) if c.is_ascii_alphanumeric() || matches!(c, b'+' | b'-' | b'.') => {
                    self.buffer.push(c.to_ascii_lowercase());
                }
                Some(b':') => return self.scheme_end(),
                _ if self.state_override.is_none() => {
                    self.buffer.clear();
                    self.state = State::NoScheme;
                    self.pointer = -1;
                }
                _ => return Some(false),
            },
            State::NoScheme => {
                let Some(base) = self.base else {
                    return Some(false);
                };
                if base.has_opaque_path() {
                    if c != Some(b'#') {
                        return Some(false);
                    }
                    self.url.copy_scheme(base);
                    self.url.path = base.path.clone();
                    self.url.query = base.query.clone();
                    self.start_fragment();
                } else {
                    self.state = if base.kind == Scheme::File {
                        State::File
                    } else {
                        State::Relative
                    };
                    self.pointer -= 1;
                }
            }
            State::SpecialRelativeOrAuthority => {
                if c == Some(b'/') && self.remaining_starts_with(b'/') {
                    self.state = State::SpecialAuthorityIgnoreSlashes;
                    self.pointer += 1;
                } else {
                    self.state = State::Relative;
                    self.pointer -= 1;
                }
            }
            State::PathOrAuthority => {
                if c == Some(b'/') {
                    self.state = State::Authority;
                } else {
                    self.state = State::Path;
                    self.pointer -= 1;
                }
            }
            State::Relative => {
                let Some(base) = self.base else {
                    return Some(false);
                };
                self.url.copy_scheme(base);
                if c == Some(b'/') || (self.special() && c == Some(b'\\')) {
                    self.state = State::RelativeSlash;
                } else {
                    self.copy_authority(base);
                    self.url.path = base.path.clone();
                    self.url.query = base.query.clone();
                    match c {
                        Some(b'?') => self.start_query(),
                        Some(b'#') => self.start_fragment(),
                        Some(_) => {
                            self.url.query = None;
                            self.shorten_path();
                            self.state = State::Path;
                            self.pointer -= 1;
                        }
                        None => {}
                    }
                }
            }
            State::RelativeSlash => {
                if self.special() && matches!(c, Some(b'/' | b'\\')) {
                    self.state = State::SpecialAuthorityIgnoreSlashes;
                } else if c == Some(b'/') {
                    self.state = State::Authority;
                } else {
                    let Some(base) = self.base else {
                        return Some(false);
                    };
                    self.copy_authority(base);
                    self.state = State::Path;
                    self.pointer -= 1;
                }
            }
            State::SpecialAuthoritySlashes => {
                self.state = State::SpecialAuthorityIgnoreSlashes;
                if c == Some(b'/') && self.remaining_starts_with(b'/') {
                    self.pointer += 1;
                } else {
                    self.pointer -= 1;
                }
            }
            State::SpecialAuthorityIgnoreSlashes => {
                if !matches!(c, Some(b'/' | b'\\')) {
                    self.state = State::Authority;
                    self.pointer -= 1;
                }
            }
            State::Authority => return self.authority_step(c),
            State::Host | State::Hostname => return self.host_step(c),
            State::Port => return self.port_step(c),
            State::File => self.file_step(c),
            State::FileSlash => {
                if matches!(c, Some(b'/' | b'\\')) {
                    self.state = State::FileHost;
                } else {
                    if let Some(base) = self.base.filter(|b| b.kind == Scheme::File) {
                        self.url.host = base.host.clone();
                        if !starts_with_windows_drive_letter(self.rest())
                            && let Some(first) = first_segment(&base.path)
                            && is_normalized_windows_drive_letter(first.as_bytes())
                        {
                            self.push_segment(first.as_bytes());
                        }
                    }
                    self.state = State::Path;
                    self.pointer -= 1;
                }
            }
            State::FileHost => return self.file_host_step(c),
            State::PathStart => {
                if self.special() {
                    self.state = State::Path;
                    if !matches!(c, Some(b'/' | b'\\')) {
                        self.pointer -= 1;
                    }
                } else if self.state_override.is_none() && c == Some(b'?') {
                    self.start_query();
                } else if self.state_override.is_none() && c == Some(b'#') {
                    self.start_fragment();
                } else if c.is_some() {
                    self.state = State::Path;
                    if c != Some(b'/') {
                        self.pointer -= 1;
                    }
                } else if self.state_override.is_some() && self.url.host.is_none() {
                    self.push_segment(b"");
                }
            }
            State::Path => self.path_step(c),
            State::OpaquePath => match c {
                Some(b'?') => self.start_query(),
                Some(b'#') => self.start_fragment(),
                Some(c) => {
                    let space_before_delimiter = c == b' '
                        && (self.remaining_starts_with(b'?') || self.remaining_starts_with(b'#'));
                    let run = self.take_run(|b| {
                        !needs_encoding(b, EncodeSet::C0Control) && !matches!(b, b' ' | b'?' | b'#')
                    });
                    if let Path::Opaque(path) = &mut self.url.path {
                        if space_before_delimiter {
                            path.push_str("%20");
                        } else {
                            encode_byte(path, c, EncodeSet::C0Control);
                        }
                        push_ascii(path, run);
                    }
                }
                None => {}
            },
            State::Query => {
                if (self.state_override.is_none() && c == Some(b'#')) || c.is_none() {
                    if c == Some(b'#') {
                        self.start_fragment();
                    }
                } else if let Some(c) = c {
                    let set = if self.special() {
                        EncodeSet::SpecialQuery
                    } else {
                        EncodeSet::Query
                    };
                    let run = self.take_run(|b| !needs_encoding(b, set) && b != b'#');
                    let query = self.url.query.get_or_insert_default();
                    encode_byte(query, c, set);
                    push_ascii(query, run);
                }
            }
            State::Fragment => {
                if let Some(c) = c {
                    let run = self.take_run(|b| !needs_encoding(b, EncodeSet::Fragment));
                    let fragment = self.url.fragment.get_or_insert_default();
                    encode_byte(fragment, c, EncodeSet::Fragment);
                    push_ascii(fragment, run);
                }
            }
        }
        None
    }

    fn scheme_end(&mut self) -> Option<bool> {
        if self.state_override.is_some() {
            let special = Scheme::of(&text(&self.buffer)).is_special();
            if self.special() != special
                || ((self.url.includes_credentials() || self.url.port.is_some())
                    && self.buffer == b"file")
                || (self.url.kind == Scheme::File && self.url.host == Some(Host::Empty))
            {
                return Some(true);
            }
        }
        self.url.set_scheme(&text(&self.buffer));
        self.buffer.clear();
        if self.state_override.is_some() {
            if self.url.port.is_some() && self.url.port == self.url.kind.default_port() {
                self.url.port = None;
            }
            return Some(true);
        }
        if self.url.kind == Scheme::File {
            self.state = State::File;
        } else if self.special() && self.base.is_some_and(|b| b.scheme == self.url.scheme) {
            self.state = State::SpecialRelativeOrAuthority;
        } else if self.special() {
            self.state = State::SpecialAuthoritySlashes;
        } else if self.remaining_starts_with(b'/') {
            self.state = State::PathOrAuthority;
            self.pointer += 1;
        } else {
            self.url.path = Path::Opaque(String::new());
            self.state = State::OpaquePath;
        }
        None
    }

    fn authority_step(&mut self, c: Option<u8>) -> Option<bool> {
        if c == Some(b'@') {
            if self.at_sign_seen {
                self.buffer.splice(0..0, *b"%40");
            }
            self.at_sign_seen = true;
            for &byte in &self.buffer {
                if byte == b':' && !self.password_token_seen {
                    self.password_token_seen = true;
                    continue;
                }
                let target = if self.password_token_seen {
                    &mut self.url.password
                } else {
                    &mut self.url.username
                };
                encode_byte(target, byte, EncodeSet::Userinfo);
            }
            self.buffer.clear();
        } else if self.ends_authority(c) {
            if self.at_sign_seen && self.buffer.is_empty() {
                return Some(false);
            }
            self.pointer -= self.buffer.len() as isize + 1;
            self.buffer.clear();
            self.state = State::Host;
        } else if let Some(c) = c {
            self.buffer.push(c);
        }
        None
    }

    fn host_step(&mut self, c: Option<u8>) -> Option<bool> {
        if self.state_override.is_some() && self.url.kind == Scheme::File {
            self.pointer -= 1;
            self.state = State::FileHost;
        } else if c == Some(b':') && !self.inside_brackets {
            if self.buffer.is_empty() || self.state_override == Some(State::Hostname) {
                return Some(false);
            }
            let Some(host) = host::parse(&text(&self.buffer), !self.special()) else {
                return Some(false);
            };
            self.url.host = Some(host);
            self.buffer.clear();
            self.state = State::Port;
        } else if self.ends_authority(c) {
            self.pointer -= 1;
            if self.buffer.is_empty()
                && (self.special()
                    || (self.state_override.is_some()
                        && (self.url.includes_credentials() || self.url.port.is_some())))
            {
                return Some(false);
            }
            let Some(host) = host::parse(&text(&self.buffer), !self.special()) else {
                return Some(false);
            };
            self.url.host = Some(host);
            self.buffer.clear();
            self.state = State::PathStart;
            if self.state_override.is_some() {
                return Some(true);
            }
        } else if let Some(c) = c {
            match c {
                b'[' => self.inside_brackets = true,
                b']' => self.inside_brackets = false,
                _ => {}
            }
            self.buffer.push(c);
        }
        None
    }

    fn port_step(&mut self, c: Option<u8>) -> Option<bool> {
        match c {
            Some(d) if d.is_ascii_digit() => self.buffer.push(d),
            _ if self.ends_authority(c) || self.state_override.is_some() => {
                if !self.buffer.is_empty() {
                    let mut port: u32 = 0;
                    for digit in &self.buffer {
                        port = port * 10 + u32::from(digit - b'0');
                        if port > 65535 {
                            return Some(false);
                        }
                    }
                    let port = port as u16;
                    self.url.port = (Some(port) != self.url.kind.default_port()).then_some(port);
                    self.buffer.clear();
                    if self.state_override.is_some() {
                        return Some(true);
                    }
                }
                if self.state_override.is_some() {
                    return Some(false);
                }
                self.state = State::PathStart;
                self.pointer -= 1;
            }
            _ => return Some(false),
        }
        None
    }

    fn file_step(&mut self, c: Option<u8>) {
        self.url.set_scheme("file");
        self.url.host = Some(Host::Empty);
        if matches!(c, Some(b'/' | b'\\')) {
            self.state = State::FileSlash;
            return;
        }
        let Some(base) = self.base.filter(|b| b.kind == Scheme::File) else {
            self.state = State::Path;
            self.pointer -= 1;
            return;
        };
        self.url.host = base.host.clone();
        self.url.path = base.path.clone();
        self.url.query = base.query.clone();
        match c {
            Some(b'?') => self.start_query(),
            Some(b'#') => self.start_fragment(),
            Some(_) => {
                self.url.query = None;
                if starts_with_windows_drive_letter(self.rest()) {
                    self.url.path = Path::List(String::new());
                } else {
                    self.shorten_path();
                }
                self.state = State::Path;
                self.pointer -= 1;
            }
            None => {}
        }
    }

    fn file_host_step(&mut self, c: Option<u8>) -> Option<bool> {
        match c {
            None | Some(b'/' | b'\\' | b'?' | b'#') => {
                self.pointer -= 1;
                if self.state_override.is_none() && is_windows_drive_letter(&self.buffer) {
                    self.state = State::Path;
                } else if self.buffer.is_empty() {
                    self.url.host = Some(Host::Empty);
                    if self.state_override.is_some() {
                        return Some(true);
                    }
                    self.state = State::PathStart;
                } else {
                    let Some(mut host) = host::parse(&text(&self.buffer), !self.special()) else {
                        return Some(false);
                    };
                    if matches!(&host, Host::Domain(d) if d == "localhost") {
                        host = Host::Empty;
                    }
                    self.url.host = Some(host);
                    if self.state_override.is_some() {
                        return Some(true);
                    }
                    self.buffer.clear();
                    self.state = State::PathStart;
                }
            }
            Some(c) => self.buffer.push(c),
        }
        None
    }

    fn path_step(&mut self, c: Option<u8>) {
        let slash = c == Some(b'/') || (self.special() && c == Some(b'\\'));
        if c.is_none() || slash || (self.state_override.is_none() && matches!(c, Some(b'?' | b'#')))
        {
            let mut buffer = core::mem::take(&mut self.buffer);
            if is_double_dot(&buffer) {
                self.shorten_path();
                if !slash {
                    self.push_segment(b"");
                }
            } else if is_single_dot(&buffer) {
                if !slash {
                    self.push_segment(b"");
                }
            } else {
                let empty_path = matches!(&self.url.path, Path::List(p) if p.is_empty());
                if self.url.kind == Scheme::File && empty_path && is_windows_drive_letter(&buffer) {
                    buffer[1] = b':';
                }
                self.push_segment(&buffer);
            }
            buffer.clear();
            self.buffer = buffer;
            match c {
                Some(b'?') => self.start_query(),
                Some(b'#') => self.start_fragment(),
                _ => {}
            }
        } else if let Some(c) = c {
            encode_byte_into(&mut self.buffer, c, EncodeSet::Path);
            let run = self
                .take_run(|b| !needs_encoding(b, EncodeSet::Path) && !matches!(b, b'/' | b'\\'));
            self.buffer.extend_from_slice(run);
        }
    }
}

fn remove_tab_and_newline(input: &[u8]) -> Cow<'_, [u8]> {
    if input.iter().any(|b| matches!(b, b'\t' | b'\n' | b'\r')) {
        Cow::Owned(
            input
                .iter()
                .copied()
                .filter(|b| !matches!(b, b'\t' | b'\n' | b'\r'))
                .collect(),
        )
    } else {
        Cow::Borrowed(input)
    }
}

pub fn parse(input: &str, base: Option<&Url>) -> Option<Url> {
    let mut url = Url::empty();
    let trimmed = input.trim_matches(|c: char| c <= ' ');
    let ok = run(
        &mut url,
        base,
        &remove_tab_and_newline(trimmed.as_bytes()),
        None,
    );
    ok.then_some(url)
}

pub fn parse_with_override(url: &mut Url, input: &str, state: State) -> bool {
    run(
        url,
        None,
        &remove_tab_and_newline(input.as_bytes()),
        Some(state),
    )
}

fn run(url: &mut Url, base: Option<&Url>, input: &[u8], state_override: Option<State>) -> bool {
    Machine {
        url,
        base,
        input,
        pointer: 0,
        state: state_override.unwrap_or(State::SchemeStart),
        state_override,
        buffer: Vec::with_capacity(input.len()),
        at_sign_seen: false,
        inside_brackets: false,
        password_token_seen: false,
    }
    .run()
}

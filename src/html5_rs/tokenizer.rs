//! Nordstjernen — the WHATWG HTML tokenizer over a whole, newline-normalized input, with zero-copy text runs and reused token buffers.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::charref;
use crate::tags::{AttrNs, Tag};
use crate::{Attribute, Position};

const REPLACEMENT: &[u8] = "\u{FFFD}".as_bytes();

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Data,
    Rcdata,
    Rawtext,
    ScriptData,
    Plaintext,
    TagOpen,
    EndTagOpen,
    TagName,
    ScriptEscapeStart,
    ScriptEscapeStartDash,
    ScriptEscaped,
    ScriptEscapedDash,
    ScriptEscapedDashDash,
    ScriptEscapedLt,
    ScriptDoubleEscaped,
    ScriptDoubleEscapedDash,
    ScriptDoubleEscapedDashDash,
    ScriptDoubleEscapedLt,
    BeforeAttrName,
    AttrName,
    AfterAttrName,
    BeforeAttrValue,
    AttrValueDq,
    AttrValueSq,
    AttrValueUnquoted,
    AfterAttrValueQuoted,
    SelfClosing,
    BogusComment,
    MarkupDecl,
    CommentStart,
    CommentStartDash,
    Comment,
    CommentLt,
    CommentLtBang,
    CommentLtBangDash,
    CommentLtBangDashDash,
    CommentEndDash,
    CommentEnd,
    CommentEndBang,
    Doctype,
    BeforeDoctypeName,
    DoctypeName,
    AfterDoctypeName,
    AfterPublicKeyword,
    BeforePublicId,
    PublicIdDq,
    PublicIdSq,
    AfterPublicId,
    BetweenIds,
    AfterSystemKeyword,
    BeforeSystemId,
    SystemIdDq,
    SystemIdSq,
    AfterSystemId,
    BogusDoctype,
    CdataSection,
    PiOpen,
    PiTarget,
    AfterPiTarget,
    PiData,
    PiQuestion,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Chars,
    StartTag,
    EndTag,
    Comment,
    Pi,
    Doctype,
    Eof,
}

#[derive(Clone, Debug)]
pub struct Attr {
    pub name: Vec<u8>,
    pub value: Vec<u8>,
    pub ns: AttrNs,
    pub prefix_len: usize,
}

impl Attr {
    pub fn view(&self) -> Attribute<'_> {
        Attribute {
            name: &self.name,
            value: &self.value,
            ns: self.ns,
            prefix_len: self.prefix_len,
        }
    }
}

#[derive(Default, Debug)]
pub struct TagToken {
    pub name: Vec<u8>,
    pub tag: Option<Tag>,
    pub self_closing: bool,
    pub attrs: Vec<Attr>,
    pub count: usize,
    pub position: Option<Position>,
}

impl TagToken {
    pub fn attrs(&self) -> &[Attr] {
        &self.attrs[..self.count]
    }

    pub fn attrs_mut(&mut self) -> &mut [Attr] {
        &mut self.attrs[..self.count]
    }

    pub fn tag(&self) -> Tag {
        self.tag.unwrap_or(Tag::Other)
    }

    pub fn attr(&self, name: &[u8]) -> Option<&[u8]> {
        self.attrs()
            .iter()
            .find(|a| a.name == name)
            .map(|a| a.value.as_slice())
    }
}

#[derive(Default, Debug)]
pub struct Doctype {
    pub name: Option<Vec<u8>>,
    pub public_id: Option<Vec<u8>>,
    pub system_id: Option<Vec<u8>>,
    pub force_quirks: bool,
}

pub struct Tokenizer<'a> {
    input: &'a [u8],
    pos: usize,
    pub state: State,
    pub allow_cdata: bool,
    text_start: usize,
    text_end: usize,
    text_buf: Vec<u8>,
    text_in_buf: bool,
    text_emitted: bool,
    scratch: Vec<u8>,
    pub tag: TagToken,
    pub end_tag: bool,
    drop_attr: bool,
    pub comment: Vec<u8>,
    pub pi_target: Vec<u8>,
    pub doctype: Doctype,
    last_start_tag: Vec<u8>,
    pending: Option<Kind>,
    done: bool,
    line: u32,
    line_start: usize,
    line_scan: usize,
}

fn is_ws(c: u8) -> bool {
    matches!(c, b'\t' | b'\n' | b'\x0c' | b' ')
}

impl<'a> Tokenizer<'a> {
    pub fn new(input: &'a [u8]) -> Self {
        Tokenizer {
            input,
            pos: 0,
            state: State::Data,
            allow_cdata: false,
            text_start: 0,
            text_end: 0,
            text_buf: Vec::new(),
            text_in_buf: false,
            text_emitted: false,
            scratch: Vec::new(),
            tag: TagToken::default(),
            end_tag: false,
            drop_attr: false,
            comment: Vec::new(),
            pi_target: Vec::new(),
            doctype: Doctype::default(),
            last_start_tag: Vec::new(),
            pending: None,
            done: false,
            line: 1,
            line_start: 0,
            line_scan: 0,
        }
    }

    pub fn text(&self) -> &[u8] {
        if self.text_in_buf {
            &self.text_buf
        } else {
            &self.input[self.text_start..self.text_end]
        }
    }

    fn text_is_empty(&self) -> bool {
        if self.text_in_buf {
            self.text_buf.is_empty()
        } else {
            self.text_start == self.text_end
        }
    }

    fn clear_text(&mut self) {
        self.text_in_buf = false;
        self.text_buf.clear();
        self.text_start = 0;
        self.text_end = 0;
    }

    fn push_input(&mut self, start: usize, end: usize) {
        if start == end {
            return;
        }
        if self.text_in_buf {
            self.text_buf.extend_from_slice(&self.input[start..end]);
        } else if self.text_start == self.text_end {
            self.text_start = start;
            self.text_end = end;
        } else if self.text_end == start {
            self.text_end = end;
        } else {
            self.text_buf
                .extend_from_slice(&self.input[self.text_start..self.text_end]);
            self.text_buf.extend_from_slice(&self.input[start..end]);
            self.text_in_buf = true;
        }
    }

    fn push_bytes(&mut self, bytes: &[u8]) {
        if !self.text_in_buf {
            self.text_buf
                .extend_from_slice(&self.input[self.text_start..self.text_end]);
            self.text_in_buf = true;
        }
        self.text_buf.extend_from_slice(bytes);
    }

    fn push_byte_at(&mut self, at: usize) {
        if self.input[at] == 0 {
            self.push_bytes(REPLACEMENT);
        } else {
            self.push_input(at, at + 1);
        }
    }

    fn position_at(&mut self, offset: usize) -> Position {
        let offset = offset.min(self.input.len());
        if offset >= self.line_scan {
            for (i, &c) in self.input[self.line_scan..offset].iter().enumerate() {
                if c == b'\n' {
                    self.line += 1;
                    self.line_start = self.line_scan + i + 1;
                }
            }
            self.line_scan = offset;
        }
        Position {
            line: self.line,
            column: (offset - self.line_start + 1) as u32,
        }
    }

    pub fn next_token(&mut self) -> Kind {
        if self.text_emitted {
            self.text_emitted = false;
            self.clear_text();
        }
        if let Some(kind) = self.pending.take() {
            return kind;
        }
        if self.done {
            return Kind::Eof;
        }
        loop {
            if let Some(kind) = self.step() {
                if kind == Kind::Chars {
                    self.text_emitted = true;
                }
                return kind;
            }
        }
    }

    fn emit(&mut self, kind: Kind) -> Option<Kind> {
        if self.text_is_empty() {
            Some(kind)
        } else {
            self.pending = Some(kind);
            Some(Kind::Chars)
        }
    }

    fn eof(&mut self) -> Option<Kind> {
        self.done = true;
        self.pos = self.input.len();
        self.emit(Kind::Eof)
    }

    fn peek(&self) -> Option<u8> {
        self.input.get(self.pos).copied()
    }

    fn starts_with_ci(&self, word: &[u8]) -> bool {
        self.input
            .get(self.pos..self.pos + word.len())
            .is_some_and(|s| s.eq_ignore_ascii_case(word))
    }

    fn new_tag(&mut self, end: bool) {
        self.tag.name.clear();
        self.tag.tag = None;
        self.tag.self_closing = false;
        self.tag.count = 0;
        self.tag.position = None;
        self.end_tag = end;
        self.drop_attr = false;
    }

    fn emit_tag(&mut self) -> Option<Kind> {
        self.finish_attr();
        self.state = State::Data;
        self.tag.tag = Some(Tag::from_bytes(&self.tag.name));
        if self.end_tag {
            return self.emit(Kind::EndTag);
        }
        self.last_start_tag.clear();
        self.last_start_tag.extend_from_slice(&self.tag.name);
        self.tag.position = Some(self.position_at(self.pos));
        self.emit(Kind::StartTag)
    }

    fn start_attr(&mut self) {
        self.finish_attr();
        if self.tag.count == self.tag.attrs.len() {
            self.tag.attrs.push(Attr {
                name: Vec::new(),
                value: Vec::new(),
                ns: AttrNs::None,
                prefix_len: 0,
            });
        }
        let attr = &mut self.tag.attrs[self.tag.count];
        attr.name.clear();
        attr.value.clear();
        attr.ns = AttrNs::None;
        attr.prefix_len = 0;
        self.tag.count += 1;
    }

    fn finish_attr(&mut self) {
        if self.drop_attr {
            self.drop_attr = false;
            self.tag.count -= 1;
        }
    }

    fn check_duplicate_attr(&mut self) {
        let count = self.tag.count;
        if count < 2 {
            return;
        }
        let (earlier, current) = self.tag.attrs.split_at(count - 1);
        let name = &current[0].name;
        if earlier.iter().any(|a| &a.name == name) {
            self.drop_attr = true;
        }
    }

    fn current_attr(&mut self) -> &mut Attr {
        let i = self.tag.count - 1;
        &mut self.tag.attrs[i]
    }

    fn appropriate_end_tag(&self, at: usize) -> Option<usize> {
        let name = &self.last_start_tag;
        if name.is_empty() {
            return None;
        }
        let end = at + name.len();
        let candidate = self.input.get(at..end)?;
        if !candidate.eq_ignore_ascii_case(name) {
            return None;
        }
        match self.input.get(end) {
            Some(&c) if is_ws(c) || c == b'/' || c == b'>' => Some(end),
            _ => None,
        }
    }

    fn begin_text_end_tag(&mut self, name_end: usize) {
        self.new_tag(true);
        self.tag.name.extend_from_slice(&self.last_start_tag);
        self.pos = name_end;
        self.state = State::BeforeAttrName;
    }

    fn charref_into_text(&mut self, amp: usize) {
        let mut scratch = core::mem::take(&mut self.scratch);
        scratch.clear();
        let next = charref::consume(self.input, amp + 1, false, &mut scratch);
        if next == amp + 1 && scratch == b"&" {
            self.push_input(amp, amp + 1);
        } else {
            self.push_bytes(&scratch);
        }
        self.pos = next;
        self.scratch = scratch;
    }

    fn charref_into_attr(&mut self, amp: usize) {
        let mut scratch = core::mem::take(&mut self.scratch);
        scratch.clear();
        self.pos = charref::consume(self.input, amp + 1, true, &mut scratch);
        self.current_attr().value.extend_from_slice(&scratch);
        self.scratch = scratch;
    }

    fn step(&mut self) -> Option<Kind> {
        match self.state {
            State::Data => self.data(),
            State::Rcdata => self.rcdata(),
            State::Rawtext => self.rawtext(),
            State::ScriptData => self.script_data(),
            State::Plaintext => self.plaintext(),
            State::TagOpen => self.tag_open(),
            State::EndTagOpen => self.end_tag_open(),
            State::TagName => self.tag_name(),
            State::ScriptEscapeStart
            | State::ScriptEscapeStartDash
            | State::ScriptEscaped
            | State::ScriptEscapedDash
            | State::ScriptEscapedDashDash
            | State::ScriptEscapedLt
            | State::ScriptDoubleEscaped
            | State::ScriptDoubleEscapedDash
            | State::ScriptDoubleEscapedDashDash
            | State::ScriptDoubleEscapedLt => self.script_escape(),
            State::BeforeAttrName => self.before_attr_name(),
            State::AttrName => self.attr_name(),
            State::AfterAttrName => self.after_attr_name(),
            State::BeforeAttrValue => self.before_attr_value(),
            State::AttrValueDq => self.attr_value_quoted(b'"'),
            State::AttrValueSq => self.attr_value_quoted(b'\''),
            State::AttrValueUnquoted => self.attr_value_unquoted(),
            State::AfterAttrValueQuoted => self.after_attr_value_quoted(),
            State::SelfClosing => self.self_closing(),
            State::BogusComment => self.bogus_comment(),
            State::MarkupDecl => self.markup_decl(),
            State::CommentStart
            | State::CommentStartDash
            | State::Comment
            | State::CommentLt
            | State::CommentLtBang
            | State::CommentLtBangDash
            | State::CommentLtBangDashDash
            | State::CommentEndDash
            | State::CommentEnd
            | State::CommentEndBang => self.comment_states(),
            State::Doctype
            | State::BeforeDoctypeName
            | State::DoctypeName
            | State::AfterDoctypeName
            | State::AfterPublicKeyword
            | State::BeforePublicId
            | State::PublicIdDq
            | State::PublicIdSq
            | State::AfterPublicId
            | State::BetweenIds
            | State::AfterSystemKeyword
            | State::BeforeSystemId
            | State::SystemIdDq
            | State::SystemIdSq
            | State::AfterSystemId
            | State::BogusDoctype => self.doctype_states(),
            State::CdataSection => self.cdata_section(),
            State::PiOpen
            | State::PiTarget
            | State::AfterPiTarget
            | State::PiData
            | State::PiQuestion => self.pi_states(),
        }
    }

    fn data(&mut self) -> Option<Kind> {
        let start = self.pos;
        let found = self.input[start..]
            .iter()
            .position(|&b| b == b'<' || b == b'&');
        let Some(i) = found else {
            self.push_input(start, self.input.len());
            return self.eof();
        };
        let at = start + i;
        self.push_input(start, at);
        if self.input[at] == b'&' {
            self.charref_into_text(at);
        } else {
            self.pos = at + 1;
            self.state = State::TagOpen;
        }
        None
    }

    fn rcdata(&mut self) -> Option<Kind> {
        let start = self.pos;
        let found = self.input[start..]
            .iter()
            .position(|&b| b == b'<' || b == b'&' || b == 0);
        let Some(i) = found else {
            self.push_input(start, self.input.len());
            return self.eof();
        };
        let at = start + i;
        self.push_input(start, at);
        match self.input[at] {
            b'&' => self.charref_into_text(at),
            0 => {
                self.push_bytes(REPLACEMENT);
                self.pos = at + 1;
            }
            _ => self.text_less_than(at),
        }
        None
    }

    fn text_less_than(&mut self, at: usize) {
        if self.input.get(at + 1) == Some(&b'/')
            && let Some(end) = self.appropriate_end_tag(at + 2)
        {
            self.begin_text_end_tag(end);
            return;
        }
        self.push_input(at, at + 1);
        self.pos = at + 1;
    }

    fn rawtext(&mut self) -> Option<Kind> {
        let start = self.pos;
        let found = self.input[start..]
            .iter()
            .position(|&b| b == b'<' || b == 0);
        let Some(i) = found else {
            self.push_input(start, self.input.len());
            return self.eof();
        };
        let at = start + i;
        self.push_input(start, at);
        if self.input[at] == 0 {
            self.push_bytes(REPLACEMENT);
            self.pos = at + 1;
        } else {
            self.text_less_than(at);
        }
        None
    }

    fn script_data(&mut self) -> Option<Kind> {
        let start = self.pos;
        let found = self.input[start..]
            .iter()
            .position(|&b| b == b'<' || b == 0);
        let Some(i) = found else {
            self.push_input(start, self.input.len());
            return self.eof();
        };
        let at = start + i;
        self.push_input(start, at);
        if self.input[at] == 0 {
            self.push_bytes(REPLACEMENT);
            self.pos = at + 1;
        } else if self.input.get(at + 1) == Some(&b'!') {
            self.push_input(at, at + 2);
            self.pos = at + 2;
            self.state = State::ScriptEscapeStart;
        } else {
            self.text_less_than(at);
        }
        None
    }

    fn plaintext(&mut self) -> Option<Kind> {
        let start = self.pos;
        match self.input[start..].iter().position(|&b| b == 0) {
            None => {
                self.push_input(start, self.input.len());
                self.eof()
            }
            Some(i) => {
                self.push_input(start, start + i);
                self.push_bytes(REPLACEMENT);
                self.pos = start + i + 1;
                None
            }
        }
    }

    fn alpha_run_is_script(&self, at: usize) -> bool {
        let run = self.input[at..]
            .iter()
            .take_while(|c| c.is_ascii_alphabetic())
            .count();
        run == 6
            && self.input[at..at + 6].eq_ignore_ascii_case(b"script")
            && self
                .input
                .get(at + 6)
                .is_some_and(|&c| is_ws(c) || c == b'/' || c == b'>')
    }

    fn script_escape(&mut self) -> Option<Kind> {
        let Some(c) = self.peek() else {
            if self.state == State::ScriptEscapedLt {
                let at = self.pos;
                self.push_input(at - 1, at);
            }
            return self.eof();
        };
        let at = self.pos;
        match self.state {
            State::ScriptEscapeStart | State::ScriptEscapeStartDash => {
                if c == b'-' {
                    self.push_input(at, at + 1);
                    self.pos += 1;
                    self.state = if self.state == State::ScriptEscapeStart {
                        State::ScriptEscapeStartDash
                    } else {
                        State::ScriptEscapedDashDash
                    };
                } else {
                    self.state = State::ScriptData;
                }
            }
            State::ScriptEscaped | State::ScriptEscapedDash | State::ScriptEscapedDashDash => {
                self.pos += 1;
                match c {
                    b'-' => {
                        self.push_input(at, at + 1);
                        self.state = match self.state {
                            State::ScriptEscaped => State::ScriptEscapedDash,
                            _ => State::ScriptEscapedDashDash,
                        };
                    }
                    b'<' => self.state = State::ScriptEscapedLt,
                    b'>' if self.state == State::ScriptEscapedDashDash => {
                        self.push_input(at, at + 1);
                        self.state = State::ScriptData;
                    }
                    _ => {
                        self.push_byte_at(at);
                        self.state = State::ScriptEscaped;
                    }
                }
            }
            State::ScriptEscapedLt => {
                let lt = at - 1;
                if c == b'/' {
                    if let Some(end) = self.appropriate_end_tag(at + 1) {
                        self.begin_text_end_tag(end);
                        return None;
                    }
                    self.push_input(lt, at + 1);
                    self.pos = at + 1;
                    self.state = State::ScriptEscaped;
                } else if c.is_ascii_alphabetic() {
                    self.push_input(lt, at);
                    self.state = if self.alpha_run_is_script(at) {
                        self.push_input(at, at + 7);
                        self.pos = at + 7;
                        State::ScriptDoubleEscaped
                    } else {
                        State::ScriptEscaped
                    };
                } else {
                    self.push_input(lt, at);
                    self.state = State::ScriptEscaped;
                }
            }
            State::ScriptDoubleEscaped
            | State::ScriptDoubleEscapedDash
            | State::ScriptDoubleEscapedDashDash => {
                self.pos += 1;
                match c {
                    b'-' => {
                        self.push_input(at, at + 1);
                        self.state = match self.state {
                            State::ScriptDoubleEscaped => State::ScriptDoubleEscapedDash,
                            _ => State::ScriptDoubleEscapedDashDash,
                        };
                    }
                    b'<' => {
                        self.push_input(at, at + 1);
                        self.state = State::ScriptDoubleEscapedLt;
                    }
                    b'>' if self.state == State::ScriptDoubleEscapedDashDash => {
                        self.push_input(at, at + 1);
                        self.state = State::ScriptData;
                    }
                    _ => {
                        self.push_byte_at(at);
                        self.state = State::ScriptDoubleEscaped;
                    }
                }
            }
            _ => {
                if c == b'/' {
                    self.push_input(at, at + 1);
                    self.pos = at + 1;
                    if self.alpha_run_is_script(at + 1) {
                        self.push_input(at + 1, at + 8);
                        self.pos = at + 8;
                        self.state = State::ScriptEscaped;
                        return None;
                    }
                }
                self.state = State::ScriptDoubleEscaped;
            }
        }
        None
    }

    fn tag_open(&mut self) -> Option<Kind> {
        let lt = self.pos - 1;
        match self.peek() {
            Some(b'!') => {
                self.pos += 1;
                self.state = State::MarkupDecl;
            }
            Some(b'/') => {
                self.pos += 1;
                self.state = State::EndTagOpen;
            }
            Some(c) if c.is_ascii_alphabetic() => {
                self.new_tag(false);
                self.state = State::TagName;
            }
            Some(b'?') => {
                self.pos += 1;
                self.state = State::PiOpen;
            }
            None => {
                self.push_input(lt, lt + 1);
                return self.eof();
            }
            Some(_) => {
                self.push_input(lt, lt + 1);
                self.state = State::Data;
            }
        }
        None
    }

    fn end_tag_open(&mut self) -> Option<Kind> {
        match self.peek() {
            Some(c) if c.is_ascii_alphabetic() => {
                self.new_tag(true);
                self.state = State::TagName;
            }
            Some(b'>') => {
                self.pos += 1;
                self.state = State::Data;
            }
            None => {
                let at = self.pos;
                self.push_input(at - 2, at);
                return self.eof();
            }
            Some(_) => {
                self.comment.clear();
                self.state = State::BogusComment;
            }
        }
        None
    }

    fn tag_name(&mut self) -> Option<Kind> {
        while let Some(c) = self.peek() {
            self.pos += 1;
            match c {
                b'\t' | b'\n' | b'\x0c' | b' ' => {
                    self.state = State::BeforeAttrName;
                    return None;
                }
                b'/' => {
                    self.state = State::SelfClosing;
                    return None;
                }
                b'>' => return self.emit_tag(),
                0 => self.tag.name.extend_from_slice(REPLACEMENT),
                _ => self.tag.name.push(c.to_ascii_lowercase()),
            }
        }
        self.eof()
    }

    fn before_attr_name(&mut self) -> Option<Kind> {
        while let Some(c) = self.peek() {
            match c {
                b'\t' | b'\n' | b'\x0c' | b' ' => self.pos += 1,
                b'/' | b'>' => {
                    self.state = State::AfterAttrName;
                    return None;
                }
                b'=' => {
                    self.start_attr();
                    self.current_attr().name.push(b'=');
                    self.pos += 1;
                    self.state = State::AttrName;
                    return None;
                }
                _ => {
                    self.start_attr();
                    self.state = State::AttrName;
                    return None;
                }
            }
        }
        self.state = State::AfterAttrName;
        None
    }

    fn attr_name(&mut self) -> Option<Kind> {
        while let Some(c) = self.peek() {
            match c {
                b'\t' | b'\n' | b'\x0c' | b' ' | b'/' | b'>' => {
                    self.check_duplicate_attr();
                    self.state = State::AfterAttrName;
                    return None;
                }
                b'=' => {
                    self.check_duplicate_attr();
                    self.pos += 1;
                    self.state = State::BeforeAttrValue;
                    return None;
                }
                0 => {
                    self.current_attr().name.extend_from_slice(REPLACEMENT);
                    self.pos += 1;
                }
                _ => {
                    self.current_attr().name.push(c.to_ascii_lowercase());
                    self.pos += 1;
                }
            }
        }
        self.check_duplicate_attr();
        self.eof()
    }

    fn after_attr_name(&mut self) -> Option<Kind> {
        while let Some(c) = self.peek() {
            match c {
                b'\t' | b'\n' | b'\x0c' | b' ' => self.pos += 1,
                b'/' => {
                    self.pos += 1;
                    self.state = State::SelfClosing;
                    return None;
                }
                b'=' => {
                    self.pos += 1;
                    self.state = State::BeforeAttrValue;
                    return None;
                }
                b'>' => {
                    self.pos += 1;
                    return self.emit_tag();
                }
                _ => {
                    self.start_attr();
                    self.state = State::AttrName;
                    return None;
                }
            }
        }
        self.eof()
    }

    fn before_attr_value(&mut self) -> Option<Kind> {
        while let Some(c) = self.peek() {
            match c {
                b'\t' | b'\n' | b'\x0c' | b' ' => self.pos += 1,
                b'"' => {
                    self.pos += 1;
                    self.state = State::AttrValueDq;
                    return None;
                }
                b'\'' => {
                    self.pos += 1;
                    self.state = State::AttrValueSq;
                    return None;
                }
                b'>' => {
                    self.pos += 1;
                    return self.emit_tag();
                }
                _ => {
                    self.state = State::AttrValueUnquoted;
                    return None;
                }
            }
        }
        self.state = State::AttrValueUnquoted;
        None
    }

    fn attr_value_quoted(&mut self, quote: u8) -> Option<Kind> {
        loop {
            let start = self.pos;
            let found = self.input[start..]
                .iter()
                .position(|&b| b == quote || b == b'&' || b == 0);
            let Some(i) = found else {
                return self.eof();
            };
            let at = start + i;
            let input = self.input;
            self.current_attr()
                .value
                .extend_from_slice(&input[start..at]);
            match input[at] {
                b'&' => self.charref_into_attr(at),
                0 => {
                    self.current_attr().value.extend_from_slice(REPLACEMENT);
                    self.pos = at + 1;
                }
                _ => {
                    self.pos = at + 1;
                    self.state = State::AfterAttrValueQuoted;
                    return None;
                }
            }
        }
    }

    fn attr_value_unquoted(&mut self) -> Option<Kind> {
        while let Some(c) = self.peek() {
            match c {
                b'\t' | b'\n' | b'\x0c' | b' ' => {
                    self.pos += 1;
                    self.state = State::BeforeAttrName;
                    return None;
                }
                b'&' => {
                    let at = self.pos;
                    self.charref_into_attr(at);
                }
                b'>' => {
                    self.pos += 1;
                    return self.emit_tag();
                }
                0 => {
                    self.current_attr().value.extend_from_slice(REPLACEMENT);
                    self.pos += 1;
                }
                _ => {
                    self.current_attr().value.push(c);
                    self.pos += 1;
                }
            }
        }
        self.eof()
    }

    fn after_attr_value_quoted(&mut self) -> Option<Kind> {
        match self.peek() {
            Some(b'\t' | b'\n' | b'\x0c' | b' ') => {
                self.pos += 1;
                self.state = State::BeforeAttrName;
            }
            Some(b'/') => {
                self.pos += 1;
                self.state = State::SelfClosing;
            }
            Some(b'>') => {
                self.pos += 1;
                return self.emit_tag();
            }
            None => return self.eof(),
            Some(_) => self.state = State::BeforeAttrName,
        }
        None
    }

    fn self_closing(&mut self) -> Option<Kind> {
        match self.peek() {
            Some(b'>') => {
                self.pos += 1;
                self.tag.self_closing = true;
                self.emit_tag()
            }
            None => self.eof(),
            Some(_) => {
                self.state = State::BeforeAttrName;
                None
            }
        }
    }

    fn emit_comment(&mut self) -> Option<Kind> {
        self.state = State::Data;
        self.emit(Kind::Comment)
    }

    fn emit_comment_then_eof(&mut self) -> Option<Kind> {
        self.done = true;
        self.pos = self.input.len();
        if self.text_is_empty() {
            self.pending = Some(Kind::Eof);
            Some(Kind::Comment)
        } else {
            self.pending = Some(Kind::Comment);
            Some(Kind::Chars)
        }
    }

    fn bogus_comment(&mut self) -> Option<Kind> {
        loop {
            let start = self.pos;
            let found = self.input[start..]
                .iter()
                .position(|&b| b == b'>' || b == 0);
            let Some(i) = found else {
                self.comment.extend_from_slice(&self.input[start..]);
                return self.emit_comment_then_eof();
            };
            let at = start + i;
            self.comment.extend_from_slice(&self.input[start..at]);
            self.pos = at + 1;
            if self.input[at] == b'>' {
                return self.emit_comment();
            }
            self.comment.extend_from_slice(REPLACEMENT);
        }
    }

    fn markup_decl(&mut self) -> Option<Kind> {
        if self.input[self.pos..].starts_with(b"--") {
            self.pos += 2;
            self.comment.clear();
            self.state = State::CommentStart;
        } else if self.starts_with_ci(b"doctype") {
            self.pos += 7;
            self.state = State::Doctype;
        } else if self.input[self.pos..].starts_with(b"[CDATA[") {
            self.pos += 7;
            if self.allow_cdata {
                self.state = State::CdataSection;
            } else {
                self.comment.clear();
                self.comment.extend_from_slice(b"[CDATA[");
                self.state = State::BogusComment;
            }
        } else {
            self.comment.clear();
            self.state = State::BogusComment;
        }
        None
    }

    fn comment_states(&mut self) -> Option<Kind> {
        if self.state == State::Comment {
            let start = self.pos;
            let found = self.input[start..]
                .iter()
                .position(|&b| b == b'<' || b == b'-' || b == 0);
            let Some(i) = found else {
                self.comment.extend_from_slice(&self.input[start..]);
                return self.emit_comment_then_eof();
            };
            let at = start + i;
            self.comment.extend_from_slice(&self.input[start..at]);
            self.pos = at + 1;
            match self.input[at] {
                b'<' => {
                    self.comment.push(b'<');
                    self.state = State::CommentLt;
                }
                b'-' => self.state = State::CommentEndDash,
                _ => self.comment.extend_from_slice(REPLACEMENT),
            }
            return None;
        }
        let c = self.peek();
        match self.state {
            State::CommentStart => match c {
                Some(b'-') => {
                    self.pos += 1;
                    self.state = State::CommentStartDash;
                }
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_comment();
                }
                _ => self.state = State::Comment,
            },
            State::CommentStartDash => match c {
                Some(b'-') => {
                    self.pos += 1;
                    self.state = State::CommentEnd;
                }
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_comment();
                }
                None => return self.emit_comment_then_eof(),
                Some(_) => {
                    self.comment.push(b'-');
                    self.state = State::Comment;
                }
            },
            State::CommentLt => match c {
                Some(b'!') => {
                    self.pos += 1;
                    self.comment.push(b'!');
                    self.state = State::CommentLtBang;
                }
                Some(b'<') => {
                    self.pos += 1;
                    self.comment.push(b'<');
                }
                _ => self.state = State::Comment,
            },
            State::CommentLtBang => {
                if c == Some(b'-') {
                    self.pos += 1;
                    self.state = State::CommentLtBangDash;
                } else {
                    self.state = State::Comment;
                }
            }
            State::CommentLtBangDash => {
                if c == Some(b'-') {
                    self.pos += 1;
                    self.state = State::CommentLtBangDashDash;
                } else {
                    self.state = State::CommentEndDash;
                }
            }
            State::CommentLtBangDashDash => self.state = State::CommentEnd,
            State::CommentEndDash => match c {
                Some(b'-') => {
                    self.pos += 1;
                    self.state = State::CommentEnd;
                }
                None => return self.emit_comment_then_eof(),
                Some(_) => {
                    self.comment.push(b'-');
                    self.state = State::Comment;
                }
            },
            State::CommentEnd => match c {
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_comment();
                }
                Some(b'!') => {
                    self.pos += 1;
                    self.state = State::CommentEndBang;
                }
                Some(b'-') => {
                    self.pos += 1;
                    self.comment.push(b'-');
                }
                None => return self.emit_comment_then_eof(),
                Some(_) => {
                    self.comment.extend_from_slice(b"--");
                    self.state = State::Comment;
                }
            },
            _ => match c {
                Some(b'-') => {
                    self.pos += 1;
                    self.comment.extend_from_slice(b"--!");
                    self.state = State::CommentEndDash;
                }
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_comment();
                }
                None => return self.emit_comment_then_eof(),
                Some(_) => {
                    self.comment.extend_from_slice(b"--!");
                    self.state = State::Comment;
                }
            },
        }
        None
    }

    fn new_doctype(&mut self) {
        self.doctype.name = None;
        self.doctype.public_id = None;
        self.doctype.system_id = None;
        self.doctype.force_quirks = false;
    }

    fn emit_doctype(&mut self) -> Option<Kind> {
        self.state = State::Data;
        self.emit(Kind::Doctype)
    }

    fn emit_doctype_then_eof(&mut self, force_quirks: bool) -> Option<Kind> {
        if force_quirks {
            self.doctype.force_quirks = true;
        }
        self.done = true;
        self.pos = self.input.len();
        if self.text_is_empty() {
            self.pending = Some(Kind::Eof);
            Some(Kind::Doctype)
        } else {
            self.pending = Some(Kind::Doctype);
            Some(Kind::Chars)
        }
    }

    fn doctype_id(&mut self) -> &mut Vec<u8> {
        let public = matches!(self.state, State::PublicIdDq | State::PublicIdSq);
        let id = if public {
            &mut self.doctype.public_id
        } else {
            &mut self.doctype.system_id
        };
        id.get_or_insert_with(Vec::new)
    }

    fn doctype_states(&mut self) -> Option<Kind> {
        let c = self.peek();
        let state = self.state;
        match state {
            State::Doctype => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => {
                    self.pos += 1;
                    self.state = State::BeforeDoctypeName;
                }
                None => {
                    self.new_doctype();
                    return self.emit_doctype_then_eof(true);
                }
                Some(_) => self.state = State::BeforeDoctypeName,
            },
            State::BeforeDoctypeName => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => self.pos += 1,
                Some(b'>') => {
                    self.pos += 1;
                    self.new_doctype();
                    self.doctype.force_quirks = true;
                    return self.emit_doctype();
                }
                None => {
                    self.new_doctype();
                    return self.emit_doctype_then_eof(true);
                }
                Some(c) => {
                    self.pos += 1;
                    self.new_doctype();
                    let mut name = Vec::new();
                    if c == 0 {
                        name.extend_from_slice(REPLACEMENT);
                    } else {
                        name.push(c.to_ascii_lowercase());
                    }
                    self.doctype.name = Some(name);
                    self.state = State::DoctypeName;
                }
            },
            State::DoctypeName => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => {
                    self.pos += 1;
                    self.state = State::AfterDoctypeName;
                }
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_doctype();
                }
                None => return self.emit_doctype_then_eof(true),
                Some(c) => {
                    self.pos += 1;
                    let name = self.doctype.name.get_or_insert_with(Vec::new);
                    if c == 0 {
                        name.extend_from_slice(REPLACEMENT);
                    } else {
                        name.push(c.to_ascii_lowercase());
                    }
                }
            },
            State::AfterDoctypeName => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => self.pos += 1,
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_doctype();
                }
                None => return self.emit_doctype_then_eof(true),
                Some(_) => {
                    if self.starts_with_ci(b"public") {
                        self.pos += 6;
                        self.state = State::AfterPublicKeyword;
                    } else if self.starts_with_ci(b"system") {
                        self.pos += 6;
                        self.state = State::AfterSystemKeyword;
                    } else {
                        self.doctype.force_quirks = true;
                        self.state = State::BogusDoctype;
                    }
                }
            },
            State::AfterPublicKeyword
            | State::BeforePublicId
            | State::AfterSystemKeyword
            | State::BeforeSystemId => {
                let public = matches!(state, State::AfterPublicKeyword | State::BeforePublicId);
                let keyword =
                    matches!(state, State::AfterPublicKeyword | State::AfterSystemKeyword);
                match c {
                    Some(b'\t' | b'\n' | b'\x0c' | b' ') => {
                        self.pos += 1;
                        if keyword {
                            self.state = if public {
                                State::BeforePublicId
                            } else {
                                State::BeforeSystemId
                            };
                        }
                    }
                    Some(q @ (b'"' | b'\'')) => {
                        self.pos += 1;
                        if public {
                            self.doctype.public_id = Some(Vec::new());
                            self.state = if q == b'"' {
                                State::PublicIdDq
                            } else {
                                State::PublicIdSq
                            };
                        } else {
                            self.doctype.system_id = Some(Vec::new());
                            self.state = if q == b'"' {
                                State::SystemIdDq
                            } else {
                                State::SystemIdSq
                            };
                        }
                    }
                    Some(b'>') => {
                        self.pos += 1;
                        self.doctype.force_quirks = true;
                        return self.emit_doctype();
                    }
                    None => return self.emit_doctype_then_eof(true),
                    Some(_) => {
                        self.doctype.force_quirks = true;
                        self.state = State::BogusDoctype;
                    }
                }
            }
            State::PublicIdDq | State::PublicIdSq | State::SystemIdDq | State::SystemIdSq => {
                let quote = if matches!(state, State::PublicIdDq | State::SystemIdDq) {
                    b'"'
                } else {
                    b'\''
                };
                match c {
                    Some(q) if q == quote => {
                        self.pos += 1;
                        self.state = if matches!(state, State::PublicIdDq | State::PublicIdSq) {
                            State::AfterPublicId
                        } else {
                            State::AfterSystemId
                        };
                    }
                    Some(b'>') => {
                        self.pos += 1;
                        self.doctype.force_quirks = true;
                        return self.emit_doctype();
                    }
                    None => return self.emit_doctype_then_eof(true),
                    Some(0) => {
                        self.pos += 1;
                        self.doctype_id().extend_from_slice(REPLACEMENT);
                    }
                    Some(c) => {
                        self.pos += 1;
                        self.doctype_id().push(c);
                    }
                }
            }
            State::AfterPublicId | State::BetweenIds => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => {
                    self.pos += 1;
                    self.state = State::BetweenIds;
                }
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_doctype();
                }
                Some(q @ (b'"' | b'\'')) => {
                    self.pos += 1;
                    self.doctype.system_id = Some(Vec::new());
                    self.state = if q == b'"' {
                        State::SystemIdDq
                    } else {
                        State::SystemIdSq
                    };
                }
                None => return self.emit_doctype_then_eof(true),
                Some(_) => {
                    self.doctype.force_quirks = true;
                    self.state = State::BogusDoctype;
                }
            },
            State::AfterSystemId => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => self.pos += 1,
                Some(b'>') => {
                    self.pos += 1;
                    return self.emit_doctype();
                }
                None => return self.emit_doctype_then_eof(true),
                Some(_) => self.state = State::BogusDoctype,
            },
            _ => match self.input[self.pos..].iter().position(|&b| b == b'>') {
                Some(i) => {
                    self.pos += i + 1;
                    return self.emit_doctype();
                }
                None => return self.emit_doctype_then_eof(false),
            },
        }
        None
    }

    fn cdata_section(&mut self) -> Option<Kind> {
        let start = self.pos;
        match self.input[start..].windows(3).position(|w| w == b"]]>") {
            Some(i) => {
                self.push_input(start, start + i);
                self.pos = start + i + 3;
                self.state = State::Data;
                None
            }
            None => {
                self.push_input(start, self.input.len());
                self.eof()
            }
        }
    }

    fn pi_to_bogus_comment(&mut self) {
        self.comment.clear();
        self.comment.push(b'?');
        self.comment.extend_from_slice(&self.pi_target);
        self.state = State::BogusComment;
    }

    fn pi_states(&mut self) -> Option<Kind> {
        let c = self.peek();
        match self.state {
            State::PiOpen => match c {
                Some(c) if c.is_ascii_alphabetic() || c == b'_' => {
                    self.pi_target.clear();
                    self.comment.clear();
                    self.state = State::PiTarget;
                }
                None => return self.eof(),
                Some(_) => {
                    self.comment.clear();
                    self.comment.push(b'?');
                    self.state = State::BogusComment;
                }
            },
            State::PiTarget => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ' | b'?' | b'>') => {
                    if self.pi_target.eq_ignore_ascii_case(b"xml")
                        || self.pi_target.eq_ignore_ascii_case(b"xml-stylesheet")
                    {
                        self.pi_to_bogus_comment();
                    } else {
                        self.state = State::AfterPiTarget;
                    }
                }
                Some(c) if c.is_ascii_alphanumeric() || c == b'-' || c == b'_' => {
                    self.pos += 1;
                    self.pi_target.push(c);
                }
                None => return self.eof(),
                Some(_) => self.pi_to_bogus_comment(),
            },
            State::AfterPiTarget => match c {
                Some(b'\t' | b'\n' | b'\x0c' | b' ') => self.pos += 1,
                _ => self.state = State::PiData,
            },
            State::PiData => {
                let start = self.pos;
                let found = self.input[start..]
                    .iter()
                    .position(|&b| b == b'?' || b == b'>');
                let Some(i) = found else {
                    return self.eof();
                };
                let at = start + i;
                self.comment.extend_from_slice(&self.input[start..at]);
                self.pos = at + 1;
                if self.input[at] == b'>' {
                    self.state = State::Data;
                    return self.emit(Kind::Pi);
                }
                self.state = State::PiQuestion;
            }
            _ => match c {
                Some(b'>') => {
                    self.pos += 1;
                    self.state = State::Data;
                    return self.emit(Kind::Pi);
                }
                None => return self.eof(),
                Some(_) => {
                    self.comment.push(b'?');
                    self.state = State::PiData;
                }
            },
        }
        None
    }
}

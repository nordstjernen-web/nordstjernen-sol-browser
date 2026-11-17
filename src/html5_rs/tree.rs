//! Nordstjernen — the WHATWG tree construction stage: insertion modes, the open-element stack and its scopes, active formatting elements, the adoption agency, foster parenting, templates, foreign content and fragment parsing.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::quirks::quirks_mode;
use crate::stack::{Elem, Fmt, Formatting, OpenStack, Scope, UNNAMED};
use crate::tags::{Tag, foreign_attr, svg_attr_name, svg_tag_name};
use crate::tokenizer::{Attr, Doctype, Kind, State, TagToken, Tokenizer};
use crate::{Attrs, Context, ElementName, LocalName, Ns, Position, QuirksMode, TreeSink};

const NO_POSITION: Position = Position { line: 0, column: 0 };
const REPLACEMENT: &[u8] = "\u{FFFD}".as_bytes();

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Mode {
    Initial,
    BeforeHtml,
    BeforeHead,
    InHead,
    InHeadNoscript,
    AfterHead,
    InBody,
    Text,
    InTable,
    InTableText,
    InCaption,
    InColumnGroup,
    InTableBody,
    InRow,
    InCell,
    InTemplate,
    AfterBody,
    InFrameset,
    AfterFrameset,
    AfterAfterBody,
    AfterAfterFrameset,
}

enum Step {
    Done,
    Again,
}

enum Place<H> {
    Append(H),
    Before(H),
}

enum Tok<'t> {
    Chars(&'t [u8]),
    Start(&'t mut TagToken),
    End(&'t mut TagToken),
    Comment(&'t [u8]),
    Pi(&'t [u8], &'t [u8]),
    Doctype(&'t Doctype),
    Eof,
}

const RESET_MODE_TAGS: &[Tag] = &[
    Tag::Td,
    Tag::Th,
    Tag::Tr,
    Tag::Tbody,
    Tag::Thead,
    Tag::Tfoot,
    Tag::Caption,
    Tag::Colgroup,
    Tag::Table,
    Tag::Template,
    Tag::Head,
    Tag::Body,
    Tag::Frameset,
    Tag::Html,
];

enum Bookmark<H> {
    Replace(H),
    InsertAfter(H),
}

struct ContextElem {
    tag: Tag,
    ns: Ns,
}

struct Builder<'s, S: TreeSink> {
    sink: &'s mut S,
    doc: S::Handle,
    mode: Mode,
    original_mode: Mode,
    template_modes: Vec<Mode>,
    open: OpenStack<S::Handle>,
    afe: Formatting<S::Handle>,
    head: Option<S::Handle>,
    form: Option<S::Handle>,
    scripting: bool,
    frameset_ok: bool,
    foster: bool,
    quirks: QuirksMode,
    context: Option<ContextElem>,
    pending_text: Vec<u8>,
    skip_newline: bool,
    next_state: Option<State>,
    fragment_root: Option<S::Handle>,
}

fn is_ws(c: u8) -> bool {
    matches!(c, b'\t' | b'\n' | b'\x0c' | b'\r' | b' ')
}

fn split_ws(text: &[u8]) -> (&[u8], &[u8]) {
    let n = text.iter().take_while(|&&c| is_ws(c)).count();
    text.split_at(n)
}

fn all_ws(text: &[u8]) -> bool {
    text.iter().all(|&c| is_ws(c))
}

fn without_nul(text: &[u8]) -> Option<Vec<u8>> {
    text.contains(&0)
        .then(|| text.iter().copied().filter(|&c| c != 0).collect())
}

fn element_local(ns: Ns, tag: Tag, name: &[u8]) -> LocalName<'_> {
    if ns == Ns::Svg
        && let Some(adjusted) = svg_tag_name(name)
    {
        return LocalName::Static(adjusted);
    }
    match tag.name() {
        Some(name) => LocalName::Static(name),
        None => LocalName::Dynamic(name),
    }
}

fn adjust_foreign_attrs(token: &mut TagToken, ns: Ns) {
    for attr in token.attrs_mut() {
        if ns == Ns::MathMl && attr.name == b"definitionurl" {
            attr.name.clear();
            attr.name.extend_from_slice(b"definitionURL");
        }
        if ns == Ns::Svg
            && let Some(adjusted) = svg_attr_name(&attr.name)
        {
            attr.name.clear();
            attr.name.extend_from_slice(adjusted);
        }
        if let Some((attr_ns, prefix)) = foreign_attr(&attr.name) {
            attr.ns = attr_ns;
            attr.prefix_len = prefix;
        }
    }
}

fn same_attrs(a: &[Attr], b: &[Attr]) -> bool {
    a.len() == b.len()
        && a.iter().all(|x| {
            b.iter()
                .any(|y| y.name == x.name && y.value == x.value && y.ns == x.ns)
        })
}

const BREAKOUT: &[Tag] = &[
    Tag::B,
    Tag::Big,
    Tag::Blockquote,
    Tag::Body,
    Tag::Br,
    Tag::Center,
    Tag::Code,
    Tag::Dd,
    Tag::Div,
    Tag::Dl,
    Tag::Dt,
    Tag::Em,
    Tag::Embed,
    Tag::H1,
    Tag::H2,
    Tag::H3,
    Tag::H4,
    Tag::H5,
    Tag::H6,
    Tag::Head,
    Tag::Hr,
    Tag::I,
    Tag::Img,
    Tag::Li,
    Tag::Listing,
    Tag::Menu,
    Tag::Meta,
    Tag::Nobr,
    Tag::Ol,
    Tag::P,
    Tag::Pre,
    Tag::Ruby,
    Tag::S,
    Tag::Small,
    Tag::Span,
    Tag::Strong,
    Tag::Strike,
    Tag::Sub,
    Tag::Sup,
    Tag::Table,
    Tag::Tt,
    Tag::U,
    Tag::Ul,
    Tag::Var,
];

const HEADINGS: &[Tag] = &[Tag::H1, Tag::H2, Tag::H3, Tag::H4, Tag::H5, Tag::H6];

impl<'s, S: TreeSink> Builder<'s, S> {
    fn new(sink: &'s mut S, scripting: bool) -> Self {
        let doc = sink.document();
        Builder {
            sink,
            doc,
            mode: Mode::Initial,
            original_mode: Mode::Initial,
            template_modes: Vec::new(),
            open: OpenStack::new(),
            afe: Formatting::new(),
            head: None,
            form: None,
            scripting,
            frameset_ok: true,
            foster: false,
            quirks: QuirksMode::NoQuirks,
            context: None,
            pending_text: Vec::new(),
            skip_newline: false,
            next_state: None,
            fragment_root: None,
        }
    }

    fn current_is(&self, tag: Tag) -> bool {
        self.open.last().is_some_and(|e| e.is(tag))
    }

    fn adjusted_current(&self) -> Option<(Tag, Ns, bool, bool, bool)> {
        if self.open.len() == 1
            && let Some(ctx) = &self.context
        {
            let elem = Elem {
                h: (),
                tag: ctx.tag,
                ns: ctx.ns,
                name: UNNAMED,
                html_ip: false,
            };
            return Some((
                ctx.tag,
                ctx.ns,
                elem.is_mathml_text_ip(),
                elem.is_html_ip(),
                ctx.ns == Ns::MathMl && ctx.tag == Tag::AnnotationXml,
            ));
        }
        self.open.last().map(|e| {
            (
                e.tag,
                e.ns,
                e.is_mathml_text_ip(),
                e.is_html_ip(),
                e.ns == Ns::MathMl && e.tag == Tag::AnnotationXml,
            )
        })
    }

    fn adjusted_current_foreign(&self) -> bool {
        self.adjusted_current()
            .is_some_and(|(_, ns, ..)| ns != Ns::Html)
    }

    fn use_html_rules(&self, tok: &Tok<'_>) -> bool {
        let Some((_, ns, mathml_text_ip, html_ip, annotation_xml)) = self.adjusted_current() else {
            return true;
        };
        if ns == Ns::Html {
            return true;
        }
        match tok {
            Tok::Start(t) => {
                (mathml_text_ip && !matches!(t.tag(), Tag::Mglyph | Tag::Malignmark))
                    || (annotation_xml && t.tag() == Tag::Svg)
                    || html_ip
            }
            Tok::Chars(_) => mathml_text_ip || html_ip,
            Tok::Eof => true,
            _ => false,
        }
    }

    fn process(&mut self, tok: &mut Tok<'_>) {
        if self.skip_newline {
            self.skip_newline = false;
            if let Tok::Chars(text) = tok
                && text.first() == Some(&b'\n')
            {
                let rest = &text[1..];
                if rest.is_empty() {
                    return;
                }
                *tok = Tok::Chars(rest);
            }
        }
        loop {
            let step = if self.use_html_rules(tok) {
                self.by_mode(tok)
            } else {
                self.foreign(tok)
            };
            if let Step::Done = step {
                return;
            }
        }
    }

    fn by_mode(&mut self, tok: &mut Tok<'_>) -> Step {
        match self.mode {
            Mode::Initial => self.initial(tok),
            Mode::BeforeHtml => self.before_html(tok),
            Mode::BeforeHead => self.before_head(tok),
            Mode::InHead => self.in_head(tok),
            Mode::InHeadNoscript => self.in_head_noscript(tok),
            Mode::AfterHead => self.after_head(tok),
            Mode::InBody => self.in_body(tok),
            Mode::Text => self.text(tok),
            Mode::InTable => self.in_table(tok),
            Mode::InTableText => self.in_table_text(tok),
            Mode::InCaption => self.in_caption(tok),
            Mode::InColumnGroup => self.in_column_group(tok),
            Mode::InTableBody => self.in_table_body(tok),
            Mode::InRow => self.in_row(tok),
            Mode::InCell => self.in_cell(tok),
            Mode::InTemplate => self.in_template(tok),
            Mode::AfterBody => self.after_body(tok),
            Mode::InFrameset => self.in_frameset(tok),
            Mode::AfterFrameset => self.after_frameset(tok),
            Mode::AfterAfterBody => self.after_after_body(tok),
            Mode::AfterAfterFrameset => self.after_after_frameset(tok),
        }
    }

    fn insert_at(&mut self, place: Place<S::Handle>, node: S::Handle) {
        match place {
            Place::Append(parent) => self.sink.append(parent, node),
            Place::Before(sibling) => self.sink.insert_before(sibling, node),
        }
    }

    fn template_adjusted(&mut self, index: usize) -> Place<S::Handle> {
        let elem = &self.open[index];
        if elem.is(Tag::Template) {
            let h = elem.h;
            Place::Append(self.sink.template_contents(h))
        } else {
            Place::Append(elem.h)
        }
    }

    fn appropriate_place(&mut self, target: Option<usize>) -> Place<S::Handle> {
        let index = target.unwrap_or(self.open.len() - 1);
        let elem = &self.open[index];
        if !(self.foster && elem.is_any(&[Tag::Table, Tag::Tbody, Tag::Tfoot, Tag::Thead, Tag::Tr]))
        {
            return self.template_adjusted(index);
        }
        let last_template = self.open.top_html(Tag::Template);
        let last_table = self.open.top_html(Tag::Table);
        if let Some(template) = last_template
            && last_table.is_none_or(|table| template > table)
        {
            let h = self.open[template].h;
            return Place::Append(self.sink.template_contents(h));
        }
        let Some(table) = last_table else {
            return self.template_adjusted(0);
        };
        let table_h = self.open[table].h;
        if self.sink.parent(table_h).is_some() {
            return Place::Before(table_h);
        }
        self.template_adjusted(table - 1)
    }

    fn create_element(
        &mut self,
        ns: Ns,
        tag: Tag,
        name: &[u8],
        attrs: &[Attr],
        position: Position,
    ) -> S::Handle {
        let name = ElementName {
            ns,
            tag,
            local: element_local(ns, tag, name),
        };
        self.sink.create_element(name, Attrs::new(attrs), position)
    }

    fn push(&mut self, h: S::Handle, ns: Ns, tag: Tag, name: &[u8], html_ip: bool) {
        let name = if tag == Tag::Other {
            self.open.intern(name)
        } else {
            UNNAMED
        };
        self.open.push(Elem {
            h,
            tag,
            ns,
            name,
            html_ip,
        });
    }

    fn insert_element(&mut self, token: &TagToken, ns: Ns) -> S::Handle {
        let place = self.appropriate_place(None);
        let tag = token.tag();
        let h = self.create_element(
            ns,
            tag,
            &token.name,
            token.attrs(),
            token.position.unwrap_or(NO_POSITION),
        );
        self.insert_at(place, h);
        let html_ip = ns == Ns::MathMl
            && tag == Tag::AnnotationXml
            && token.attr(b"encoding").is_some_and(|v| {
                v.eq_ignore_ascii_case(b"text/html")
                    || v.eq_ignore_ascii_case(b"application/xhtml+xml")
            });
        self.push(h, ns, tag, &token.name, html_ip);
        h
    }

    fn insert_html(&mut self, token: &TagToken) -> S::Handle {
        self.insert_element(token, Ns::Html)
    }

    fn insert_html_tag(&mut self, tag: Tag) -> S::Handle {
        let place = self.appropriate_place(None);
        let name = tag.name().map_or(&b""[..], |n| n.to_bytes());
        let h = self.create_element(Ns::Html, tag, name, &[], NO_POSITION);
        self.insert_at(place, h);
        self.push(h, Ns::Html, tag, name, false);
        h
    }

    fn insert_foreign(&mut self, token: &mut TagToken, ns: Ns) {
        adjust_foreign_attrs(token, ns);
        self.insert_element(token, ns);
        if token.self_closing {
            self.pop_open();
        }
    }

    fn insert_text(&mut self, text: &[u8]) {
        if text.is_empty() {
            return;
        }
        match self.appropriate_place(None) {
            Place::Append(parent) if parent == self.doc => {}
            Place::Append(parent) => self.sink.append_text(parent, text),
            Place::Before(sibling) => self.sink.insert_text_before(sibling, text),
        }
    }

    fn comment_node(&mut self, tok: &Tok<'_>) -> S::Handle {
        match tok {
            Tok::Pi(target, data) => self.sink.create_processing_instruction(target, data),
            Tok::Comment(text) => self.sink.create_comment(text),
            _ => self.sink.create_comment(b""),
        }
    }

    fn insert_comment(&mut self, tok: &Tok<'_>) {
        let place = self.appropriate_place(None);
        let node = self.comment_node(tok);
        self.insert_at(place, node);
    }

    fn append_comment_to(&mut self, parent: S::Handle, tok: &Tok<'_>) {
        let node = self.comment_node(tok);
        self.sink.append(parent, node);
    }

    fn in_scope(&self, tag: Tag, scope: Scope) -> bool {
        self.open.in_scope(&[tag], scope)
    }

    fn any_in_scope(&self, tags: &[Tag], scope: Scope) -> bool {
        self.open.in_scope(tags, scope)
    }

    fn generate_implied_end_tags(&mut self, except: Option<Tag>) {
        while let Some(e) = self.open.last() {
            let implied = e.is_any(&[
                Tag::Dd,
                Tag::Dt,
                Tag::Li,
                Tag::Optgroup,
                Tag::Option,
                Tag::P,
                Tag::Rb,
                Tag::Rp,
                Tag::Rt,
                Tag::Rtc,
            ]);
            if !implied || except.is_some_and(|t| e.is(t)) {
                return;
            }
            self.pop_open();
        }
    }

    fn generate_all_implied_end_tags(&mut self) {
        while let Some(e) = self.open.last() {
            if !e.is_any(&[
                Tag::Caption,
                Tag::Colgroup,
                Tag::Dd,
                Tag::Dt,
                Tag::Li,
                Tag::Optgroup,
                Tag::Option,
                Tag::P,
                Tag::Rb,
                Tag::Rp,
                Tag::Rt,
                Tag::Rtc,
                Tag::Tbody,
                Tag::Td,
                Tag::Tfoot,
                Tag::Th,
                Tag::Thead,
                Tag::Tr,
            ]) {
                return;
            }
            self.pop_open();
        }
    }

    fn pop_open(&mut self) -> Option<Elem<S::Handle>> {
        let elem = self.open.pop()?;
        self.finish_children(&elem);
        Some(elem)
    }

    fn truncate_open(&mut self, len: usize) {
        while self.open.len() > len {
            self.pop_open();
        }
    }

    fn remove_open(&mut self, position: usize) {
        let finished = self.open[position].h;
        let is_option = self.open[position].is(Tag::Option);
        self.open.remove(position);
        if is_option {
            self.sink.option_children_parsed(finished);
        }
    }

    fn finish_children(&mut self, elem: &Elem<S::Handle>) {
        if elem.is(Tag::Option) {
            self.sink.option_children_parsed(elem.h);
        }
    }

    fn pop_until(&mut self, tag: Tag) {
        while let Some(e) = self.pop_open() {
            if e.is(tag) {
                return;
            }
        }
    }

    fn pop_until_any(&mut self, tags: &[Tag]) {
        while let Some(e) = self.pop_open() {
            if e.is_any(tags) {
                return;
            }
        }
    }

    fn clear_to_context(&mut self, tags: &[Tag]) {
        while let Some(e) = self.open.last() {
            if e.is_any(tags) || e.is(Tag::Template) || e.is(Tag::Html) {
                return;
            }
            self.pop_open();
        }
    }

    fn close_p(&mut self) {
        self.generate_implied_end_tags(Some(Tag::P));
        self.pop_until(Tag::P);
    }

    fn close_p_in_button_scope(&mut self) {
        if self.in_scope(Tag::P, Scope::Button) {
            self.close_p();
        }
    }

    fn has_template(&self) -> bool {
        self.open.has_html(Tag::Template)
    }

    fn parsing_template_contents(&self) -> bool {
        !self.template_modes.is_empty()
    }

    fn remove_from_stack(&mut self, h: S::Handle, tag: Tag) {
        if let Some(i) = self.stack_position(h, tag) {
            self.remove_open(i);
        }
    }

    fn recreate_listed(&mut self, position: usize) -> Option<(Tag, S::Handle)> {
        let Fmt::El { tag, .. } = &self.afe[position] else {
            return None;
        };
        let tag = *tag;
        let attrs = self.afe.take_attrs(position);
        let name = tag.name().map_or(&b""[..], |n| n.to_bytes());
        let h = self.create_element(Ns::Html, tag, name, &attrs, NO_POSITION);
        self.afe.restore_attrs(position, attrs);
        Some((tag, h))
    }

    fn afe_index(&self, h: S::Handle) -> Option<usize> {
        self.afe.iter().rposition(|f| f.handle() == Some(h))
    }

    fn listed_position(&self, position: usize, cursor: &mut usize) -> Option<usize> {
        if !self.afe.lists(self.open.serial_at(position)) {
            return None;
        }
        let h = self.open[position].h;
        let found = self.afe[..*cursor]
            .iter()
            .rposition(|f| f.handle() == Some(h))
            .or_else(|| self.afe_index(h));
        if let Some(i) = found {
            *cursor = i;
        }
        found
    }

    fn stack_position(&self, h: S::Handle, tag: Tag) -> Option<usize> {
        let mut position = self.open.top_html(tag);
        while let Some(i) = position {
            if self.open[i].h == h {
                return Some(i);
            }
            position = self.open.below_same(i);
        }
        None
    }

    fn push_formatting(&mut self, h: S::Handle, token: &TagToken) {
        let tag = token.tag();
        let (candidates, key) = self.afe.identical_candidates(tag, token.attrs());
        if candidates >= 3 {
            let mut matching = Vec::new();
            for (i, f) in self.afe.iter().enumerate().rev() {
                match f {
                    Fmt::Marker => break,
                    Fmt::El { tag: t, attrs, .. }
                        if *t == tag && same_attrs(attrs, token.attrs()) =>
                    {
                        matching.push(i);
                        if matching.len() == candidates {
                            break;
                        }
                    }
                    _ => {}
                }
            }
            if matching.len() >= 3 {
                self.afe.remove(matching[matching.len() - 1]);
            }
        }
        let serial = self.open.top_serial();
        self.afe.push(h, serial, tag, token.attrs(), key);
    }

    fn clear_afe_to_marker(&mut self) {
        self.afe.clear_to_marker();
    }

    fn reconstruct_formatting(&mut self) {
        let Some(last) = self.afe.last() else {
            return;
        };
        match last {
            Fmt::Marker => return,
            Fmt::El { serial, .. } if self.open.holds(*serial) => return,
            _ => {}
        }
        let mut i = self.afe.len() - 1;
        while i > 0 {
            match &self.afe[i - 1] {
                Fmt::Marker => break,
                Fmt::El { serial, .. } if self.open.holds(*serial) => break,
                _ => i -= 1,
            }
        }
        while i < self.afe.len() {
            if let Fmt::Marker = &self.afe[i] {
                i += 1;
                continue;
            }
            let place = self.appropriate_place(None);
            let Some((tag, h)) = self.recreate_listed(i) else {
                i += 1;
                continue;
            };
            self.insert_at(place, h);
            let name = tag.name().map_or(&b""[..], |n| n.to_bytes());
            self.push(h, Ns::Html, tag, name, false);
            let serial = self.open.top_serial();
            self.afe.rehandle(i, h, serial);
            i += 1;
        }
    }

    fn adoption_agency(&mut self, subject: Tag) -> bool {
        if let Some(cur) = self.open.last()
            && cur.is(subject)
            && !self.afe.lists(self.open.top_serial())
        {
            self.pop_open();
            return true;
        }
        for _ in 0..8 {
            if self.afe.count_tag(subject) == 0 {
                return false;
            }
            let mut found = None;
            for (i, f) in self.afe.iter().enumerate().rev() {
                match f {
                    Fmt::Marker => break,
                    Fmt::El { tag, h, .. } if *tag == subject => {
                        found = Some((i, *h));
                        break;
                    }
                    _ => {}
                }
            }
            let Some((fe_afe, fe)) = found else {
                return false;
            };
            let Some(fe_index) = self.stack_position(fe, subject) else {
                self.afe.remove(fe_afe);
                return true;
            };
            if !self
                .open
                .clear_of_boundaries_above(fe_index, Scope::Default)
            {
                return true;
            }
            let furthest = (fe_index + 1..self.open.len()).find(|&i| self.open[i].is_special());
            let Some(fb_index) = furthest else {
                self.truncate_open(fe_index);
                self.afe.remove(fe_afe);
                return true;
            };
            let fb = self.open[fb_index].h;
            let common_ancestor = fe_index - 1;
            let mut bookmark = Bookmark::Replace(fe);
            let mut node_index = fb_index;
            let mut last_node = fb;
            let mut inner = 0;
            let mut cursor = self.afe.len();
            let mut unstacked = Vec::new();
            let mut unlisted = Vec::new();
            loop {
                inner += 1;
                node_index -= 1;
                let node = self.open[node_index].h;
                if node == fe {
                    break;
                }
                let mut node_afe = self.listed_position(node_index, &mut cursor);
                if inner > 3
                    && let Some(i) = node_afe
                {
                    unlisted.push(i);
                    node_afe = None;
                }
                let Some(node_afe) = node_afe else {
                    unstacked.push(node_index);
                    if self.open[node_index].is(Tag::Option) {
                        self.sink.option_children_parsed(node);
                    }
                    continue;
                };
                let Some((_, new)) = self.recreate_listed(node_afe) else {
                    break;
                };
                let serial = self.open.replace_handle(node_index, new);
                self.afe.rehandle(node_afe, new, serial);
                if last_node == fb {
                    bookmark = Bookmark::InsertAfter(new);
                }
                self.sink.remove(last_node);
                self.sink.append(new, last_node);
                last_node = new;
            }
            unstacked.reverse();
            self.open.remove_all(&unstacked);
            unlisted.sort_unstable();
            self.afe.remove_all(&unlisted);
            self.sink.remove(last_node);
            let place = self.appropriate_place(Some(common_ancestor));
            self.insert_at(place, last_node);
            let Some(fe_afe) = self.afe_index(fe) else {
                return true;
            };
            let Some((tag, new)) = self.recreate_listed(fe_afe) else {
                return true;
            };
            self.sink.reparent_children(fb, new);
            self.sink.append(fb, new);
            self.remove_from_stack(fe, subject);
            let fb_now = self
                .open
                .iter()
                .rposition(|e| e.h == fb)
                .unwrap_or(self.open.len() - 1);
            let serial = self.open.insert(
                fb_now + 1,
                Elem {
                    h: new,
                    tag,
                    ns: Ns::Html,
                    name: UNNAMED,
                    html_ip: false,
                },
            );
            match bookmark {
                Bookmark::Replace(_) => self.afe.rehandle(fe_afe, new, serial),
                Bookmark::InsertAfter(after) => {
                    let entry = match self.afe.take(fe_afe) {
                        Fmt::El {
                            tag,
                            attrs,
                            key,
                            level,
                            ..
                        } => Fmt::El {
                            h: new,
                            serial,
                            tag,
                            attrs,
                            key,
                            level,
                        },
                        Fmt::Marker => Fmt::Marker,
                    };
                    let at = self.afe_index(after).map_or(self.afe.len(), |i| i + 1);
                    self.afe.insert(at, entry);
                }
            }
        }
        true
    }

    fn mode_for(&self, tag: Tag, last: bool) -> Option<Mode> {
        match tag {
            Tag::Td | Tag::Th if !last => Some(Mode::InCell),
            Tag::Tr => Some(Mode::InRow),
            Tag::Tbody | Tag::Thead | Tag::Tfoot => Some(Mode::InTableBody),
            Tag::Caption => Some(Mode::InCaption),
            Tag::Colgroup => Some(Mode::InColumnGroup),
            Tag::Table => Some(Mode::InTable),
            Tag::Template => Some(*self.template_modes.last().unwrap_or(&Mode::InBody)),
            Tag::Head if !last => Some(Mode::InHead),
            Tag::Body => Some(Mode::InBody),
            Tag::Frameset => Some(Mode::InFrameset),
            Tag::Html => Some(if self.head.is_none() {
                Mode::BeforeHead
            } else {
                Mode::AfterHead
            }),
            _ => None,
        }
    }

    fn reset_insertion_mode(&mut self) {
        let topmost = RESET_MODE_TAGS
            .iter()
            .filter_map(|&tag| self.open.top_html(tag))
            .filter(|&i| i > 0)
            .max();
        if let Some(i) = topmost
            && let Some(mode) = self.mode_for(self.open[i].tag, false)
        {
            self.mode = mode;
            return;
        }
        let Some(first) = self.open.first() else {
            self.mode = Mode::InBody;
            return;
        };
        let (tag, ns) = match &self.context {
            Some(ctx) => (ctx.tag, ctx.ns),
            None => (first.tag, first.ns),
        };
        self.mode = if ns == Ns::Html {
            self.mode_for(tag, true).unwrap_or(Mode::InBody)
        } else {
            Mode::InBody
        };
    }

    fn generic_text(&mut self, token: &TagToken, state: State) {
        self.insert_html(token);
        self.next_state = Some(state);
        self.original_mode = self.mode;
        self.mode = Mode::Text;
    }

    fn stop(&mut self) -> Step {
        self.truncate_open(0);
        Step::Done
    }

    fn initial(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let (_, rest) = split_ws(text);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                let doc = self.doc;
                self.append_comment_to(doc, tok);
                return Step::Done;
            }
            Tok::Doctype(d) => {
                let name = d.name.as_deref().unwrap_or(b"");
                let public = d.public_id.as_deref().unwrap_or(b"");
                let system = d.system_id.as_deref().unwrap_or(b"");
                self.sink.append_doctype(name, public, system);
                self.quirks = quirks_mode(d);
                self.mode = Mode::BeforeHtml;
                return Step::Done;
            }
            _ => {}
        }
        self.quirks = QuirksMode::Quirks;
        self.mode = Mode::BeforeHtml;
        Step::Again
    }

    fn create_root(&mut self, token: Option<&TagToken>) {
        let attrs = token.map_or(&[][..], |t| t.attrs());
        let position = token.and_then(|t| t.position).unwrap_or(NO_POSITION);
        let h = self.create_element(Ns::Html, Tag::Html, b"html", attrs, position);
        let doc = self.doc;
        self.sink.append(doc, h);
        self.push(h, Ns::Html, Tag::Html, b"html", false);
        self.mode = Mode::BeforeHead;
    }

    fn before_html(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Doctype(_) => return Step::Done,
            Tok::Comment(_) | Tok::Pi(..) => {
                let doc = self.doc;
                self.append_comment_to(doc, tok);
                return Step::Done;
            }
            Tok::Chars(text) => {
                let (_, rest) = split_ws(text);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Start(t) if t.tag() == Tag::Html => {
                self.create_root(Some(t));
                return Step::Done;
            }
            Tok::End(t) if !matches!(t.tag(), Tag::Head | Tag::Body | Tag::Html | Tag::Br) => {
                return Step::Done;
            }
            _ => {}
        }
        self.create_root(None);
        Step::Again
    }

    fn before_head(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let (_, rest) = split_ws(text);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                return Step::Done;
            }
            Tok::Doctype(_) => return Step::Done,
            Tok::Start(t) if t.tag() == Tag::Html => return self.in_body(tok),
            Tok::Start(t) if t.tag() == Tag::Head => {
                let h = self.insert_html(t);
                self.head = Some(h);
                self.mode = Mode::InHead;
                return Step::Done;
            }
            Tok::End(t) if !matches!(t.tag(), Tag::Head | Tag::Body | Tag::Html | Tag::Br) => {
                return Step::Done;
            }
            _ => {}
        }
        let h = self.insert_html_tag(Tag::Head);
        self.head = Some(h);
        self.mode = Mode::InHead;
        Step::Again
    }

    fn in_head(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let (ws, rest) = split_ws(text);
                self.insert_text(ws);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                return Step::Done;
            }
            Tok::Doctype(_) => return Step::Done,
            Tok::Start(t) => match t.tag() {
                Tag::Html => return self.in_body(tok),
                Tag::Base | Tag::Basefont | Tag::Bgsound | Tag::Link | Tag::Meta => {
                    self.insert_html(t);
                    self.pop_open();
                    return Step::Done;
                }
                Tag::Title => {
                    self.generic_text(t, State::Rcdata);
                    return Step::Done;
                }
                Tag::Noscript if self.scripting => {
                    self.generic_text(t, State::Rawtext);
                    return Step::Done;
                }
                Tag::Noframes | Tag::Style => {
                    self.generic_text(t, State::Rawtext);
                    return Step::Done;
                }
                Tag::Noscript => {
                    self.insert_html(t);
                    self.mode = Mode::InHeadNoscript;
                    return Step::Done;
                }
                Tag::Script => {
                    self.generic_text(t, State::ScriptData);
                    return Step::Done;
                }
                Tag::Template => {
                    self.insert_html(t);
                    self.afe.push_marker();
                    self.frameset_ok = false;
                    self.mode = Mode::InTemplate;
                    self.template_modes.push(Mode::InTemplate);
                    return Step::Done;
                }
                Tag::Head => return Step::Done,
                _ => {}
            },
            Tok::End(t) => match t.tag() {
                Tag::Head => {
                    self.pop_open();
                    self.mode = Mode::AfterHead;
                    return Step::Done;
                }
                Tag::Body | Tag::Html | Tag::Br => {}
                Tag::Template => {
                    if !self.has_template() {
                        return Step::Done;
                    }
                    self.generate_all_implied_end_tags();
                    self.pop_until(Tag::Template);
                    self.clear_afe_to_marker();
                    self.template_modes.pop();
                    self.reset_insertion_mode();
                    return Step::Done;
                }
                _ => return Step::Done,
            },
            _ => {}
        }
        self.pop_open();
        self.mode = Mode::AfterHead;
        Step::Again
    }

    fn in_head_noscript(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Doctype(_) => return Step::Done,
            Tok::Start(t) if t.tag() == Tag::Html => return self.in_body(tok),
            Tok::End(t) if t.tag() == Tag::Noscript => {
                self.pop_open();
                self.mode = Mode::InHead;
                return Step::Done;
            }
            Tok::Chars(text) => {
                let (ws, rest) = split_ws(text);
                self.insert_text(ws);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                return Step::Done;
            }
            Tok::Start(t)
                if matches!(
                    t.tag(),
                    Tag::Basefont
                        | Tag::Bgsound
                        | Tag::Link
                        | Tag::Meta
                        | Tag::Noframes
                        | Tag::Style
                ) =>
            {
                return self.in_head(tok);
            }
            Tok::Start(t) if matches!(t.tag(), Tag::Head | Tag::Noscript) => return Step::Done,
            Tok::End(t) if t.tag() != Tag::Br => return Step::Done,
            _ => {}
        }
        self.pop_open();
        self.mode = Mode::InHead;
        Step::Again
    }

    fn after_head(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let (ws, rest) = split_ws(text);
                self.insert_text(ws);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                return Step::Done;
            }
            Tok::Doctype(_) => return Step::Done,
            Tok::Start(t) => match t.tag() {
                Tag::Html => return self.in_body(tok),
                Tag::Body => {
                    self.insert_html(t);
                    self.frameset_ok = false;
                    self.mode = Mode::InBody;
                    return Step::Done;
                }
                Tag::Frameset => {
                    self.insert_html(t);
                    self.mode = Mode::InFrameset;
                    return Step::Done;
                }
                Tag::Base
                | Tag::Basefont
                | Tag::Bgsound
                | Tag::Link
                | Tag::Meta
                | Tag::Noframes
                | Tag::Script
                | Tag::Style
                | Tag::Template
                | Tag::Title => {
                    let Some(head) = self.head else {
                        return self.in_head(tok);
                    };
                    self.open.push(Elem {
                        h: head,
                        tag: Tag::Head,
                        ns: Ns::Html,
                        name: UNNAMED,
                        html_ip: false,
                    });
                    let step = self.in_head(tok);
                    self.remove_from_stack(head, Tag::Head);
                    return step;
                }
                Tag::Head => return Step::Done,
                _ => {}
            },
            Tok::End(t) => match t.tag() {
                Tag::Template => return self.in_head(tok),
                Tag::Body | Tag::Html | Tag::Br => {}
                _ => return Step::Done,
            },
            _ => {}
        }
        self.insert_html_tag(Tag::Body);
        self.frameset_ok = true;
        self.mode = Mode::InBody;
        Step::Again
    }

    fn body_chars(&mut self, text: &[u8]) {
        let stripped = without_nul(text);
        let text = stripped.as_deref().unwrap_or(text);
        if text.is_empty() {
            return;
        }
        self.reconstruct_formatting();
        self.insert_text(text);
        if !all_ws(text) {
            self.frameset_ok = false;
        }
    }

    fn in_body(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let text: &[u8] = text;
                self.body_chars(text);
                Step::Done
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                Step::Done
            }
            Tok::Doctype(_) => Step::Done,
            Tok::Start(_) => self.in_body_start(tok),
            Tok::End(_) => self.in_body_end(tok),
            Tok::Eof => {
                if !self.template_modes.is_empty() {
                    return self.in_template(tok);
                }
                self.stop()
            }
        }
    }

    fn in_body_start(&mut self, tok: &mut Tok<'_>) -> Step {
        let Tok::Start(t) = tok else {
            return Step::Done;
        };
        match t.tag() {
            Tag::Html => {
                if !self.has_template() {
                    let root = self.open[0].h;
                    self.sink
                        .add_missing_attributes(root, Attrs::new(t.attrs()));
                }
            }
            Tag::Base
            | Tag::Basefont
            | Tag::Bgsound
            | Tag::Link
            | Tag::Meta
            | Tag::Noframes
            | Tag::Script
            | Tag::Style
            | Tag::Template
            | Tag::Title => return self.in_head(tok),
            Tag::Body => {
                if self.open.len() > 1 && self.open[1].is(Tag::Body) && !self.has_template() {
                    self.frameset_ok = false;
                    let body = self.open[1].h;
                    self.sink
                        .add_missing_attributes(body, Attrs::new(t.attrs()));
                }
            }
            Tag::Frameset => {
                if self.open.len() > 1 && self.open[1].is(Tag::Body) && self.frameset_ok {
                    let body = self.open[1].h;
                    self.sink.remove(body);
                    self.truncate_open(1);
                    self.insert_html(t);
                    self.mode = Mode::InFrameset;
                }
            }
            Tag::Address
            | Tag::Article
            | Tag::Aside
            | Tag::Blockquote
            | Tag::Center
            | Tag::Details
            | Tag::Dialog
            | Tag::Dir
            | Tag::Div
            | Tag::Dl
            | Tag::Fieldset
            | Tag::Figcaption
            | Tag::Figure
            | Tag::Footer
            | Tag::Header
            | Tag::Hgroup
            | Tag::Main
            | Tag::Menu
            | Tag::Nav
            | Tag::Ol
            | Tag::P
            | Tag::Search
            | Tag::Section
            | Tag::Summary
            | Tag::Ul => {
                self.close_p_in_button_scope();
                self.insert_html(t);
            }
            Tag::H1 | Tag::H2 | Tag::H3 | Tag::H4 | Tag::H5 | Tag::H6 => {
                self.close_p_in_button_scope();
                if self.open.last().is_some_and(|e| e.is_any(HEADINGS)) {
                    self.pop_open();
                }
                self.insert_html(t);
            }
            Tag::Pre | Tag::Listing => {
                self.close_p_in_button_scope();
                self.insert_html(t);
                self.skip_newline = true;
                self.frameset_ok = false;
            }
            Tag::Form => {
                if self.form.is_some() && !self.parsing_template_contents() {
                    return Step::Done;
                }
                self.close_p_in_button_scope();
                let h = self.insert_html(t);
                if !self.parsing_template_contents() {
                    self.form = Some(h);
                }
            }
            Tag::Li | Tag::Dd | Tag::Dt => {
                self.frameset_ok = false;
                let closes: &[Tag] = if t.tag() == Tag::Li {
                    &[Tag::Li]
                } else {
                    &[Tag::Dd, Tag::Dt]
                };
                for i in (0..self.open.len()).rev() {
                    let e = &self.open[i];
                    if e.is_any(closes) {
                        let tag = e.tag;
                        self.generate_implied_end_tags(Some(tag));
                        self.pop_until(tag);
                        break;
                    }
                    if e.is_special() && !e.is_any(&[Tag::Address, Tag::Div, Tag::P]) {
                        break;
                    }
                }
                self.close_p_in_button_scope();
                self.insert_html(t);
            }
            Tag::Plaintext => {
                self.close_p_in_button_scope();
                self.insert_html(t);
                self.next_state = Some(State::Plaintext);
            }
            Tag::Button => {
                if self.in_scope(Tag::Button, Scope::Default) {
                    self.generate_implied_end_tags(None);
                    self.pop_until(Tag::Button);
                }
                self.reconstruct_formatting();
                self.insert_html(t);
                self.frameset_ok = false;
            }
            Tag::A => {
                let mut existing = None;
                if self.afe.count_tag(Tag::A) > 0 {
                    for f in self.afe.iter().rev() {
                        match f {
                            Fmt::Marker => break,
                            Fmt::El {
                                tag: Tag::A,
                                h,
                                serial,
                                ..
                            } => {
                                existing = Some((*h, *serial));
                                break;
                            }
                            _ => {}
                        }
                    }
                }
                if let Some((a, serial)) = existing {
                    if !self.adoption_agency(Tag::A) {
                        self.any_other_end_tag(Tag::A, b"a");
                    }
                    if self.afe.lists(serial)
                        && let Some(i) = self.afe_index(a)
                    {
                        self.afe.remove(i);
                    }
                    self.remove_from_stack(a, Tag::A);
                }
                self.reconstruct_formatting();
                let h = self.insert_html(t);
                self.push_formatting(h, t);
            }
            Tag::B
            | Tag::Big
            | Tag::Code
            | Tag::Em
            | Tag::Font
            | Tag::I
            | Tag::S
            | Tag::Small
            | Tag::Strike
            | Tag::Strong
            | Tag::Tt
            | Tag::U => {
                self.reconstruct_formatting();
                let h = self.insert_html(t);
                self.push_formatting(h, t);
            }
            Tag::Nobr => {
                self.reconstruct_formatting();
                if self.in_scope(Tag::Nobr, Scope::Default) {
                    if !self.adoption_agency(Tag::Nobr) {
                        self.any_other_end_tag(Tag::Nobr, b"nobr");
                    }
                    self.reconstruct_formatting();
                }
                let h = self.insert_html(t);
                self.push_formatting(h, t);
            }
            Tag::Applet | Tag::Marquee | Tag::Object => {
                self.reconstruct_formatting();
                self.insert_html(t);
                self.afe.push_marker();
                self.frameset_ok = false;
            }
            Tag::Table => {
                if self.quirks != QuirksMode::Quirks {
                    self.close_p_in_button_scope();
                }
                self.insert_html(t);
                self.frameset_ok = false;
                self.mode = Mode::InTable;
            }
            Tag::Area | Tag::Br | Tag::Embed | Tag::Img | Tag::Keygen | Tag::Wbr => {
                self.reconstruct_formatting();
                self.insert_html(t);
                self.pop_open();
                self.frameset_ok = false;
            }
            Tag::Input => {
                if self.context_is(Tag::Select) {
                    return Step::Done;
                }
                if self.in_scope(Tag::Select, Scope::Default) {
                    self.pop_until(Tag::Select);
                }
                self.reconstruct_formatting();
                self.insert_html(t);
                self.pop_open();
                if !t
                    .attr(b"type")
                    .is_some_and(|v| v.eq_ignore_ascii_case(b"hidden"))
                {
                    self.frameset_ok = false;
                }
            }
            Tag::Param | Tag::Source | Tag::Track => {
                self.insert_html(t);
                self.pop_open();
            }
            Tag::Hr => {
                self.close_p_in_button_scope();
                if self.in_scope(Tag::Select, Scope::Default) {
                    self.generate_implied_end_tags(None);
                }
                self.insert_html(t);
                self.pop_open();
                self.frameset_ok = false;
            }
            Tag::Image => {
                t.name.clear();
                t.name.extend_from_slice(b"img");
                t.tag = Some(Tag::Img);
                return Step::Again;
            }
            Tag::Textarea => {
                self.insert_html(t);
                self.skip_newline = true;
                self.next_state = Some(State::Rcdata);
                self.original_mode = self.mode;
                self.frameset_ok = false;
                self.mode = Mode::Text;
            }
            Tag::Xmp => {
                self.close_p_in_button_scope();
                self.reconstruct_formatting();
                self.frameset_ok = false;
                self.generic_text(t, State::Rawtext);
            }
            Tag::Iframe => {
                self.frameset_ok = false;
                self.generic_text(t, State::Rawtext);
            }
            Tag::Noembed => self.generic_text(t, State::Rawtext),
            Tag::Noscript if self.scripting => self.generic_text(t, State::Rawtext),
            Tag::Select => {
                if self.context_is(Tag::Select) {
                    return Step::Done;
                }
                if self.in_scope(Tag::Select, Scope::Default) {
                    self.pop_until(Tag::Select);
                    return Step::Done;
                }
                self.reconstruct_formatting();
                self.insert_html(t);
                self.frameset_ok = false;
            }
            Tag::Option => {
                if self.in_scope(Tag::Select, Scope::Default) {
                    self.generate_implied_end_tags(Some(Tag::Optgroup));
                } else if self.current_is(Tag::Option) {
                    self.pop_open();
                }
                self.reconstruct_formatting();
                self.insert_html(t);
            }
            Tag::Optgroup => {
                if self.in_scope(Tag::Select, Scope::Default) {
                    self.generate_implied_end_tags(None);
                } else if self.current_is(Tag::Option) {
                    self.pop_open();
                }
                self.reconstruct_formatting();
                self.insert_html(t);
            }
            Tag::Rb | Tag::Rtc => {
                if self.in_scope(Tag::Ruby, Scope::Default) {
                    self.generate_implied_end_tags(None);
                }
                self.insert_html(t);
            }
            Tag::Rp | Tag::Rt => {
                if self.in_scope(Tag::Ruby, Scope::Default) {
                    self.generate_implied_end_tags(Some(Tag::Rtc));
                }
                self.insert_html(t);
            }
            Tag::Math => {
                self.reconstruct_formatting();
                self.insert_foreign(t, Ns::MathMl);
            }
            Tag::Svg => {
                self.reconstruct_formatting();
                self.insert_foreign(t, Ns::Svg);
            }
            Tag::Caption
            | Tag::Col
            | Tag::Colgroup
            | Tag::Frame
            | Tag::Head
            | Tag::Tbody
            | Tag::Td
            | Tag::Tfoot
            | Tag::Th
            | Tag::Thead
            | Tag::Tr => {}
            _ => {
                self.reconstruct_formatting();
                self.insert_html(t);
            }
        }
        Step::Done
    }

    fn context_is(&self, tag: Tag) -> bool {
        self.context
            .as_ref()
            .is_some_and(|c| c.ns == Ns::Html && c.tag == tag)
    }

    fn any_other_end_tag(&mut self, tag: Tag, name: &[u8]) {
        if let Some(i) = self.open.html_end_target(tag, name) {
            self.truncate_open(i);
        }
    }

    fn in_body_end(&mut self, tok: &mut Tok<'_>) -> Step {
        let Tok::End(t) = tok else {
            return Step::Done;
        };
        let tag = t.tag();
        match tag {
            Tag::Template => return self.in_head(tok),
            Tag::Body => {
                if self.in_scope(Tag::Body, Scope::Default) {
                    self.mode = Mode::AfterBody;
                }
            }
            Tag::Html => {
                if self.in_scope(Tag::Body, Scope::Default) {
                    self.mode = Mode::AfterBody;
                    return Step::Again;
                }
            }
            Tag::Address
            | Tag::Article
            | Tag::Aside
            | Tag::Blockquote
            | Tag::Button
            | Tag::Center
            | Tag::Details
            | Tag::Dialog
            | Tag::Dir
            | Tag::Div
            | Tag::Dl
            | Tag::Fieldset
            | Tag::Figcaption
            | Tag::Figure
            | Tag::Footer
            | Tag::Header
            | Tag::Hgroup
            | Tag::Listing
            | Tag::Main
            | Tag::Menu
            | Tag::Nav
            | Tag::Ol
            | Tag::Pre
            | Tag::Search
            | Tag::Section
            | Tag::Select
            | Tag::Summary
            | Tag::Ul => {
                if self.in_scope(tag, Scope::Default) {
                    self.generate_implied_end_tags(None);
                    self.pop_until(tag);
                }
            }
            Tag::Form => {
                if self.parsing_template_contents() {
                    if self.in_scope(Tag::Form, Scope::Default) {
                        self.generate_implied_end_tags(None);
                        self.pop_until(Tag::Form);
                    }
                } else {
                    let node = self.form.take();
                    if let Some(node) = node
                        && let Some(i) = self.stack_position(node, Tag::Form)
                        && self.open.clear_of_boundaries_above(i, Scope::Default)
                    {
                        self.generate_implied_end_tags(None);
                        self.remove_from_stack(node, Tag::Form);
                    }
                }
            }
            Tag::P => {
                if !self.in_scope(Tag::P, Scope::Button) {
                    self.insert_html_tag(Tag::P);
                }
                self.close_p();
            }
            Tag::Li => {
                if self.in_scope(Tag::Li, Scope::ListItem) {
                    self.generate_implied_end_tags(Some(Tag::Li));
                    self.pop_until(Tag::Li);
                }
            }
            Tag::Dd | Tag::Dt => {
                if self.in_scope(tag, Scope::Default) {
                    self.generate_implied_end_tags(Some(tag));
                    self.pop_until(tag);
                }
            }
            Tag::H1 | Tag::H2 | Tag::H3 | Tag::H4 | Tag::H5 | Tag::H6 => {
                if self.any_in_scope(HEADINGS, Scope::Default) {
                    self.generate_implied_end_tags(None);
                    self.pop_until_any(HEADINGS);
                }
            }
            Tag::A
            | Tag::B
            | Tag::Big
            | Tag::Code
            | Tag::Em
            | Tag::Font
            | Tag::I
            | Tag::Nobr
            | Tag::S
            | Tag::Small
            | Tag::Strike
            | Tag::Strong
            | Tag::Tt
            | Tag::U => {
                if !self.adoption_agency(tag) {
                    self.any_other_end_tag(tag, &t.name);
                }
            }
            Tag::Applet | Tag::Marquee | Tag::Object => {
                if self.in_scope(tag, Scope::Default) {
                    self.generate_implied_end_tags(None);
                    self.pop_until(tag);
                    self.clear_afe_to_marker();
                }
            }
            Tag::Br => {
                self.reconstruct_formatting();
                self.insert_html_tag(Tag::Br);
                self.pop_open();
                self.frameset_ok = false;
            }
            _ => self.any_other_end_tag(tag, &t.name),
        }
        Step::Done
    }

    fn text(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let text: &[u8] = text;
                self.insert_text(text);
                Step::Done
            }
            Tok::Eof => {
                self.pop_open();
                self.mode = self.original_mode;
                Step::Again
            }
            Tok::End(_) => {
                self.pop_open();
                self.mode = self.original_mode;
                Step::Done
            }
            _ => Step::Done,
        }
    }

    fn table_anything_else(&mut self, tok: &mut Tok<'_>) -> Step {
        self.foster = true;
        let step = self.in_body(tok);
        self.foster = false;
        step
    }

    fn in_table(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(_)
                if self.open.last().is_some_and(|e| {
                    e.is_any(&[
                        Tag::Table,
                        Tag::Tbody,
                        Tag::Template,
                        Tag::Tfoot,
                        Tag::Thead,
                        Tag::Tr,
                    ])
                }) =>
            {
                self.pending_text.clear();
                self.original_mode = self.mode;
                self.mode = Mode::InTableText;
                Step::Again
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                Step::Done
            }
            Tok::Doctype(_) => Step::Done,
            Tok::Start(t) => match t.tag() {
                Tag::Caption => {
                    self.clear_to_context(&[Tag::Table]);
                    self.afe.push_marker();
                    self.insert_html(t);
                    self.mode = Mode::InCaption;
                    Step::Done
                }
                Tag::Colgroup => {
                    self.clear_to_context(&[Tag::Table]);
                    self.insert_html(t);
                    self.mode = Mode::InColumnGroup;
                    Step::Done
                }
                Tag::Col => {
                    self.clear_to_context(&[Tag::Table]);
                    self.insert_html_tag(Tag::Colgroup);
                    self.mode = Mode::InColumnGroup;
                    Step::Again
                }
                Tag::Tbody | Tag::Tfoot | Tag::Thead => {
                    self.clear_to_context(&[Tag::Table]);
                    self.insert_html(t);
                    self.mode = Mode::InTableBody;
                    Step::Done
                }
                Tag::Td | Tag::Th | Tag::Tr => {
                    self.clear_to_context(&[Tag::Table]);
                    self.insert_html_tag(Tag::Tbody);
                    self.mode = Mode::InTableBody;
                    Step::Again
                }
                Tag::Table => {
                    if !self.in_scope(Tag::Table, Scope::Table) {
                        return Step::Done;
                    }
                    self.pop_until(Tag::Table);
                    self.reset_insertion_mode();
                    Step::Again
                }
                Tag::Style | Tag::Script | Tag::Template => self.in_head(tok),
                Tag::Input
                    if t.attr(b"type")
                        .is_some_and(|v| v.eq_ignore_ascii_case(b"hidden")) =>
                {
                    self.insert_html(t);
                    self.pop_open();
                    Step::Done
                }
                Tag::Form => {
                    if self.form.is_some() && !self.parsing_template_contents() {
                        return Step::Done;
                    }
                    let h = self.insert_html(t);
                    if !self.parsing_template_contents() {
                        self.form = Some(h);
                    }
                    self.pop_open();
                    Step::Done
                }
                _ => self.table_anything_else(tok),
            },
            Tok::End(t) => match t.tag() {
                Tag::Table => {
                    if self.in_scope(Tag::Table, Scope::Table) {
                        self.pop_until(Tag::Table);
                        self.reset_insertion_mode();
                    }
                    Step::Done
                }
                Tag::Body
                | Tag::Caption
                | Tag::Col
                | Tag::Colgroup
                | Tag::Html
                | Tag::Tbody
                | Tag::Td
                | Tag::Tfoot
                | Tag::Th
                | Tag::Thead
                | Tag::Tr => Step::Done,
                Tag::Template => self.in_head(tok),
                _ => self.table_anything_else(tok),
            },
            Tok::Eof => self.in_body(tok),
            Tok::Chars(_) => self.table_anything_else(tok),
        }
    }

    fn in_table_text(&mut self, tok: &mut Tok<'_>) -> Step {
        if let Tok::Chars(text) = tok {
            self.pending_text
                .extend(text.iter().copied().filter(|&c| c != 0));
            return Step::Done;
        }
        let pending = core::mem::take(&mut self.pending_text);
        if !pending.is_empty() {
            if all_ws(&pending) {
                self.insert_text(&pending);
            } else {
                self.foster = true;
                self.body_chars(&pending);
                self.foster = false;
            }
        }
        self.pending_text = pending;
        self.pending_text.clear();
        self.mode = self.original_mode;
        Step::Again
    }

    fn close_caption(&mut self) -> bool {
        if !self.in_scope(Tag::Caption, Scope::Table) {
            return false;
        }
        self.generate_implied_end_tags(None);
        self.pop_until(Tag::Caption);
        self.clear_afe_to_marker();
        self.mode = Mode::InTable;
        true
    }

    fn in_caption(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::End(t) if t.tag() == Tag::Caption => {
                self.close_caption();
                Step::Done
            }
            Tok::Start(t)
                if matches!(
                    t.tag(),
                    Tag::Caption
                        | Tag::Col
                        | Tag::Colgroup
                        | Tag::Tbody
                        | Tag::Td
                        | Tag::Tfoot
                        | Tag::Th
                        | Tag::Thead
                        | Tag::Tr
                ) =>
            {
                if self.close_caption() {
                    Step::Again
                } else {
                    Step::Done
                }
            }
            Tok::End(t) if t.tag() == Tag::Table => {
                if self.close_caption() {
                    Step::Again
                } else {
                    Step::Done
                }
            }
            Tok::End(t)
                if matches!(
                    t.tag(),
                    Tag::Body
                        | Tag::Col
                        | Tag::Colgroup
                        | Tag::Html
                        | Tag::Tbody
                        | Tag::Td
                        | Tag::Tfoot
                        | Tag::Th
                        | Tag::Thead
                        | Tag::Tr
                ) =>
            {
                Step::Done
            }
            _ => self.in_body(tok),
        }
    }

    fn in_column_group(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let (ws, rest) = split_ws(text);
                self.insert_text(ws);
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                return Step::Done;
            }
            Tok::Doctype(_) => return Step::Done,
            Tok::Start(t) => match t.tag() {
                Tag::Html => return self.in_body(tok),
                Tag::Col => {
                    self.insert_html(t);
                    self.pop_open();
                    return Step::Done;
                }
                Tag::Template => return self.in_head(tok),
                _ => {}
            },
            Tok::End(t) => match t.tag() {
                Tag::Colgroup => {
                    if self.current_is(Tag::Colgroup) {
                        self.pop_open();
                        self.mode = Mode::InTable;
                    }
                    return Step::Done;
                }
                Tag::Col => return Step::Done,
                Tag::Template => return self.in_head(tok),
                _ => {}
            },
            Tok::Eof => return self.in_body(tok),
        }
        if !self.current_is(Tag::Colgroup) {
            return Step::Done;
        }
        self.pop_open();
        self.mode = Mode::InTable;
        Step::Again
    }

    fn in_table_body(&mut self, tok: &mut Tok<'_>) -> Step {
        const SECTIONS: &[Tag] = &[Tag::Tbody, Tag::Thead, Tag::Tfoot];
        match tok {
            Tok::Start(t) if t.tag() == Tag::Tr => {
                self.clear_to_context(SECTIONS);
                self.insert_html(t);
                self.mode = Mode::InRow;
                Step::Done
            }
            Tok::Start(t) if matches!(t.tag(), Tag::Th | Tag::Td) => {
                self.clear_to_context(SECTIONS);
                self.insert_html_tag(Tag::Tr);
                self.mode = Mode::InRow;
                Step::Again
            }
            Tok::End(t) if SECTIONS.contains(&t.tag()) => {
                if self.in_scope(t.tag(), Scope::Table) {
                    self.clear_to_context(SECTIONS);
                    self.pop_open();
                    self.mode = Mode::InTable;
                }
                Step::Done
            }
            Tok::Start(t)
                if matches!(
                    t.tag(),
                    Tag::Caption | Tag::Col | Tag::Colgroup | Tag::Tbody | Tag::Tfoot | Tag::Thead
                ) =>
            {
                self.leave_table_body()
            }
            Tok::End(t) if t.tag() == Tag::Table => self.leave_table_body(),
            Tok::End(t)
                if matches!(
                    t.tag(),
                    Tag::Body
                        | Tag::Caption
                        | Tag::Col
                        | Tag::Colgroup
                        | Tag::Html
                        | Tag::Td
                        | Tag::Th
                        | Tag::Tr
                ) =>
            {
                Step::Done
            }
            _ => self.in_table(tok),
        }
    }

    fn leave_table_body(&mut self) -> Step {
        const SECTIONS: &[Tag] = &[Tag::Tbody, Tag::Thead, Tag::Tfoot];
        if !self.any_in_scope(SECTIONS, Scope::Table) {
            return Step::Done;
        }
        self.clear_to_context(SECTIONS);
        self.pop_open();
        self.mode = Mode::InTable;
        Step::Again
    }

    fn close_row(&mut self) -> bool {
        if !self.in_scope(Tag::Tr, Scope::Table) {
            return false;
        }
        self.clear_to_context(&[Tag::Tr]);
        self.pop_open();
        self.mode = Mode::InTableBody;
        true
    }

    fn in_row(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Start(t) if matches!(t.tag(), Tag::Th | Tag::Td) => {
                self.clear_to_context(&[Tag::Tr]);
                self.insert_html(t);
                self.mode = Mode::InCell;
                self.afe.push_marker();
                Step::Done
            }
            Tok::End(t) if t.tag() == Tag::Tr => {
                self.close_row();
                Step::Done
            }
            Tok::Start(t)
                if matches!(
                    t.tag(),
                    Tag::Caption
                        | Tag::Col
                        | Tag::Colgroup
                        | Tag::Tbody
                        | Tag::Tfoot
                        | Tag::Thead
                        | Tag::Tr
                ) =>
            {
                if self.close_row() {
                    Step::Again
                } else {
                    Step::Done
                }
            }
            Tok::End(t) if t.tag() == Tag::Table => {
                if self.close_row() {
                    Step::Again
                } else {
                    Step::Done
                }
            }
            Tok::End(t) if matches!(t.tag(), Tag::Tbody | Tag::Tfoot | Tag::Thead) => {
                if !self.in_scope(t.tag(), Scope::Table) {
                    return Step::Done;
                }
                if self.close_row() {
                    Step::Again
                } else {
                    Step::Done
                }
            }
            Tok::End(t)
                if matches!(
                    t.tag(),
                    Tag::Body
                        | Tag::Caption
                        | Tag::Col
                        | Tag::Colgroup
                        | Tag::Html
                        | Tag::Td
                        | Tag::Th
                ) =>
            {
                Step::Done
            }
            _ => self.in_table(tok),
        }
    }

    fn close_cell(&mut self) {
        self.generate_implied_end_tags(None);
        self.pop_until_any(&[Tag::Td, Tag::Th]);
        self.clear_afe_to_marker();
        self.mode = Mode::InRow;
    }

    fn in_cell(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::End(t) if matches!(t.tag(), Tag::Td | Tag::Th) => {
                let tag = t.tag();
                if self.in_scope(tag, Scope::Table) {
                    self.generate_implied_end_tags(None);
                    self.pop_until(tag);
                    self.clear_afe_to_marker();
                    self.mode = Mode::InRow;
                }
                Step::Done
            }
            Tok::Start(t)
                if matches!(
                    t.tag(),
                    Tag::Caption
                        | Tag::Col
                        | Tag::Colgroup
                        | Tag::Tbody
                        | Tag::Td
                        | Tag::Tfoot
                        | Tag::Th
                        | Tag::Thead
                        | Tag::Tr
                ) =>
            {
                if !self.any_in_scope(&[Tag::Td, Tag::Th], Scope::Table) {
                    return Step::Done;
                }
                self.close_cell();
                Step::Again
            }
            Tok::End(t)
                if matches!(
                    t.tag(),
                    Tag::Body | Tag::Caption | Tag::Col | Tag::Colgroup | Tag::Html
                ) =>
            {
                Step::Done
            }
            Tok::End(t)
                if matches!(
                    t.tag(),
                    Tag::Table | Tag::Tbody | Tag::Tfoot | Tag::Thead | Tag::Tr
                ) =>
            {
                if !self.in_scope(t.tag(), Scope::Table) {
                    return Step::Done;
                }
                self.close_cell();
                Step::Again
            }
            _ => self.in_body(tok),
        }
    }

    fn switch_template_mode(&mut self, mode: Mode) -> Step {
        self.template_modes.pop();
        self.template_modes.push(mode);
        self.mode = mode;
        Step::Again
    }

    fn in_template(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(_) | Tok::Comment(_) | Tok::Pi(..) | Tok::Doctype(_) => self.in_body(tok),
            Tok::Start(t) => match t.tag() {
                Tag::Base
                | Tag::Basefont
                | Tag::Bgsound
                | Tag::Link
                | Tag::Meta
                | Tag::Noframes
                | Tag::Script
                | Tag::Style
                | Tag::Template
                | Tag::Title => self.in_head(tok),
                Tag::Caption | Tag::Colgroup | Tag::Tbody | Tag::Tfoot | Tag::Thead => {
                    self.switch_template_mode(Mode::InTable)
                }
                Tag::Col => self.switch_template_mode(Mode::InColumnGroup),
                Tag::Tr => self.switch_template_mode(Mode::InTableBody),
                Tag::Td | Tag::Th => self.switch_template_mode(Mode::InRow),
                _ => self.switch_template_mode(Mode::InBody),
            },
            Tok::End(t) if t.tag() == Tag::Template => self.in_head(tok),
            Tok::End(_) => Step::Done,
            Tok::Eof => {
                if !self.has_template() {
                    return self.stop();
                }
                self.pop_until(Tag::Template);
                self.clear_afe_to_marker();
                self.template_modes.pop();
                self.reset_insertion_mode();
                Step::Again
            }
        }
    }

    fn after_body(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let (ws, rest) = split_ws(text);
                if !ws.is_empty() {
                    self.body_chars(ws);
                }
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                let root = self.open[0].h;
                self.append_comment_to(root, tok);
                return Step::Done;
            }
            Tok::Doctype(_) => return Step::Done,
            Tok::Start(t) if t.tag() == Tag::Html => return self.in_body(tok),
            Tok::End(t) if t.tag() == Tag::Html => {
                if self.context.is_none() {
                    self.mode = Mode::AfterAfterBody;
                }
                return Step::Done;
            }
            Tok::Eof => return self.stop(),
            _ => {}
        }
        self.mode = Mode::InBody;
        Step::Again
    }

    fn frameset_ws(&mut self, text: &[u8]) {
        let ws: Vec<u8> = text.iter().copied().filter(|&c| is_ws(c)).collect();
        self.insert_text(&ws);
    }

    fn in_frameset(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let text: &[u8] = text;
                self.frameset_ws(text);
            }
            Tok::Comment(_) | Tok::Pi(..) => self.insert_comment(tok),
            Tok::Start(t) => match t.tag() {
                Tag::Html => return self.in_body(tok),
                Tag::Frameset => {
                    self.insert_html(t);
                }
                Tag::Frame => {
                    self.insert_html(t);
                    self.pop_open();
                }
                Tag::Noframes => return self.in_head(tok),
                _ => {}
            },
            Tok::End(t) if t.tag() == Tag::Frameset => {
                if self.open.len() > 1 {
                    self.pop_open();
                    if self.context.is_none() && !self.current_is(Tag::Frameset) {
                        self.mode = Mode::AfterFrameset;
                    }
                }
            }
            Tok::Eof => return self.stop(),
            _ => {}
        }
        Step::Done
    }

    fn after_frameset(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let text: &[u8] = text;
                self.frameset_ws(text);
            }
            Tok::Comment(_) | Tok::Pi(..) => self.insert_comment(tok),
            Tok::Start(t) if t.tag() == Tag::Html => return self.in_body(tok),
            Tok::Start(t) if t.tag() == Tag::Noframes => return self.in_head(tok),
            Tok::End(t) if t.tag() == Tag::Html => self.mode = Mode::AfterAfterFrameset,
            Tok::Eof => return self.stop(),
            _ => {}
        }
        Step::Done
    }

    fn after_after_body(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Comment(_) | Tok::Pi(..) => {
                let doc = self.doc;
                self.append_comment_to(doc, tok);
                return Step::Done;
            }
            Tok::Doctype(_) => return self.in_body(tok),
            Tok::Chars(text) => {
                let (ws, rest) = split_ws(text);
                if !ws.is_empty() {
                    self.body_chars(ws);
                }
                if rest.is_empty() {
                    return Step::Done;
                }
                *tok = Tok::Chars(rest);
            }
            Tok::Start(t) if t.tag() == Tag::Html => return self.in_body(tok),
            Tok::Eof => return self.stop(),
            _ => {}
        }
        self.mode = Mode::InBody;
        Step::Again
    }

    fn after_after_frameset(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Comment(_) | Tok::Pi(..) => {
                let doc = self.doc;
                self.append_comment_to(doc, tok);
            }
            Tok::Doctype(_) => return self.in_body(tok),
            Tok::Chars(text) => {
                let ws: Vec<u8> = text.iter().copied().filter(|&c| is_ws(c)).collect();
                if !ws.is_empty() {
                    self.body_chars(&ws);
                }
            }
            Tok::Start(t) if t.tag() == Tag::Html => return self.in_body(tok),
            Tok::Start(t) if t.tag() == Tag::Noframes => return self.in_head(tok),
            Tok::Eof => return self.stop(),
            _ => {}
        }
        Step::Done
    }

    fn foreign_chars(&mut self, text: &[u8]) {
        if text.contains(&0) {
            let mut fixed = Vec::with_capacity(text.len() + 8);
            for &c in text {
                if c == 0 {
                    fixed.extend_from_slice(REPLACEMENT);
                } else {
                    fixed.push(c);
                }
            }
            self.insert_text(&fixed);
        } else {
            self.insert_text(text);
        }
        if !text.iter().all(|&c| is_ws(c) || c == 0) {
            self.frameset_ok = false;
        }
    }

    fn breaks_out(token: &TagToken, start: bool) -> bool {
        let tag = token.tag();
        if !start {
            return matches!(tag, Tag::Br | Tag::P);
        }
        BREAKOUT.contains(&tag)
            || (tag == Tag::Font
                && token
                    .attrs()
                    .iter()
                    .any(|a| matches!(a.name.as_slice(), b"color" | b"face" | b"size")))
    }

    fn foreign(&mut self, tok: &mut Tok<'_>) -> Step {
        match tok {
            Tok::Chars(text) => {
                let text: &[u8] = text;
                self.foreign_chars(text);
                Step::Done
            }
            Tok::Comment(_) | Tok::Pi(..) => {
                self.insert_comment(tok);
                Step::Done
            }
            Tok::Doctype(_) => Step::Done,
            Tok::Start(t) if Self::breaks_out(t, true) => self.break_out(tok),
            Tok::End(t) if Self::breaks_out(t, false) => self.break_out(tok),
            Tok::Start(t) => {
                let ns = self.adjusted_current().map_or(Ns::Html, |(_, ns, ..)| ns);
                self.insert_foreign(t, ns);
                Step::Done
            }
            Tok::End(t) => {
                if self.open.len() <= 1 {
                    return Step::Done;
                }
                if let Some(i) = self.open.foreign_end_target(t.tag(), &t.name) {
                    self.truncate_open(i);
                    return Step::Done;
                }
                self.by_mode(tok)
            }
            Tok::Eof => self.by_mode(tok),
        }
    }

    fn break_out(&mut self, tok: &mut Tok<'_>) -> Step {
        while let Some(e) = self.open.last() {
            if e.ns == Ns::Html || e.is_mathml_text_ip() || e.is_html_ip() {
                break;
            }
            self.pop_open();
        }
        self.by_mode(tok)
    }

    fn start_fragment(&mut self, context: Context<'_>) -> Option<State> {
        let lower = context.name.to_ascii_lowercase();
        let tag = Tag::from_bytes(&lower);
        self.context = Some(ContextElem {
            tag,
            ns: context.ns,
        });
        let h = self.create_element(Ns::Html, Tag::Html, b"html", &[], NO_POSITION);
        let doc = self.doc;
        self.sink.append(doc, h);
        self.push(h, Ns::Html, Tag::Html, b"html", false);
        self.fragment_root = Some(h);
        if context.ns == Ns::Html && tag == Tag::Template {
            self.template_modes.push(Mode::InTemplate);
        }
        self.reset_insertion_mode();
        if context.ns != Ns::Html {
            return None;
        }
        match tag {
            Tag::Title | Tag::Textarea => Some(State::Rcdata),
            Tag::Style | Tag::Xmp | Tag::Iframe | Tag::Noembed | Tag::Noframes => {
                Some(State::Rawtext)
            }
            Tag::Noscript if self.scripting => Some(State::Rawtext),
            Tag::Script => Some(State::ScriptData),
            Tag::Plaintext => Some(State::Plaintext),
            _ => None,
        }
    }
}

fn normalize_newlines(input: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len());
    let mut i = 0;
    while i < input.len() {
        let c = input[i];
        if c == b'\r' {
            out.push(b'\n');
            if input.get(i + 1) == Some(&b'\n') {
                i += 1;
            }
        } else {
            out.push(c);
        }
        i += 1;
    }
    out
}

pub fn run<S: TreeSink>(
    input: &[u8],
    scripting: bool,
    context: Option<Context<'_>>,
    sink: &mut S,
) -> QuirksMode {
    let normalized;
    let input = if input.contains(&b'\r') {
        normalized = normalize_newlines(input);
        &normalized[..]
    } else {
        input
    };
    let mut tokenizer = Tokenizer::new(input);
    let mut builder = Builder::new(sink, scripting);
    let fragment = context.is_some();
    if let Some(context) = context
        && let Some(state) = builder.start_fragment(context)
    {
        tokenizer.state = state;
    }
    tokenizer.allow_cdata = builder.adjusted_current_foreign();
    loop {
        let kind = tokenizer.next_token();
        {
            let mut tok = match kind {
                Kind::Chars => Tok::Chars(tokenizer.text()),
                Kind::StartTag => Tok::Start(&mut tokenizer.tag),
                Kind::EndTag => Tok::End(&mut tokenizer.tag),
                Kind::Comment => Tok::Comment(&tokenizer.comment),
                Kind::Pi => Tok::Pi(&tokenizer.pi_target, &tokenizer.comment),
                Kind::Doctype => Tok::Doctype(&tokenizer.doctype),
                Kind::Eof => Tok::Eof,
            };
            builder.process(&mut tok);
        }
        if let Some(state) = builder.next_state.take() {
            tokenizer.state = state;
        }
        tokenizer.allow_cdata = builder.adjusted_current_foreign();
        if kind == Kind::Eof {
            break;
        }
    }
    if fragment && let Some(root) = builder.fragment_root {
        let doc = builder.doc;
        builder.sink.reparent_children(root, doc);
        builder.sink.remove(root);
        builder.sink.discard(root);
    }
    builder.quirks
}

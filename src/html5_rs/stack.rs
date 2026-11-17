//! Nordstjernen — the stack of open elements and the list of active formatting elements, indexed so the tree builder's scope, end-tag and Noah's Ark checks need no walk.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use std::collections::HashMap;
use std::hash::{BuildHasher, RandomState};

use crate::Ns;
use crate::tags::Tag;
use crate::tokenizer::Attr;

const TAG_COUNT: usize = Tag::Other as usize + 1;
const NONE: u32 = u32::MAX;
pub(crate) const UNNAMED: u32 = u32::MAX;
const HTML: usize = 0;
const FOREIGN: usize = 1;
const SPECIAL: usize = 4;
const COUNTERS: usize = 5;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Scope {
    Default,
    ListItem,
    Button,
    Table,
}

pub(crate) struct Elem<H> {
    pub(crate) h: H,
    pub(crate) tag: Tag,
    pub(crate) ns: Ns,
    pub(crate) name: u32,
    pub(crate) html_ip: bool,
}

impl<H> Elem<H> {
    pub(crate) fn is(&self, tag: Tag) -> bool {
        self.ns == Ns::Html && self.tag == tag
    }

    pub(crate) fn is_any(&self, tags: &[Tag]) -> bool {
        self.ns == Ns::Html && tags.contains(&self.tag)
    }

    pub(crate) fn is_mathml_text_ip(&self) -> bool {
        self.ns == Ns::MathMl
            && matches!(self.tag, Tag::Mi | Tag::Mo | Tag::Mn | Tag::Ms | Tag::Mtext)
    }

    pub(crate) fn is_html_ip(&self) -> bool {
        match self.ns {
            Ns::MathMl => self.html_ip,
            Ns::Svg => matches!(self.tag, Tag::ForeignObject | Tag::Desc | Tag::Title),
            Ns::Html => false,
        }
    }

    pub(crate) fn is_special(&self) -> bool {
        match self.ns {
            Ns::Html => matches!(
                self.tag,
                Tag::Address
                    | Tag::Applet
                    | Tag::Area
                    | Tag::Article
                    | Tag::Aside
                    | Tag::Base
                    | Tag::Basefont
                    | Tag::Bgsound
                    | Tag::Blockquote
                    | Tag::Body
                    | Tag::Br
                    | Tag::Button
                    | Tag::Caption
                    | Tag::Center
                    | Tag::Col
                    | Tag::Colgroup
                    | Tag::Dd
                    | Tag::Details
                    | Tag::Dir
                    | Tag::Div
                    | Tag::Dl
                    | Tag::Dt
                    | Tag::Embed
                    | Tag::Fieldset
                    | Tag::Figcaption
                    | Tag::Figure
                    | Tag::Footer
                    | Tag::Form
                    | Tag::Frame
                    | Tag::Frameset
                    | Tag::H1
                    | Tag::H2
                    | Tag::H3
                    | Tag::H4
                    | Tag::H5
                    | Tag::H6
                    | Tag::Head
                    | Tag::Header
                    | Tag::Hgroup
                    | Tag::Hr
                    | Tag::Html
                    | Tag::Iframe
                    | Tag::Image
                    | Tag::Img
                    | Tag::Input
                    | Tag::Keygen
                    | Tag::Li
                    | Tag::Link
                    | Tag::Listing
                    | Tag::Main
                    | Tag::Marquee
                    | Tag::Menu
                    | Tag::Meta
                    | Tag::Nav
                    | Tag::Noembed
                    | Tag::Noframes
                    | Tag::Noscript
                    | Tag::Object
                    | Tag::Ol
                    | Tag::P
                    | Tag::Param
                    | Tag::Plaintext
                    | Tag::Pre
                    | Tag::Script
                    | Tag::Search
                    | Tag::Section
                    | Tag::Select
                    | Tag::Source
                    | Tag::Style
                    | Tag::Summary
                    | Tag::Table
                    | Tag::Tbody
                    | Tag::Td
                    | Tag::Template
                    | Tag::Textarea
                    | Tag::Tfoot
                    | Tag::Th
                    | Tag::Thead
                    | Tag::Title
                    | Tag::Tr
                    | Tag::Track
                    | Tag::Ul
                    | Tag::Wbr
                    | Tag::Xmp
            ),
            Ns::MathMl => matches!(
                self.tag,
                Tag::Mi | Tag::Mo | Tag::Mn | Tag::Ms | Tag::Mtext | Tag::AnnotationXml
            ),
            Ns::Svg => matches!(self.tag, Tag::ForeignObject | Tag::Desc | Tag::Title),
        }
    }

    fn default_boundary(&self) -> bool {
        match self.ns {
            Ns::Html => matches!(
                self.tag,
                Tag::Applet
                    | Tag::Caption
                    | Tag::Html
                    | Tag::Table
                    | Tag::Td
                    | Tag::Th
                    | Tag::Marquee
                    | Tag::Object
                    | Tag::Template
                    | Tag::Select
            ),
            Ns::MathMl => matches!(
                self.tag,
                Tag::Mi | Tag::Mo | Tag::Mn | Tag::Ms | Tag::Mtext | Tag::AnnotationXml
            ),
            Ns::Svg => matches!(self.tag, Tag::ForeignObject | Tag::Desc | Tag::Title),
        }
    }

    pub(crate) fn bounds(&self, scope: Scope) -> bool {
        match scope {
            Scope::Default => self.default_boundary(),
            Scope::ListItem => self.default_boundary() || self.is_any(&[Tag::Ol, Tag::Ul]),
            Scope::Button => self.default_boundary() || self.is(Tag::Button),
            Scope::Table => self.is_any(&[Tag::Html, Tag::Table, Tag::Template]),
        }
    }

    fn counters(&self) -> [bool; COUNTERS] {
        let base = self.default_boundary();
        [
            base,
            base || self.is_any(&[Tag::Ol, Tag::Ul]),
            base || self.is(Tag::Button),
            self.bounds(Scope::Table),
            self.is_special(),
        ]
    }

    fn class(&self) -> usize {
        if self.ns == Ns::Html { HTML } else { FOREIGN }
    }
}

struct Link {
    serial: usize,
    below_same: u32,
    last_html: u32,
    counts: [u32; COUNTERS],
}

fn index(raw: u32) -> Option<usize> {
    (raw != NONE).then_some(raw as usize)
}

pub(crate) struct OpenStack<H> {
    elems: Vec<Elem<H>>,
    links: Vec<Link>,
    open_serials: Vec<bool>,
    tops: [[u32; 2]; TAG_COUNT],
    names: HashMap<Box<[u8]>, u32>,
    named_tops: Vec<[u32; 2]>,
    last_name: (Vec<u8>, u32),
}

impl<H> OpenStack<H> {
    pub(crate) fn new() -> Self {
        OpenStack {
            elems: Vec::new(),
            links: Vec::new(),
            open_serials: Vec::new(),
            tops: [[NONE; 2]; TAG_COUNT],
            names: HashMap::new(),
            named_tops: Vec::new(),
            last_name: (Vec::new(), UNNAMED),
        }
    }

    fn name_id(&mut self, name: &[u8]) -> Option<u32> {
        if self.last_name.1 != UNNAMED && self.last_name.0 == name {
            return Some(self.last_name.1);
        }
        let id = *self.names.get(name)?;
        self.last_name.0.clear();
        self.last_name.0.extend_from_slice(name);
        self.last_name.1 = id;
        Some(id)
    }

    pub(crate) fn intern(&mut self, name: &[u8]) -> u32 {
        if let Some(id) = self.name_id(name) {
            return id;
        }
        let id = self.named_tops.len() as u32;
        self.named_tops.push([NONE; 2]);
        self.names.insert(name.into(), id);
        id
    }

    fn top_slot(&mut self, elem: &Elem<H>) -> &mut u32 {
        let class = elem.class();
        match self.named_tops.get_mut(elem.name as usize) {
            Some(tops) => &mut tops[class],
            None => &mut self.tops[elem.tag as usize][class],
        }
    }

    fn attach(&mut self, elem: Elem<H>, serial: usize) {
        let position = self.elems.len() as u32;
        let below = self.links.last();
        let mut counts = below.map_or([0; COUNTERS], |link| link.counts);
        for (count, hit) in counts.iter_mut().zip(elem.counters()) {
            *count += u32::from(hit);
        }
        let last_html = if elem.ns == Ns::Html {
            position
        } else {
            below.map_or(NONE, |link| link.last_html)
        };
        let below_same = core::mem::replace(self.top_slot(&elem), position);
        self.links.push(Link {
            serial,
            below_same,
            last_html,
            counts,
        });
        self.elems.push(elem);
    }

    fn detach(&mut self) -> Option<(Elem<H>, usize)> {
        let elem = self.elems.pop()?;
        let link = self.links.pop()?;
        *self.top_slot(&elem) = link.below_same;
        Some((elem, link.serial))
    }

    fn new_serial(&mut self) -> usize {
        self.open_serials.push(true);
        self.open_serials.len() - 1
    }

    fn rewind(&mut self, len: usize) -> Vec<(Elem<H>, usize)> {
        let mut tail = Vec::with_capacity(self.elems.len().saturating_sub(len));
        while self.elems.len() > len {
            if let Some(entry) = self.detach() {
                tail.push(entry);
            }
        }
        tail.reverse();
        tail
    }

    pub(crate) fn push(&mut self, elem: Elem<H>) {
        let serial = self.new_serial();
        self.attach(elem, serial);
    }

    pub(crate) fn pop(&mut self) -> Option<Elem<H>> {
        let (elem, serial) = self.detach()?;
        self.open_serials[serial] = false;
        Some(elem)
    }

    pub(crate) fn truncate(&mut self, len: usize) {
        while self.elems.len() > len {
            self.pop();
        }
    }

    pub(crate) fn clear(&mut self) {
        self.truncate(0);
    }

    pub(crate) fn remove_all(&mut self, ascending: &[usize]) {
        let Some(&first) = ascending.first() else {
            return;
        };
        let tail = self.rewind(first);
        let mut doomed = ascending.iter().peekable();
        for (offset, (elem, serial)) in tail.into_iter().enumerate() {
            if doomed.next_if(|&&i| i == first + offset).is_some() {
                self.open_serials[serial] = false;
            } else {
                self.attach(elem, serial);
            }
        }
    }

    pub(crate) fn remove(&mut self, position: usize) {
        self.remove_all(&[position]);
    }

    pub(crate) fn insert(&mut self, position: usize, elem: Elem<H>) -> usize {
        let tail = self.rewind(position);
        let serial = self.new_serial();
        self.attach(elem, serial);
        for (elem, serial) in tail {
            self.attach(elem, serial);
        }
        serial
    }

    pub(crate) fn replace_handle(&mut self, position: usize, h: H) -> usize {
        let serial = self.new_serial();
        let link = &mut self.links[position];
        self.open_serials[link.serial] = false;
        link.serial = serial;
        self.elems[position].h = h;
        serial
    }

    pub(crate) fn holds(&self, serial: usize) -> bool {
        self.open_serials.get(serial).copied().unwrap_or(false)
    }

    pub(crate) fn serial_at(&self, position: usize) -> usize {
        self.links[position].serial
    }

    pub(crate) fn top_serial(&self) -> usize {
        self.links.last().map_or(usize::MAX, |link| link.serial)
    }

    pub(crate) fn top_html(&self, tag: Tag) -> Option<usize> {
        index(self.tops[tag as usize][HTML])
    }

    pub(crate) fn has_html(&self, tag: Tag) -> bool {
        self.top_html(tag).is_some()
    }

    pub(crate) fn below_same(&self, position: usize) -> Option<usize> {
        index(self.links[position].below_same)
    }

    fn clear_above(&self, position: usize, counter: usize) -> bool {
        self.links
            .last()
            .is_some_and(|top| top.counts[counter] == self.links[position].counts[counter])
    }

    pub(crate) fn clear_of_boundaries_above(&self, position: usize, scope: Scope) -> bool {
        self.clear_above(position, scope as usize)
    }

    pub(crate) fn in_scope(&self, tags: &[Tag], scope: Scope) -> bool {
        tags.iter()
            .filter_map(|&tag| self.top_html(tag))
            .max()
            .is_some_and(|position| self.clear_above(position, scope as usize))
    }

    fn topmost_named(&mut self, tag: Tag, name: &[u8], class: usize) -> Option<usize> {
        index(if tag == Tag::Other {
            self.name_id(name)
                .map_or(NONE, |id| self.named_tops[id as usize][class])
        } else {
            self.tops[tag as usize][class]
        })
    }

    pub(crate) fn html_end_target(&mut self, tag: Tag, name: &[u8]) -> Option<usize> {
        self.topmost_named(tag, name, HTML)
            .filter(|&position| self.clear_above(position, SPECIAL))
    }

    pub(crate) fn foreign_end_target(&mut self, tag: Tag, name: &[u8]) -> Option<usize> {
        let position = self.topmost_named(tag, name, FOREIGN)?;
        let last_html = self.links.last().and_then(|link| index(link.last_html));
        last_html
            .is_none_or(|html| position > html)
            .then_some(position)
    }
}

impl<H> core::ops::Deref for OpenStack<H> {
    type Target = [Elem<H>];

    fn deref(&self) -> &[Elem<H>] {
        &self.elems
    }
}

const FORMATTING_TAGS: usize = 14;

fn formatting_slot(tag: Tag) -> Option<usize> {
    Some(match tag {
        Tag::A => 0,
        Tag::B => 1,
        Tag::Big => 2,
        Tag::Code => 3,
        Tag::Em => 4,
        Tag::Font => 5,
        Tag::I => 6,
        Tag::Nobr => 7,
        Tag::S => 8,
        Tag::Small => 9,
        Tag::Strike => 10,
        Tag::Strong => 11,
        Tag::Tt => 12,
        Tag::U => 13,
        _ => return None,
    })
}

pub(crate) enum Fmt<H> {
    Marker,
    El {
        h: H,
        serial: usize,
        tag: Tag,
        attrs: Vec<Attr>,
        key: Option<u64>,
        level: u32,
    },
}

impl<H: Copy + Eq> Fmt<H> {
    pub(crate) fn handle(&self) -> Option<H> {
        match self {
            Fmt::Marker => None,
            Fmt::El { h, .. } => Some(*h),
        }
    }
}

#[derive(Default)]
struct Level {
    tags: [u32; FORMATTING_TAGS],
    keyed: u16,
    keys: HashMap<u64, u32>,
}

impl Level {
    fn reset(&mut self) {
        self.tags = [0; FORMATTING_TAGS];
        self.keyed = 0;
        self.keys.clear();
    }

    fn is_keyed(&self, slot: usize) -> bool {
        self.keyed & (1 << slot) != 0
    }
}

pub(crate) struct Formatting<H> {
    entries: Vec<Fmt<H>>,
    levels: Vec<Level>,
    depth: usize,
    listed_serials: Vec<bool>,
    hasher: RandomState,
}

impl<H> Formatting<H> {
    pub(crate) fn new() -> Self {
        Formatting {
            entries: Vec::new(),
            levels: vec![Level::default()],
            depth: 0,
            listed_serials: Vec::new(),
            hasher: RandomState::new(),
        }
    }

    fn key(&self, tag: Tag, attrs: &[Attr]) -> u64 {
        let seed = (tag as u64)
            .wrapping_add(attrs.len() as u64)
            .wrapping_mul(0x9E37_79B9_7F4A_7C15);
        attrs.iter().fold(seed, |sum, attr| {
            sum.wrapping_add(
                self.hasher
                    .hash_one((&attr.name, &attr.value, attr.ns as u8)),
            )
        })
    }

    pub(crate) fn count_tag(&self, tag: Tag) -> u32 {
        formatting_slot(tag).map_or(0, |slot| self.levels[self.depth].tags[slot])
    }

    fn key_existing(&mut self, tag: Tag, slot: usize) {
        let mut keys = Vec::new();
        for (i, entry) in self.entries.iter().enumerate().rev() {
            match entry {
                Fmt::Marker => break,
                Fmt::El { tag: t, attrs, .. } if *t == tag => keys.push((i, self.key(tag, attrs))),
                _ => {}
            }
        }
        let level = &mut self.levels[self.depth];
        level.keyed |= 1 << slot;
        for &(_, key) in &keys {
            *level.keys.entry(key).or_insert(0) += 1;
        }
        for (i, new_key) in keys {
            if let Fmt::El { key, .. } = &mut self.entries[i] {
                *key = Some(new_key);
            }
        }
    }

    pub(crate) fn identical_candidates(
        &mut self,
        tag: Tag,
        attrs: &[Attr],
    ) -> (usize, Option<u64>) {
        let Some(slot) = formatting_slot(tag) else {
            return (0, None);
        };
        if !self.levels[self.depth].is_keyed(slot) {
            if self.levels[self.depth].tags[slot] < 3 {
                return (0, None);
            }
            self.key_existing(tag, slot);
        }
        let key = self.key(tag, attrs);
        let count = self.levels[self.depth].keys.get(&key).copied().unwrap_or(0);
        (count as usize, Some(key))
    }

    pub(crate) fn lists(&self, serial: usize) -> bool {
        self.listed_serials.get(serial).copied().unwrap_or(false)
    }

    fn list_serial(&mut self, serial: usize, listed: bool) {
        if serial >= self.listed_serials.len() {
            if !listed {
                return;
            }
            self.listed_serials.resize(serial + 1, false);
        }
        self.listed_serials[serial] = listed;
    }

    fn account(&mut self, entry: &Fmt<H>, listed: bool) {
        let Fmt::El {
            serial,
            tag,
            key,
            level,
            ..
        } = entry
        else {
            return;
        };
        self.list_serial(*serial, listed);
        let Some(slot) = formatting_slot(*tag) else {
            return;
        };
        let level = &mut self.levels[*level as usize];
        if listed {
            level.tags[slot] += 1;
            if let Some(key) = key {
                *level.keys.entry(*key).or_insert(0) += 1;
            }
        } else {
            level.tags[slot] = level.tags[slot].saturating_sub(1);
            if let Some(key) = key
                && let Some(count) = level.keys.get_mut(key)
            {
                *count = count.saturating_sub(1);
                if *count == 0 {
                    level.keys.remove(key);
                }
            }
        }
    }

    pub(crate) fn push(
        &mut self,
        h: H,
        serial: usize,
        tag: Tag,
        attrs: Vec<Attr>,
        key: Option<u64>,
    ) {
        let keyed = formatting_slot(tag).is_some_and(|slot| self.levels[self.depth].is_keyed(slot));
        let key = keyed.then(|| key.unwrap_or_else(|| self.key(tag, &attrs)));
        let entry = Fmt::El {
            h,
            serial,
            tag,
            attrs,
            key,
            level: self.depth as u32,
        };
        self.account(&entry, true);
        self.entries.push(entry);
    }

    pub(crate) fn push_marker(&mut self) {
        self.entries.push(Fmt::Marker);
        self.depth += 1;
        if self.depth == self.levels.len() {
            self.levels.push(Level::default());
        }
    }

    pub(crate) fn clear_to_marker(&mut self) {
        while let Some(entry) = self.entries.pop() {
            if let Fmt::Marker = entry {
                self.levels[self.depth].reset();
                self.depth = self.depth.saturating_sub(1);
                return;
            }
            self.account(&entry, false);
        }
    }

    pub(crate) fn remove(&mut self, position: usize) -> Fmt<H> {
        let entry = self.entries.remove(position);
        self.account(&entry, false);
        entry
    }

    pub(crate) fn remove_all(&mut self, ascending: &[usize]) {
        let Some(&first) = ascending.first() else {
            return;
        };
        let tail: Vec<Fmt<H>> = self.entries.drain(first..).collect();
        let mut doomed = ascending.iter().peekable();
        for (offset, entry) in tail.into_iter().enumerate() {
            if doomed.next_if(|&&i| i == first + offset).is_some() {
                self.account(&entry, false);
            } else {
                self.entries.push(entry);
            }
        }
    }

    pub(crate) fn insert(&mut self, position: usize, entry: Fmt<H>) {
        self.account(&entry, true);
        self.entries.insert(position, entry);
    }

    pub(crate) fn rehandle(&mut self, position: usize, new_h: H, new_serial: usize) {
        let old = match &mut self.entries[position] {
            Fmt::El { h, serial, .. } => {
                *h = new_h;
                core::mem::replace(serial, new_serial)
            }
            Fmt::Marker => return,
        };
        self.list_serial(old, false);
        self.list_serial(new_serial, true);
    }
}

impl<H> core::ops::Deref for Formatting<H> {
    type Target = [Fmt<H>];

    fn deref(&self) -> &[Fmt<H>] {
        &self.entries
    }
}

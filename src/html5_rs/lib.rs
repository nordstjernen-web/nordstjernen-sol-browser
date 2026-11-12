//! Nordstjernen — an HTML5 parser written from the WHATWG HTML Standard: a tokenizer and a tree builder that build through a TreeSink.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

#![forbid(unsafe_code)]

mod charref;
mod entities;
mod quirks;
mod tags;
mod tokenizer;
mod tree;

use core::ffi::CStr;

pub use tags::{AttrNs, Tag};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Ns {
    Html,
    Svg,
    MathMl,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum QuirksMode {
    NoQuirks,
    LimitedQuirks,
    Quirks,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Position {
    pub line: u32,
    pub column: u32,
}

#[derive(Clone, Copy, Debug)]
pub enum LocalName<'a> {
    Static(&'static CStr),
    Dynamic(&'a [u8]),
}

impl LocalName<'_> {
    pub fn bytes(&self) -> &[u8] {
        match self {
            LocalName::Static(name) => name.to_bytes(),
            LocalName::Dynamic(name) => name,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ElementName<'a> {
    pub ns: Ns,
    pub tag: Tag,
    pub local: LocalName<'a>,
}

#[derive(Clone, Copy, Debug)]
pub struct Attribute<'a> {
    pub name: &'a [u8],
    pub value: &'a [u8],
    pub ns: AttrNs,
    pub prefix_len: usize,
}

impl Attribute<'_> {
    pub fn prefix(&self) -> Option<&[u8]> {
        (self.prefix_len > 0).then(|| &self.name[..self.prefix_len])
    }

    pub fn local_name(&self) -> &[u8] {
        if self.prefix_len > 0 {
            &self.name[self.prefix_len + 1..]
        } else {
            self.name
        }
    }
}

#[derive(Clone)]
pub struct Attrs<'a> {
    inner: core::slice::Iter<'a, tokenizer::Attr>,
}

impl<'a> Attrs<'a> {
    fn new(attrs: &'a [tokenizer::Attr]) -> Self {
        Attrs {
            inner: attrs.iter(),
        }
    }
}

impl<'a> Iterator for Attrs<'a> {
    type Item = Attribute<'a>;

    fn next(&mut self) -> Option<Attribute<'a>> {
        self.inner.next().map(tokenizer::Attr::view)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.inner.size_hint()
    }
}

impl ExactSizeIterator for Attrs<'_> {}

pub trait TreeSink {
    type Handle: Copy + Eq;

    fn document(&self) -> Self::Handle;
    fn create_element(
        &mut self,
        name: ElementName<'_>,
        attrs: Attrs<'_>,
        position: Position,
    ) -> Self::Handle;
    fn create_comment(&mut self, text: &[u8]) -> Self::Handle;
    fn create_processing_instruction(&mut self, target: &[u8], data: &[u8]) -> Self::Handle;
    fn append_doctype(&mut self, name: &[u8], public_id: &[u8], system_id: &[u8]);
    fn append(&mut self, parent: Self::Handle, child: Self::Handle);
    fn insert_before(&mut self, sibling: Self::Handle, child: Self::Handle);
    fn append_text(&mut self, parent: Self::Handle, text: &[u8]);
    fn insert_text_before(&mut self, sibling: Self::Handle, text: &[u8]);
    fn parent(&self, node: Self::Handle) -> Option<Self::Handle>;
    fn remove(&mut self, node: Self::Handle);
    fn reparent_children(&mut self, from: Self::Handle, to: Self::Handle);
    fn template_contents(&mut self, template: Self::Handle) -> Self::Handle;
    fn add_missing_attributes(&mut self, element: Self::Handle, attrs: Attrs<'_>);
    fn discard(&mut self, _node: Self::Handle) {}
}

#[derive(Clone, Copy, Debug)]
pub struct Context<'a> {
    pub ns: Ns,
    pub name: &'a [u8],
}

pub fn named_character_reference(name: &[u8]) -> Option<&'static str> {
    charref::lookup(name)
}

pub fn parse_document<S: TreeSink>(input: &[u8], scripting: bool, sink: &mut S) -> QuirksMode {
    tree::run(input, scripting, None, sink)
}

pub fn parse_fragment<S: TreeSink>(
    context: Context<'_>,
    input: &[u8],
    scripting: bool,
    sink: &mut S,
) {
    tree::run(input, scripting, Some(context), sink);
}

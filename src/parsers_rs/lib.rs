//! Nordstjernen — the C ABI of parsers_rs.h over the in-tree Rust WHATWG URL parser and HTML5 parser.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::slice;

use ns_html5::{
    AttrNs, Attribute, Attrs, Context, ElementName, LocalName, Ns, Position, QuirksMode, TreeSink,
};
use ns_url::bytes;

unsafe extern "C" {
    fn g_malloc(size: usize) -> *mut c_void;

    fn ns_h5_element(
        sink: *mut c_void,
        name: *const c_char,
        len: usize,
        borrowed: c_int,
        line: c_int,
        column: c_int,
    ) -> *mut c_void;
    fn ns_h5_attribute(
        sink: *mut c_void,
        element: *mut c_void,
        name: *const c_char,
        name_len: usize,
        value: *const c_char,
        value_len: usize,
        namespace: c_int,
        prefix_len: usize,
        only_if_missing: c_int,
    );
    fn ns_h5_set_namespace(element: *mut c_void, namespace: c_int);
    fn ns_h5_comment(sink: *mut c_void, text: *const c_char, len: usize) -> *mut c_void;
    fn ns_h5_processing_instruction(
        sink: *mut c_void,
        target: *const c_char,
        target_len: usize,
        data: *const c_char,
        data_len: usize,
    ) -> *mut c_void;
    fn ns_h5_doctype(
        sink: *mut c_void,
        document: *mut c_void,
        name: *const c_char,
        name_len: usize,
        public_id: *const c_char,
        public_len: usize,
        system_id: *const c_char,
        system_len: usize,
    );
    fn ns_h5_append(parent: *mut c_void, child: *mut c_void);
    fn ns_h5_insert_before(sibling: *mut c_void, child: *mut c_void);
    fn ns_h5_append_text(sink: *mut c_void, parent: *mut c_void, text: *const c_char, len: usize);
    fn ns_h5_insert_text_before(
        sink: *mut c_void,
        sibling: *mut c_void,
        text: *const c_char,
        len: usize,
    );
    fn ns_h5_parent(node: *mut c_void) -> *mut c_void;
    fn ns_h5_remove(node: *mut c_void);
    fn ns_h5_reparent_children(from: *mut c_void, to: *mut c_void);
    fn ns_h5_template_contents(template: *mut c_void) -> *mut c_void;
    fn ns_h5_discard(node: *mut c_void);
}

unsafe fn input<'a>(data: *const c_char, len: usize) -> &'a [u8] {
    if data.is_null() || len == 0 {
        return &[];
    }
    unsafe { slice::from_raw_parts(data.cast::<u8>(), len) }
}

fn c_string(bytes: &[u8]) -> *mut c_char {
    let len = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    unsafe {
        let out = g_malloc(len + 1).cast::<u8>();
        ptr::copy_nonoverlapping(bytes.as_ptr(), out, len);
        out.add(len).write(0);
        out.cast::<c_char>()
    }
}

fn c_string_or_null(bytes: Option<Vec<u8>>) -> *mut c_char {
    bytes.map_or(ptr::null_mut(), |b| c_string(&b))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_url_resolve(
    base: *const c_char,
    base_len: usize,
    href: *const c_char,
    href_len: usize,
) -> *mut c_char {
    let (base, href) = unsafe { (input(base, base_len), input(href, href_len)) };
    c_string_or_null(bytes::resolve(Some(base), href))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_url_set_component(
    href: *const c_char,
    href_len: usize,
    component: *const c_char,
    component_len: usize,
    value: *const c_char,
    value_len: usize,
) -> *mut c_char {
    let (href, component, value) = unsafe {
        (
            input(href, href_len),
            input(component, component_len),
            input(value, value_len),
        )
    };
    c_string_or_null(bytes::set_component(href, component, value))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_url_origin(
    url: *const c_char,
    len: usize,
    default_port: c_int,
) -> *mut c_char {
    let url = unsafe { input(url, len) };
    c_string_or_null(if default_port != 0 {
        bytes::origin_from_any(url)
    } else {
        bytes::origin_from(url)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_url_host(url: *const c_char, len: usize) -> *mut c_char {
    c_string_or_null(bytes::host_from(unsafe { input(url, len) }))
}

#[repr(C)]
pub struct UrlParts {
    href: *mut c_char,
    protocol: *mut c_char,
    origin: *mut c_char,
    host: *mut c_char,
    hostname: *mut c_char,
    port: *mut c_char,
    pathname: *mut c_char,
    search: *mut c_char,
    hash: *mut c_char,
    username: *mut c_char,
    password: *mut c_char,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_url_parts(
    url: *const c_char,
    len: usize,
    out: *mut UrlParts,
) -> c_int {
    let Some(parts) = bytes::parts(unsafe { input(url, len) }) else {
        return 0;
    };
    let filled = UrlParts {
        href: c_string(&parts.href),
        protocol: c_string(&parts.protocol),
        origin: c_string(&parts.origin),
        host: c_string(&parts.host),
        hostname: c_string(&parts.hostname),
        port: c_string(&parts.port),
        pathname: c_string(&parts.pathname),
        search: c_string(&parts.search),
        hash: c_string(&parts.hash),
        username: c_string(&parts.username),
        password: c_string(&parts.password),
    };
    unsafe { out.write(filled) };
    1
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Handle(*mut c_void);

struct Sink {
    context: *mut c_void,
    document: Handle,
    positions: bool,
    detached: Vec<*mut c_void>,
}

impl Sink {
    fn new(context: *mut c_void, document: *mut c_void, positions: bool) -> Sink {
        Sink {
            context,
            document: Handle(document),
            positions,
            detached: Vec::new(),
        }
    }

    fn free_detached(&mut self) {
        self.detached.sort_unstable();
        self.detached.dedup();
        self.detached
            .retain(|&node| unsafe { ns_h5_parent(node).is_null() });
        for &root in &self.detached {
            unsafe { ns_h5_discard(root) };
        }
        self.detached.clear();
    }

    fn forget_subtree(&mut self, root: *mut c_void) {
        self.detached.retain(|&node| {
            let mut top = node;
            loop {
                let parent = unsafe { ns_h5_parent(top) };
                if parent.is_null() {
                    break;
                }
                top = parent;
            }
            top != root
        });
    }
}

fn text_ptr(text: &[u8]) -> *const c_char {
    text.as_ptr().cast::<c_char>()
}

fn namespace_id(ns: Ns) -> c_int {
    match ns {
        Ns::Html => 0,
        Ns::Svg => 1,
        Ns::MathMl => 2,
    }
}

fn add_attribute(context: *mut c_void, element: Handle, attr: Attribute<'_>, only_if_missing: bool) {
    let namespace = match attr.ns {
        AttrNs::None => 0,
        AttrNs::XLink => 1,
        AttrNs::Xml => 2,
        AttrNs::Xmlns => 3,
    };
    unsafe {
        ns_h5_attribute(
            context,
            element.0,
            text_ptr(attr.name),
            attr.name.len(),
            text_ptr(attr.value),
            attr.value.len(),
            namespace,
            attr.prefix_len,
            c_int::from(only_if_missing),
        );
    }
}

impl TreeSink for Sink {
    type Handle = Handle;

    fn document(&self) -> Handle {
        self.document
    }

    fn create_element(
        &mut self,
        name: ElementName<'_>,
        attrs: Attrs<'_>,
        position: Position,
    ) -> Handle {
        let (data, len, borrowed) = match name.local {
            LocalName::Static(text) => (text.as_ptr(), text.to_bytes().len(), 1),
            LocalName::Dynamic(text) => (text_ptr(text), text.len(), 0),
        };
        let (line, column) = if self.positions {
            (position.line as c_int, position.column as c_int)
        } else {
            (0, 0)
        };
        let element =
            Handle(unsafe { ns_h5_element(self.context, data, len, borrowed, line, column) });
        for attr in attrs {
            add_attribute(self.context, element, attr, false);
        }
        if name.ns != Ns::Html {
            unsafe { ns_h5_set_namespace(element.0, namespace_id(name.ns)) };
        }
        element
    }

    fn create_comment(&mut self, text: &[u8]) -> Handle {
        Handle(unsafe { ns_h5_comment(self.context, text_ptr(text), text.len()) })
    }

    fn create_processing_instruction(&mut self, target: &[u8], data: &[u8]) -> Handle {
        Handle(unsafe {
            ns_h5_processing_instruction(
                self.context,
                text_ptr(target),
                target.len(),
                text_ptr(data),
                data.len(),
            )
        })
    }

    fn append_doctype(&mut self, name: &[u8], public_id: &[u8], system_id: &[u8]) {
        unsafe {
            ns_h5_doctype(
                self.context,
                self.document.0,
                text_ptr(name),
                name.len(),
                text_ptr(public_id),
                public_id.len(),
                text_ptr(system_id),
                system_id.len(),
            );
        }
    }

    fn append(&mut self, parent: Handle, child: Handle) {
        unsafe { ns_h5_append(parent.0, child.0) };
    }

    fn insert_before(&mut self, sibling: Handle, child: Handle) {
        unsafe { ns_h5_insert_before(sibling.0, child.0) };
    }

    fn append_text(&mut self, parent: Handle, text: &[u8]) {
        unsafe { ns_h5_append_text(self.context, parent.0, text_ptr(text), text.len()) };
    }

    fn insert_text_before(&mut self, sibling: Handle, text: &[u8]) {
        unsafe {
            ns_h5_insert_text_before(self.context, sibling.0, text_ptr(text), text.len());
        }
    }

    fn parent(&self, node: Handle) -> Option<Handle> {
        let parent = unsafe { ns_h5_parent(node.0) };
        (!parent.is_null()).then_some(Handle(parent))
    }

    fn remove(&mut self, node: Handle) {
        unsafe { ns_h5_remove(node.0) };
        self.detached.push(node.0);
    }

    fn reparent_children(&mut self, from: Handle, to: Handle) {
        unsafe { ns_h5_reparent_children(from.0, to.0) };
    }

    fn template_contents(&mut self, template: Handle) -> Handle {
        Handle(unsafe { ns_h5_template_contents(template.0) })
    }

    fn add_missing_attributes(&mut self, element: Handle, attrs: Attrs<'_>) {
        for attr in attrs {
            add_attribute(self.context, element, attr, true);
        }
    }

    fn discard(&mut self, node: Handle) {
        self.forget_subtree(node.0);
        unsafe { ns_h5_discard(node.0) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_html_parse_document(
    data: *const c_char,
    len: usize,
    scripting: c_int,
    document: *mut c_void,
    context: *mut c_void,
) -> c_int {
    let mut sink = Sink::new(context, document, true);
    let bytes = unsafe { input(data, len) };
    let quirks = ns_html5::parse_document(bytes, scripting != 0, &mut sink);
    sink.free_detached();
    match quirks {
        QuirksMode::NoQuirks => 0,
        QuirksMode::LimitedQuirks => 1,
        QuirksMode::Quirks => 2,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_html_parse_fragment(
    context: *const c_char,
    context_len: usize,
    namespace: c_int,
    data: *const c_char,
    len: usize,
    scripting: c_int,
    document: *mut c_void,
    sink_context: *mut c_void,
) {
    let ns = match namespace {
        1 => Ns::Svg,
        2 => Ns::MathMl,
        _ => Ns::Html,
    };
    let (qualified, bytes) = unsafe { (input(context, context_len), input(data, len)) };
    let qualified = if qualified.is_empty() {
        &b"body"[..]
    } else {
        qualified
    };
    let name = match (ns, qualified.iter().rposition(|&c| c == b':')) {
        (Ns::Svg | Ns::MathMl, Some(colon)) => &qualified[colon + 1..],
        _ => qualified,
    };
    let mut sink = Sink::new(sink_context, document, false);
    ns_html5::parse_fragment(Context { ns, name }, bytes, scripting != 0, &mut sink);
    sink.free_detached();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ns_rs_html_named_char_ref(name: *const c_char, len: usize) -> *mut c_char {
    let name = unsafe { input(name, len) };
    ns_html5::named_character_reference(name).map_or(ptr::null_mut(), |v| c_string(v.as_bytes()))
}

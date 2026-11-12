//! Nordstjernen — URL helpers over raw bytes: scalar-value conversion, resolving against a base, the setters, origins, hosts and the URL API's string components.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use std::borrow::Cow;
use std::cell::RefCell;

use crate::{Host, Scheme, Setter, Url};

pub fn is_http_or_https(url: &[u8]) -> bool {
    url.starts_with(b"http://") || url.starts_with(b"https://")
}

fn is_encoded_surrogate(bytes: &[u8]) -> bool {
    matches!(bytes, [0xed, 0xa0..=0xbf, 0x80..=0xbf, ..])
}

fn scalar_values(input: &[u8]) -> Cow<'_, str> {
    if let Ok(text) = core::str::from_utf8(input) {
        return Cow::Borrowed(text);
    }
    let mut out = String::with_capacity(input.len());
    let mut rest = input;
    while !rest.is_empty() {
        match core::str::from_utf8(rest) {
            Ok(text) => {
                out.push_str(text);
                break;
            }
            Err(error) => {
                let (valid, after) = rest.split_at(error.valid_up_to());
                out.push_str(core::str::from_utf8(valid).unwrap_or_default());
                out.push('\u{fffd}');
                let skip = if is_encoded_surrogate(after) {
                    3
                } else {
                    error.error_len().unwrap_or(after.len())
                };
                rest = &after[skip..];
            }
        }
    }
    Cow::Owned(out)
}

fn parse(input: &[u8], base: Option<&Url>) -> Option<Url> {
    Url::parse(&scalar_values(input), base)
}

fn href(url: &Url) -> Option<Vec<u8>> {
    Some(url.href().into_bytes()).filter(|s| !s.is_empty())
}

thread_local! {
    static LAST_BASE: RefCell<Option<(Vec<u8>, Url)>> = const { RefCell::new(None) };
}

fn with_base<T>(base: &[u8], f: impl FnOnce(&Url) -> Option<T>) -> Option<T> {
    let mut f = Some(f);
    let cached = LAST_BASE.try_with(|cell| {
        let mut slot = cell.borrow_mut();
        if !matches!(&*slot, Some((key, _)) if key == base) {
            *slot = Some((base.to_vec(), parse(base, None)?));
        }
        let url = &slot.as_ref()?.1;
        f.take().and_then(|f| f(url))
    });
    match cached {
        Ok(result) => result,
        Err(_) => f.take().and_then(|f| f(&parse(base, None)?)),
    }
}

pub fn resolve(base: Option<&[u8]>, href_input: &[u8]) -> Option<Vec<u8>> {
    match base.filter(|b| !b.is_empty()) {
        Some(base) => with_base(base, |base| href(&parse(href_input, Some(base))?)),
        None if href_input.is_empty() => None,
        None => href(&parse(href_input, None)?),
    }
}

pub fn set_component(href_input: &[u8], component: &[u8], value: &[u8]) -> Option<Vec<u8>> {
    let mut url = parse(href_input, None)?;
    if let Some(setter) = core::str::from_utf8(component)
        .ok()
        .and_then(Setter::by_name)
    {
        url.set(setter, &String::from_utf8_lossy(value));
    }
    href(&url)
}

pub fn to_ascii(url: &[u8]) -> Option<Vec<u8>> {
    if url.is_empty() {
        return None;
    }
    if url.starts_with(b"data:") || url.starts_with(b"about:") || url.starts_with(b"file:") {
        return Some(url.to_vec());
    }
    resolve(None, url)
}

fn non_empty_host(url: &Url) -> Option<&Host> {
    url.host().filter(|h| **h != Host::Empty)
}

fn parse_with_host(url: &[u8]) -> Option<Url> {
    parse(url, None).filter(|u| non_empty_host(u).is_some())
}

fn origin(url: &[u8], default_port: bool) -> Option<Vec<u8>> {
    let u = parse_with_host(url)?;
    let mut out = u.scheme().as_bytes().to_vec();
    out.extend_from_slice(b"://");
    out.extend_from_slice(u.host_string().as_bytes());
    let port = match (u.port(), default_port) {
        (Some(port), _) => Some(port),
        (None, false) => None,
        (None, true) => Some(match u.scheme_kind() {
            Scheme::Http | Scheme::Ws => 80,
            Scheme::Https | Scheme::Wss => 443,
            Scheme::Ftp => 21,
            Scheme::File | Scheme::Other => 0,
        }),
    };
    if let Some(port) = port {
        out.extend_from_slice(format!(":{port}").as_bytes());
    }
    Some(out)
}

fn blob_inner(url: &[u8]) -> Option<&[u8]> {
    (url.len() > 5 && url[..5].eq_ignore_ascii_case(b"blob:")).then(|| &url[5..])
}

pub fn origin_from(url: &[u8]) -> Option<Vec<u8>> {
    let url = blob_inner(url).unwrap_or(url);
    if url.is_empty() || !is_http_or_https(url) {
        return None;
    }
    origin(url, false)
}

pub fn origin_from_any(url: &[u8]) -> Option<Vec<u8>> {
    if url.is_empty() {
        return None;
    }
    origin(url, true)
}

pub fn host_from(url: &[u8]) -> Option<Vec<u8>> {
    let u = parse_with_host(url)?;
    Some(u.host_string().into_bytes()).filter(|h| !h.is_empty())
}

pub fn same_origin(a: &[u8], b: &[u8]) -> bool {
    match (origin_from(a), origin_from(b)) {
        (Some(oa), Some(ob)) => !oa.is_empty() && !ob.is_empty() && oa.eq_ignore_ascii_case(&ob),
        _ => false,
    }
}

pub fn blob_url_reachable(blob_url: &[u8], requester: &[u8]) -> bool {
    if blob_inner(blob_url).is_none() {
        return false;
    }
    match origin_from(blob_url) {
        Some(_) => same_origin(blob_url, requester),
        None => origin_from(requester).is_none(),
    }
}

pub struct Parts {
    pub href: Vec<u8>,
    pub protocol: Vec<u8>,
    pub origin: Vec<u8>,
    pub host: Vec<u8>,
    pub hostname: Vec<u8>,
    pub port: Vec<u8>,
    pub pathname: Vec<u8>,
    pub search: Vec<u8>,
    pub hash: Vec<u8>,
    pub username: Vec<u8>,
    pub password: Vec<u8>,
}

fn prefixed(prefix: u8, value: &[u8]) -> Vec<u8> {
    if value.is_empty() {
        Vec::new()
    } else {
        [&[prefix][..], value].concat()
    }
}

fn parts_at_depth(url: &[u8], depth: u32) -> Option<Parts> {
    let u = parse(url, None)?;
    let href = href(&u).unwrap_or_else(|| url.to_vec());
    let protocol = if u.scheme().is_empty() {
        Vec::new()
    } else {
        [u.scheme().as_bytes(), b":"].concat()
    };
    let hostname = non_empty_host(&u)
        .map(|h| h.serialize().into_bytes())
        .unwrap_or_default();
    let port = u
        .port()
        .map_or_else(Vec::new, |p| p.to_string().into_bytes());
    let host = if !hostname.is_empty() && !port.is_empty() {
        [&hostname[..], b":", &port].concat()
    } else {
        hostname.clone()
    };
    let tuple_origin = !hostname.is_empty()
        && matches!(
            &protocol[..],
            b"http:" | b"https:" | b"ws:" | b"wss:" | b"ftp:"
        );
    let mut origin = if tuple_origin {
        [&protocol[..], b"//", &host].concat()
    } else {
        b"null".to_vec()
    };
    let pathname = u.path_string().into_bytes();
    if depth == 0
        && protocol == b"blob:"
        && !pathname.is_empty()
        && let Some(inner) = parts_at_depth(&pathname, depth + 1)
        && (inner.protocol == b"http:" || inner.protocol == b"https:")
    {
        origin = inner.origin;
    }
    Some(Parts {
        href,
        protocol,
        origin,
        host,
        hostname,
        port,
        pathname,
        search: prefixed(b'?', u.query().unwrap_or_default().as_bytes()),
        hash: prefixed(b'#', u.fragment().unwrap_or_default().as_bytes()),
        username: u.username().as_bytes().to_vec(),
        password: u.password().as_bytes().to_vec(),
    })
}

pub fn parts(url: &[u8]) -> Option<Parts> {
    parts_at_depth(url, 0)
}

pub fn is_valid_absolute(url: &[u8]) -> bool {
    !url.is_empty()
        && !url
            .iter()
            .any(|&c| matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0c))
        && parts(url).is_some_and(|p| !p.protocol.is_empty())
}

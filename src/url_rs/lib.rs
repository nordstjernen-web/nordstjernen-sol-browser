//! Nordstjernen — the WHATWG URL Standard: a parsed URL, its components, its serialization and the URL API setters.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::borrow::Cow;

pub mod bytes;
mod host;
mod idna;
mod nfc;
mod parser;
mod percent;
mod punycode;
mod setters;
mod tables;

pub use host::Host;
pub use idna::domain_to_ascii;
pub use nfc::{combining_class, nfd};
pub use setters::Setter;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Scheme {
    Http,
    Https,
    Ws,
    Wss,
    Ftp,
    File,
    Other,
}

impl Scheme {
    fn name(self) -> Option<&'static str> {
        Some(match self {
            Scheme::Http => "http",
            Scheme::Https => "https",
            Scheme::Ws => "ws",
            Scheme::Wss => "wss",
            Scheme::Ftp => "ftp",
            Scheme::File => "file",
            Scheme::Other => return None,
        })
    }

    fn of(name: &str) -> Scheme {
        match name {
            "http" => Scheme::Http,
            "https" => Scheme::Https,
            "ws" => Scheme::Ws,
            "wss" => Scheme::Wss,
            "ftp" => Scheme::Ftp,
            "file" => Scheme::File,
            _ => Scheme::Other,
        }
    }

    pub fn is_special(self) -> bool {
        self != Scheme::Other
    }

    pub fn default_port(self) -> Option<u16> {
        match self {
            Scheme::Http | Scheme::Ws => Some(80),
            Scheme::Https | Scheme::Wss => Some(443),
            Scheme::Ftp => Some(21),
            Scheme::File | Scheme::Other => None,
        }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
enum Path {
    Opaque(String),
    List(String),
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Url {
    scheme: Cow<'static, str>,
    kind: Scheme,
    username: String,
    password: String,
    host: Option<Host>,
    port: Option<u16>,
    path: Path,
    query: Option<String>,
    fragment: Option<String>,
}

impl Url {
    fn empty() -> Url {
        Url {
            scheme: Cow::Borrowed(""),
            kind: Scheme::Other,
            username: String::new(),
            password: String::new(),
            host: None,
            port: None,
            path: Path::List(String::new()),
            query: None,
            fragment: None,
        }
    }

    pub fn parse(input: &str, base: Option<&Url>) -> Option<Url> {
        parser::parse(input, base)
    }

    pub fn scheme(&self) -> &str {
        &self.scheme
    }

    pub fn scheme_kind(&self) -> Scheme {
        self.kind
    }

    pub fn is_special(&self) -> bool {
        self.kind.is_special()
    }

    pub fn username(&self) -> &str {
        &self.username
    }

    pub fn password(&self) -> &str {
        &self.password
    }

    pub fn host(&self) -> Option<&Host> {
        self.host.as_ref()
    }

    pub fn host_string(&self) -> String {
        self.host.as_ref().map(Host::serialize).unwrap_or_default()
    }

    pub fn port(&self) -> Option<u16> {
        self.port
    }

    pub fn query(&self) -> Option<&str> {
        self.query.as_deref()
    }

    pub fn fragment(&self) -> Option<&str> {
        self.fragment.as_deref()
    }

    pub fn has_opaque_path(&self) -> bool {
        matches!(self.path, Path::Opaque(_))
    }

    fn includes_credentials(&self) -> bool {
        !self.username.is_empty() || !self.password.is_empty()
    }

    fn cannot_have_credentials_or_port(&self) -> bool {
        matches!(self.host, None | Some(Host::Empty)) || self.kind == Scheme::File
    }

    fn set_scheme(&mut self, scheme: &str) {
        self.kind = Scheme::of(scheme);
        self.scheme = match self.kind.name() {
            Some(name) => Cow::Borrowed(name),
            None => Cow::Owned(scheme.to_owned()),
        };
    }

    fn copy_scheme(&mut self, from: &Url) {
        self.kind = from.kind;
        self.scheme = from.scheme.clone();
    }

    fn write_path(&self, out: &mut String) {
        match &self.path {
            Path::Opaque(path) | Path::List(path) => out.push_str(path),
        }
    }

    pub fn path_string(&self) -> String {
        let mut out = String::new();
        self.write_path(&mut out);
        out
    }

    pub fn serialize(&self, exclude_fragment: bool) -> String {
        let mut out = String::with_capacity(self.scheme.len() + 32);
        out.push_str(&self.scheme);
        out.push(':');
        match &self.host {
            Some(host) => {
                out.push_str("//");
                if self.includes_credentials() {
                    out.push_str(&self.username);
                    if !self.password.is_empty() {
                        out.push(':');
                        out.push_str(&self.password);
                    }
                    out.push('@');
                }
                host.write(&mut out);
                if let Some(port) = self.port {
                    out.push(':');
                    out.push_str(&port.to_string());
                }
            }
            None => {
                if let Path::List(path) = &self.path
                    && path.starts_with("//")
                {
                    out.push_str("/.");
                }
            }
        }
        self.write_path(&mut out);
        if let Some(query) = &self.query {
            out.push('?');
            out.push_str(query);
        }
        if !exclude_fragment && let Some(fragment) = &self.fragment {
            out.push('#');
            out.push_str(fragment);
        }
        out
    }

    pub fn href(&self) -> String {
        self.serialize(false)
    }
}

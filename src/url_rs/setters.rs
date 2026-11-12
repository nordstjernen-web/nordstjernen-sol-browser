//! Nordstjernen — the URL API setters: protocol, username, password, host, hostname, port, pathname, search and hash.
//! Copyright 2026 Andreas Røsdal
//! SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later

use crate::parser::{State, parse_with_override};
use crate::percent::{EncodeSet, encode};
use crate::{Path, Url};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Setter {
    Protocol,
    Username,
    Password,
    Host,
    Hostname,
    Port,
    Pathname,
    Search,
    Hash,
}

impl Setter {
    pub fn by_name(name: &str) -> Option<Setter> {
        Some(match name {
            "protocol" => Setter::Protocol,
            "username" => Setter::Username,
            "password" => Setter::Password,
            "host" => Setter::Host,
            "hostname" => Setter::Hostname,
            "port" => Setter::Port,
            "pathname" => Setter::Pathname,
            "search" => Setter::Search,
            "hash" => Setter::Hash,
            _ => return None,
        })
    }
}

impl Url {
    pub fn set(&mut self, setter: Setter, value: &str) {
        match setter {
            Setter::Protocol => {
                parse_with_override(self, &format!("{value}:"), State::SchemeStart);
            }
            Setter::Username | Setter::Password => {
                if self.cannot_have_credentials_or_port() {
                    return;
                }
                let encoded = encode(value, EncodeSet::Userinfo);
                if setter == Setter::Username {
                    self.username = encoded;
                } else {
                    self.password = encoded;
                }
            }
            Setter::Host | Setter::Hostname => {
                if self.has_opaque_path() {
                    return;
                }
                let state = if setter == Setter::Host {
                    State::Host
                } else {
                    State::Hostname
                };
                parse_with_override(self, value, state);
            }
            Setter::Port => {
                if self.cannot_have_credentials_or_port() {
                    return;
                }
                if value.is_empty() {
                    self.port = None;
                } else {
                    parse_with_override(self, value, State::Port);
                }
            }
            Setter::Pathname => {
                if self.has_opaque_path() {
                    return;
                }
                self.path = Path::List(String::new());
                parse_with_override(self, value, State::PathStart);
            }
            Setter::Search => {
                if value.is_empty() {
                    self.query = None;
                    return;
                }
                self.query = Some(String::new());
                parse_with_override(self, value.strip_prefix('?').unwrap_or(value), State::Query);
            }
            Setter::Hash => {
                if value.is_empty() {
                    self.fragment = None;
                    return;
                }
                self.fragment = Some(String::new());
                parse_with_override(
                    self,
                    value.strip_prefix('#').unwrap_or(value),
                    State::Fragment,
                );
            }
        }
    }
}

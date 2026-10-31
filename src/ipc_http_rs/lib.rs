// Nordstjernen — renderer IPC HTTP/1.1 + JSON framing in memory-safe Rust (C ABI of ipc_http.h).

use std::ffi::{c_char, c_int, c_long, c_void, CStr};
use std::io;
use std::ptr;
use std::slice;

const CONN_BUF: usize = 16384;
const LINE_CAP: usize = 20480;
const MAX_HEADERS: usize = 64;
const MAX_BODY: c_long = 64 * 1024 * 1024;
const REQUEST_HEAD_MAX: usize = 512;

#[repr(C)]
pub struct HttpConn {
    fd: c_int,
    buf: [u8; CONN_BUF],
    start: usize,
    len: usize,
}

#[repr(C)]
pub struct HttpHead {
    method: [u8; 8],
    path: [u8; 128],
    status: c_int,
    content_length: c_long,
    x_w: c_long,
    x_h: c_long,
    x_stride: c_long,
    x_anim: c_long,
    x_unchanged: c_long,
    x_render_rc: c_long,
    x_page_w: c_long,
    x_page_h: c_long,
    x_scroll_y: c_long,
    x_scroll_x: c_long,
    x_clipboard: c_long,
    x_tiles: c_long,
    x_wake: c_long,
    x_nav: [u8; 2048],
    x_webgl: [u8; 2048],
    x_camera: [u8; 2048],
    x_download: [u8; 3072],
    x_config: [u8; 12288],
    x_audio: [u8; 16384],
    x_window_action: [u8; 32],
}

extern "C" {
    fn malloc(n: usize) -> *mut c_void;
}

#[cfg(not(windows))]
extern "C" {
    fn read(fd: c_int, buf: *mut c_void, n: usize) -> isize;
    fn write(fd: c_int, buf: *const c_void, n: usize) -> isize;
}

#[cfg(windows)]
extern "C" {
    fn _read(fd: c_int, buf: *mut c_void, n: u32) -> c_int;
    fn _write(fd: c_int, buf: *const c_void, n: u32) -> c_int;
}

#[cfg(not(windows))]
fn fd_read(fd: c_int, buf: &mut [u8]) -> isize {
    unsafe { read(fd, buf.as_mut_ptr().cast(), buf.len()) }
}

#[cfg(not(windows))]
fn fd_write(fd: c_int, buf: &[u8]) -> isize {
    unsafe { write(fd, buf.as_ptr().cast(), buf.len()) }
}

#[cfg(windows)]
fn fd_read(fd: c_int, buf: &mut [u8]) -> isize {
    let n = buf.len().min(i32::MAX as usize) as u32;
    unsafe { _read(fd, buf.as_mut_ptr().cast(), n) as isize }
}

#[cfg(windows)]
fn fd_write(fd: c_int, buf: &[u8]) -> isize {
    let n = buf.len().min(i32::MAX as usize) as u32;
    unsafe { _write(fd, buf.as_ptr().cast(), n) as isize }
}

fn interrupted() -> bool {
    io::Error::last_os_error().kind() == io::ErrorKind::Interrupted
}

impl HttpConn {
    fn fill(&mut self) -> bool {
        if self.len > 0 {
            return true;
        }
        self.start = 0;
        loop {
            let r = fd_read(self.fd, &mut self.buf);
            if r > 0 {
                self.len = (r as usize).min(CONN_BUF);
                return true;
            }
            if r < 0 && interrupted() {
                continue;
            }
            return false;
        }
    }

    fn next_byte(&mut self) -> Option<u8> {
        if self.len == 0 && !self.fill() {
            return None;
        }
        let ch = self.buf[self.start];
        self.start += 1;
        self.len -= 1;
        Some(ch)
    }

    fn read_line(&mut self, out: &mut Vec<u8>) -> bool {
        out.clear();
        loop {
            let ch = match self.next_byte() {
                Some(ch) => ch,
                None => return false,
            };
            if ch == b'\n' {
                while out.last() == Some(&b'\r') {
                    out.pop();
                }
                return true;
            }
            if out.len() + 1 < LINE_CAP {
                out.push(ch);
            }
        }
    }

    fn consume(&mut self, n: usize, mut dst: Option<&mut [u8]>) -> bool {
        let mut got = 0usize;
        while got < n {
            if self.len == 0 && !self.fill() {
                return false;
            }
            let take = self.len.min(n - got);
            if let Some(d) = dst.as_deref_mut() {
                d[got..got + take].copy_from_slice(&self.buf[self.start..self.start + take]);
            }
            self.start += take;
            self.len -= take;
            got += take;
        }
        true
    }
}

fn c_string_prefix(bytes: &[u8]) -> &[u8] {
    match bytes.iter().position(|&b| b == 0) {
        Some(n) => &bytes[..n],
        None => bytes,
    }
}

fn parse_long(s: &[u8]) -> c_long {
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) {
        i += 1;
    }
    let negative = i < s.len() && s[i] == b'-';
    if i < s.len() && (s[i] == b'-' || s[i] == b'+') {
        i += 1;
    }
    let mut v: c_long = 0;
    while i < s.len() && s[i].is_ascii_digit() {
        let d = (s[i] - b'0') as c_long;
        v = if negative {
            v.saturating_mul(10).saturating_sub(d)
        } else {
            v.saturating_mul(10).saturating_add(d)
        };
        i += 1;
    }
    v
}

fn copy_field(dst: &mut [u8], src: &[u8]) {
    let n = src.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
}

fn head_defaults(h: &mut HttpHead) {
    unsafe { ptr::write_bytes(h as *mut HttpHead, 0, 1) };
    h.x_w = -1;
    h.x_h = -1;
    h.x_stride = -1;
    h.x_anim = -1;
    h.x_page_w = -1;
    h.x_page_h = -1;
    h.x_scroll_y = -1;
    h.x_scroll_x = -1;
}

fn apply_header(h: &mut HttpHead, name: &[u8], val: &[u8]) {
    let is = |n: &str| name.eq_ignore_ascii_case(n.as_bytes());
    let number = || parse_long(val);
    if is("Content-Length") {
        h.content_length = number();
    } else if is("X-W") {
        h.x_w = number();
    } else if is("X-H") {
        h.x_h = number();
    } else if is("X-Stride") {
        h.x_stride = number();
    } else if is("X-Anim") {
        h.x_anim = number();
    } else if is("X-PageW") {
        h.x_page_w = number();
    } else if is("X-PageH") {
        h.x_page_h = number();
    } else if is("X-ScrollY") {
        h.x_scroll_y = number();
    } else if is("X-ScrollX") {
        h.x_scroll_x = number();
    } else if is("X-Unchanged") {
        h.x_unchanged = number();
    } else if is("X-Render-RC") {
        h.x_render_rc = number();
    } else if is("X-Clipboard") {
        h.x_clipboard = number();
    } else if is("X-Tiles") {
        h.x_tiles = number();
    } else if is("X-Wake") {
        h.x_wake = number();
    } else if is("X-Nav") {
        copy_field(&mut h.x_nav, val);
    } else if is("X-WebGL") {
        copy_field(&mut h.x_webgl, val);
    } else if is("X-Camera") {
        copy_field(&mut h.x_camera, val);
    } else if is("X-Download") {
        copy_field(&mut h.x_download, val);
    } else if is("X-Config") {
        copy_field(&mut h.x_config, val);
    } else if is("X-Audio") {
        copy_field(&mut h.x_audio, val);
    } else if is("X-Window-Action") {
        copy_field(&mut h.x_window_action, val);
    }
}

fn read_head(c: &mut HttpConn, h: &mut HttpHead) -> bool {
    head_defaults(h);
    let mut raw = Vec::with_capacity(256);
    if !c.read_line(&mut raw) || raw.is_empty() {
        return false;
    }
    let line = c_string_prefix(&raw);
    if line.is_empty() {
        return false;
    }
    if line.starts_with(b"HTTP/") {
        h.status = match line.iter().position(|&b| b == b' ') {
            Some(sp) => parse_long(&line[sp + 1..]) as c_int,
            None => 0,
        };
    } else {
        let sp = match line.iter().position(|&b| b == b' ') {
            Some(sp) => sp,
            None => return false,
        };
        copy_field(&mut h.method, &line[..sp]);
        let rest = &line[sp + 1..];
        let path_end = rest.iter().position(|&b| b == b' ').unwrap_or(rest.len());
        copy_field(&mut h.path, &rest[..path_end]);
    }

    let mut headers = 0usize;
    loop {
        if !c.read_line(&mut raw) {
            return false;
        }
        if raw.is_empty() {
            break;
        }
        headers += 1;
        if headers > MAX_HEADERS {
            return false;
        }
        let line = c_string_prefix(&raw);
        let colon = match line.iter().position(|&b| b == b':') {
            Some(colon) => colon,
            None => continue,
        };
        let mut val = &line[colon + 1..];
        while val.first() == Some(&b' ') {
            val = &val[1..];
        }
        apply_header(h, &line[..colon], val);
    }
    h.content_length >= 0 && h.content_length <= MAX_BODY
}

fn write_all(fd: c_int, mut buf: &[u8]) -> bool {
    while !buf.is_empty() {
        let w = fd_write(fd, buf);
        if w > 0 {
            buf = &buf[(w as usize).min(buf.len())..];
            continue;
        }
        if w < 0 && interrupted() {
            continue;
        }
        return false;
    }
    true
}

unsafe fn c_bytes<'a>(p: *const c_char) -> Option<&'a [u8]> {
    if p.is_null() {
        None
    } else {
        Some(CStr::from_ptr(p).to_bytes())
    }
}

unsafe fn body_slice<'a>(body: *const c_void, n: usize) -> Option<&'a [u8]> {
    if n == 0 {
        Some(&[])
    } else if body.is_null() {
        None
    } else {
        Some(slice::from_raw_parts(body.cast::<u8>(), n))
    }
}

fn malloc_c_string(bytes: &[u8]) -> *mut c_char {
    let out = unsafe { malloc(bytes.len() + 1) }.cast::<u8>();
    if out.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
        *out.add(bytes.len()) = 0;
    }
    out.cast()
}

fn json_skip_string(s: &[u8], mut p: usize) -> usize {
    p += 1;
    while p < s.len() && s[p] != b'"' {
        if s[p] == b'\\' && p + 1 < s.len() {
            p += 1;
        }
        p += 1;
    }
    if p < s.len() {
        p + 1
    } else {
        p
    }
}

fn json_skip_space(s: &[u8], mut p: usize) -> usize {
    while p < s.len() && matches!(s[p], b' ' | b'\t' | b'\n' | b'\r') {
        p += 1;
    }
    p
}

fn json_find_value(body: &[u8], key: &[u8]) -> Option<usize> {
    let mut p = 0usize;
    while let Some(off) = body[p..].iter().position(|&b| b == b'"') {
        let quote = p + off;
        let name = quote + 1;
        let end = json_skip_string(body, quote);
        let after = json_skip_space(body, end);
        if after < body.len()
            && body[after] == b':'
            && end > name
            && end - 1 - name == key.len()
            && &body[name..name + key.len()] == key
        {
            return Some(json_skip_space(body, after + 1));
        }
        p = end;
        if p >= body.len() {
            break;
        }
    }
    None
}

fn json_hex4(s: &[u8], at: usize) -> Option<u32> {
    let digits = s.get(at..at + 4)?;
    let mut v = 0u32;
    for &d in digits {
        v = (v << 4) | (d as char).to_digit(16)?;
    }
    Some(v)
}

fn json_push_code_point(out: &mut Vec<u8>, cp: u32) {
    let ch = char::from_u32(cp).unwrap_or(char::REPLACEMENT_CHARACTER);
    let mut tmp = [0u8; 4];
    out.extend_from_slice(ch.encode_utf8(&mut tmp).as_bytes());
}

fn json_unescape_string(s: &[u8], mut v: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len().saturating_sub(v));
    while v < s.len() && s[v] != b'"' {
        if s[v] == b'\\' && v + 1 < s.len() {
            v += 1;
            match s[v] {
                b'n' => out.push(b'\n'),
                b'r' => out.push(b'\r'),
                b't' => out.push(b'\t'),
                b'u' => {
                    if let Some(hi) = json_hex4(s, v + 1) {
                        v += 4;
                        let mut cp = hi;
                        if (0xD800..=0xDBFF).contains(&cp)
                            && s.get(v + 1) == Some(&b'\\')
                            && s.get(v + 2) == Some(&b'u')
                        {
                            if let Some(lo) = json_hex4(s, v + 3) {
                                if (0xDC00..=0xDFFF).contains(&lo) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    v += 6;
                                }
                            }
                        }
                        json_push_code_point(&mut out, cp);
                    }
                }
                other => out.push(other),
            }
            v += 1;
        } else {
            out.push(s[v]);
            v += 1;
        }
    }
    out
}

fn json_parse_double(s: &[u8]) -> f64 {
    let mut i = 0usize;
    let at = |i: usize| s.get(i).copied().unwrap_or(0);
    let mut sign = 1.0;
    if at(i) == b'-' {
        sign = -1.0;
        i += 1;
    } else if at(i) == b'+' {
        i += 1;
    }
    let mut val = 0.0f64;
    while at(i).is_ascii_digit() {
        val = val * 10.0 + (at(i) - b'0') as f64;
        i += 1;
    }
    if at(i) == b'.' {
        i += 1;
        let mut frac = 0.1;
        while at(i).is_ascii_digit() {
            val += (at(i) - b'0') as f64 * frac;
            frac *= 0.1;
            i += 1;
        }
    }
    if at(i) == b'e' || at(i) == b'E' {
        i += 1;
        let mut esign = 1;
        if at(i) == b'-' {
            esign = -1;
            i += 1;
        } else if at(i) == b'+' {
            i += 1;
        }
        let mut e = 0i32;
        while at(i).is_ascii_digit() {
            if e < 1000 {
                e = e * 10 + (at(i) - b'0') as i32;
            }
            i += 1;
        }
        let p = 10f64.powi(e.min(308));
        val = if esign > 0 { val * p } else { val / p };
    }
    sign * val
}

fn sizes_match(conn_size: usize, head_size: usize) -> bool {
    conn_size == std::mem::size_of::<HttpConn>() && head_size == std::mem::size_of::<HttpHead>()
}

#[no_mangle]
pub unsafe extern "C" fn ns_ipc_rs_conn_init(c: *mut HttpConn, fd: c_int, conn_size: usize) -> c_int {
    if c.is_null() || conn_size != std::mem::size_of::<HttpConn>() {
        return -1;
    }
    let c = &mut *c;
    c.fd = fd;
    c.start = 0;
    c.len = 0;
    0
}

#[no_mangle]
pub unsafe extern "C" fn ns_ipc_rs_read_head(
    c: *mut HttpConn,
    out: *mut HttpHead,
    conn_size: usize,
    head_size: usize,
) -> c_int {
    if c.is_null() || out.is_null() || !sizes_match(conn_size, head_size) {
        return -1;
    }
    if read_head(&mut *c, &mut *out) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn ns_ipc_rs_read_body(
    c: *mut HttpConn,
    n: c_long,
    dst: *mut c_void,
    conn_size: usize,
) -> c_int {
    if c.is_null() || n < 0 || conn_size != std::mem::size_of::<HttpConn>() {
        return -1;
    }
    let n = n as usize;
    if n > 0 && dst.is_null() {
        return -1;
    }
    let dst = if n > 0 {
        Some(slice::from_raw_parts_mut(dst.cast::<u8>(), n))
    } else {
        None
    };
    if (*c).consume(n, dst) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn ns_ipc_rs_skip_body(c: *mut HttpConn, n: c_long, conn_size: usize) -> c_int {
    if c.is_null() || n < 0 || conn_size != std::mem::size_of::<HttpConn>() {
        return -1;
    }
    if (*c).consume(n as usize, None) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn http_write_request(
    fd: c_int,
    method: *const c_char,
    path: *const c_char,
    content_type: *const c_char,
    body: *const c_void,
    n: usize,
) -> c_int {
    let body = match body_slice(body, n) {
        Some(b) => b,
        None => return -1,
    };
    let mut head = Vec::with_capacity(128);
    head.extend_from_slice(c_bytes(method).unwrap_or(b""));
    head.push(b' ');
    head.extend_from_slice(c_bytes(path).unwrap_or(b""));
    head.extend_from_slice(b" HTTP/1.1\r\nContent-Type: ");
    head.extend_from_slice(c_bytes(content_type).unwrap_or(b"text/plain"));
    head.extend_from_slice(format!("\r\nContent-Length: {}\r\n\r\n", n).as_bytes());
    if head.len() >= REQUEST_HEAD_MAX {
        return -1;
    }
    if write_all(fd, &head) && write_all(fd, body) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn http_write_response(
    fd: c_int,
    status: c_int,
    content_type: *const c_char,
    extra_headers: *const c_char,
    body: *const c_void,
    n: usize,
) -> c_int {
    let body = match body_slice(body, n) {
        Some(b) => b,
        None => return -1,
    };
    let mut head = Vec::with_capacity(256);
    head.extend_from_slice(format!("HTTP/1.1 {} OK\r\nContent-Type: ", status).as_bytes());
    head.extend_from_slice(c_bytes(content_type).unwrap_or(b"text/plain"));
    head.extend_from_slice(b"\r\n");
    head.extend_from_slice(c_bytes(extra_headers).unwrap_or(b""));
    head.extend_from_slice(format!("Content-Length: {}\r\n\r\n", n).as_bytes());
    if write_all(fd, &head) && write_all(fd, body) {
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn json_escape(s: *const c_char) -> *mut c_char {
    let s = c_bytes(s).unwrap_or(b"");
    let mut out = Vec::with_capacity(s.len() + 16);
    for &ch in s {
        match ch {
            b'"' | b'\\' => {
                out.push(b'\\');
                out.push(ch);
            }
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            c if c < 0x20 => out.extend_from_slice(format!("\\u{:04x}", c).as_bytes()),
            c => out.push(c),
        }
    }
    malloc_c_string(&out)
}

#[no_mangle]
pub unsafe extern "C" fn json_get_long(body: *const c_char, key: *const c_char, out: *mut c_long) -> c_int {
    let (Some(body), Some(key)) = (c_bytes(body), c_bytes(key)) else {
        return -1;
    };
    match json_find_value(body, key) {
        Some(v) if !out.is_null() => {
            *out = parse_long(&body[v..]);
            0
        }
        _ => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn json_get_double(body: *const c_char, key: *const c_char, out: *mut f64) -> c_int {
    let (Some(body), Some(key)) = (c_bytes(body), c_bytes(key)) else {
        return -1;
    };
    match json_find_value(body, key) {
        Some(v) if !out.is_null() => {
            *out = json_parse_double(&body[v..]);
            0
        }
        _ => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn json_get_str(body: *const c_char, key: *const c_char) -> *mut c_char {
    let (Some(body), Some(key)) = (c_bytes(body), c_bytes(key)) else {
        return ptr::null_mut();
    };
    match json_find_value(body, key) {
        Some(v) if v < body.len() && body[v] == b'"' => malloc_c_string(&json_unescape_string(body, v + 1)),
        _ => ptr::null_mut(),
    }
}

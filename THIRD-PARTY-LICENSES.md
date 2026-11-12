# Third-party software notices

Nordstjernen links to (and in some cases statically includes) the
following open-source libraries. One of them, web-text-lib, is a **modified**
LGPL library; its source and our relink offer are described in its entry
below. Their copyright notices and license
texts are reproduced below. For libraries shipped dynamically in the
release bundles, you are entitled by the LGPL terms to replace them
with modified versions; the binary will continue to function with any
ABI-compatible replacement.

The Nordstjernen source code itself is dual-licensed under the
Nordstjernen Source License v1.0 or the GNU General Public License
version 3 or later, at your option. See `License.md` and `COPYING` for the
project's own license terms.

---

## Statically linked

### Wuffs — Apache License 2.0

> Memory-safe PNG / GIF / BMP / JPEG decoders, transpiled from the
> Wuffs language to C. Vendored as the single-file release in
> `subprojects/wuffs/wuffs-v0.4.c`.
> <https://github.com/google/wuffs-mirror-release-c>
>
> Copyright (c) 2017 The Wuffs Authors.

Licensed under the Apache License, Version 2.0 (the "License"); you may
not use this software except in compliance with the License. You may
obtain a copy of the License at:

  <http://www.apache.org/licenses/LICENSE-2.0>

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied. See the License for the specific language governing permissions
and limitations under the License.

### quickjs-ng — MIT License

> JavaScript engine.
> <https://github.com/quickjs-ng/quickjs>
>
> Copyright (c) 2017-2026 Fabrice Bellard
> Copyright (c) 2017-2026 Charlie Gordon
> Copyright (c) 2023-2026 the quickjs-ng contributors

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

### Rust standard library — MIT OR Apache-2.0

> Runtime and core library linked into the in-tree Rust HTML5 and URL
> parsers (`src/html5_rs/`, `src/url_rs/`).
> <https://www.rust-lang.org>
>
> Copyright (c) The Rust Project Developers

Licensed under either the MIT License or the Apache License, Version
2.0, at your option. See the quickjs-ng section above for the MIT license
text and the Wuffs section above for the Apache License notice.

### pl_mpeg — MIT License

> Single-file MPEG-1 video and MP2 audio decoder and MPEG-PS demuxer.
> Vendored at `subprojects/plmpeg/pl_mpeg.h`.
> <https://github.com/phoboslab/pl_mpeg>
>
> Copyright (c) 2019 Dominic Szablewski

Licensed under the MIT License. See the quickjs-ng section above for the
license text (same license).

### minimp3 — CC0 1.0 Universal (public domain dedication)

> Single-file MP3 decoder, used by the `nordstjernen-audio` helper.
> Vendored at `src/audio/minimp3.h`.
> <https://github.com/lieff/minimp3>

To the extent possible under law, the authors have dedicated all
copyright and related and neighboring rights to this software to the
public domain worldwide. This software is distributed without any
warranty. See <http://creativecommons.org/publicdomain/zero/1.0/>.

### WebAssembly Micro Runtime (WAMR) — Apache License 2.0 with LLVM exceptions

> WebAssembly runtime. Vendored in `src/wamr/`.
> <https://github.com/bytecodealliance/wasm-micro-runtime>
>
> Copyright (c) The WebAssembly Micro Runtime contributors.

Licensed under the Apache License, Version 2.0, with LLVM exceptions.
See the Wuffs section above for the base Apache 2.0 text; the full
license including the LLVM exceptions is reproduced in
`src/wamr/LICENSE`.

---

### PDF.js — Apache License 2.0

> PDF viewer library. Not in the source tree: the pinned `pdfjs-dist`
> release is fetched by `subprojects/pdfjs-dist.wrap` at build time and
> its `pdf.min.mjs`, `pdf.worker.min.mjs` and WebAssembly image decoders
> are embedded in the engine binary (`-Dpdfjs`).
> <https://github.com/mozilla/pdf.js>
>
> Copyright Mozilla Foundation.

Licensed under the Apache License, Version 2.0; see the Wuffs section
above for the text. The embedded WebAssembly decoders carry their own
licences, shipped in the package's `wasm/` directory: OpenJPEG
(BSD 2-Clause), the JBIG2 decoder from PDFium (BSD 3-Clause) and qcms
(MIT).

---

### ICU4X calendar data and CLDR time zone names — Unicode License v3

> Chinese, Korean and Umm al-Qura year tables
> (`src/quickjs/quickjs-temporal-caldata.h`, packed from ICU4X by
> `scripts/gen-temporal-calendar-data.py`) and the Windows-to-IANA time
> zone table (`src/quickjs/quickjs-temporal-winzones.h`, from CLDR
> `windowsZones.xml` by `scripts/gen-temporal-windows-zones.py`).
> <https://github.com/unicode-org/icu4x>, <https://github.com/unicode-org/cldr>
>
> Copyright © 2016-2025 Unicode, Inc.

Licensed under the Unicode License v3,
<https://www.unicode.org/license.txt>. Permission is granted to use, copy,
modify and distribute the data provided the copyright and permission
notice appear with all copies.

---

### web-text-lib — GNU LGPL 2.1 or later (modified Pango, statically linked)

> Text itemization, shaping and line breaking, with a cache of shaped runs
> that Pango itself does not keep. Source in this repository under
> `src/web-text-lib/`; it originated as ns-pango
> (<https://github.com/nordstjernen-web/ns-pango>).
>
> Copyright the GNU Project and contributors, and Northstar and Nordstjernen
> contributors for the modifications.

**This is a modified copy of Pango.** It adds a process-wide cache of
shaped glyph strings and of context font metrics, renames every exported
symbol so it can share a process with the system Pango that GTK loads, and
removes the backends and tooling a browser does not use. It remains
licensed under the GNU Lesser General Public License version 2.1 or, at
your option, any later version, and its complete corresponding source --
including every modification -- is part of the Nordstjernen source tree and
source tarball, under `src/web-text-lib/`. Upstream Pango's license file
is kept there as `src/web-text-lib/COPYING`; the LGPL 2.1 text is at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Unlike the other LGPL libraries listed here, web-text-lib is linked
**statically** into the Nordstjernen executables on desktop platforms. Per
LGPL section 6(a), you are entitled to modify web-text-lib and relink
Nordstjernen against your modified copy: write to the address in
`README.md` and we will supply the Nordstjernen object files, together with
any data and utility programs needed, so that you can produce a modified
executable. Because the complete Nordstjernen source is published as well,
you can also rebuild the browser from source with your modified
`src/web-text-lib/`. Android builds do not include web-text-lib;
they link the system Pango dynamically as before.

## Dynamically linked

### libcurl — curl license (MIT-like)

> HTTP/TLS client.
> <https://curl.se>
>
> Copyright (c) 1996-2026 Daniel Stenberg, and many contributors.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE
FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

### OpenSSL (libcrypto) — Apache License 2.0

> Web Cryptography (`crypto.subtle`) primitives and the TLS backend used
> transitively by libcurl.
> <https://www.openssl.org>
>
> Copyright (c) 1998-2026 The OpenSSL Project Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0. See the Wuffs section
above for the license text (same license).

### libwebp — BSD 3-Clause License

> WebP image decoding.
> <https://chromium.googlesource.com/webm/libwebp>
>
> Copyright (c) 2010, Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met: redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer;
redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution;
neither the name of Google nor the names of its contributors may be used
to endorse or promote products derived from this software without
specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE
COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE.

### libpsl — MIT License

> Public Suffix List handling for cookie / origin policy.
> <https://github.com/rockdaboot/libpsl>
>
> Copyright (c) 2014-2024 Tim Rühsen

Licensed under the MIT License. See the quickjs-ng section above for the
full text (same license).

### SQLite — public domain

> Embedded SQL database used for history, bookmarks, and caches.
> <https://www.sqlite.org>

SQLite is in the public domain. The authors disclaim copyright to the
source code; it may be used for any purpose, commercial or
non-commercial, without restriction.

### libepoxy — MIT License

> OpenGL / OpenGL ES function-pointer management (used by the WebGL
> backend and GTK's GL rendering).
> <https://github.com/anholt/libepoxy>
>
> Copyright (c) 2013-2014 Intel Corporation

Licensed under the MIT License. See the quickjs-ng section above for the
full text (same license).

### zlib — zlib License

> DEFLATE compression used by image decoders and HTTP content decoding.
> <https://zlib.net>
>
> Copyright (c) 1995-2024 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software. Permission is granted to anyone to
use this software for any purpose, including commercial applications, and
to alter it and redistribute it freely, subject to the restrictions in
the zlib license: the origin of this software must not be misrepresented;
altered source versions must be plainly marked as such; and this notice
may not be removed from any source distribution.

### libseccomp — GNU LGPL 2.1 (Linux only)

> Syscall-filter sandbox for the renderer process. Linked only on Linux.
> <https://github.com/seccomp/libseccomp>
>
> Copyright (c) Paul Moore and the libseccomp contributors.

Licensed under the GNU Lesser General Public License version 2.1. See the
LGPL section below for terms and obligations.

### libuchardet — MPL-1.1 / LGPL-2.1+ / GPL-2.0+ (tri-license)

> Charset detection.
> <https://www.freedesktop.org/wiki/Software/uchardet/>
>
> Based on Mozilla's universalchardet, originally Copyright (c)
> 1998-2006 Netscape Communications Corporation and others.

Distributed under the terms of the Mozilla Public License 1.1, the
GNU Lesser General Public License 2.1, or the GNU General Public
License 2.0, at your option. The full license texts are available at:

- <https://www.mozilla.org/MPL/1.1/>
- <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

### GTK 4, GLib, Pango, gdk-pixbuf — GNU LGPL 2.1 or later

> UI toolkit and core utilities.
> <https://www.gtk.org>, <https://gitlab.gnome.org/GNOME/glib>,
> <https://gitlab.gnome.org/GNOME/pango>,
> <https://gitlab.gnome.org/GNOME/gdk-pixbuf>
>
> Copyright the GNU Project and contributors.

These libraries are licensed under the GNU Lesser General Public
License version 2.1, or (at your option) any later version. The full
license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Per LGPL section 6, since Nordstjernen links to these libraries
dynamically, you are entitled to modify them and re-link Nordstjernen
against the modified copies. On Windows / macOS bundles the libraries
are shipped alongside the executable as ordinary DLLs / dylibs that you
can replace; on Linux distributions they are loaded from the system
package manager.

### FFmpeg — libavformat / libavcodec / libavutil / libswscale / libswresample — GNU LGPL 2.1 or later (inline WebM)

> Container demuxing and audio/video decoding for the inline WebM path
> (VP9/VP8 video, Opus/Vorbis audio). <https://ffmpeg.org>
>
> Copyright the FFmpeg developers.

Required on Linux and Windows, auto-detected on macOS, and absent from
Android builds, so it is present in every build with WebM support. The copy
bundled in the macOS / Windows releases is built **LGPL-only** — its
`configure` uses `--disable-gpl --disable-nonfree --disable-version3
--disable-autodetect`, so it contains no GPL components and no external
codec libraries; the enabled VP8/VP9/Opus/Vorbis decoders are FFmpeg's
own LGPL implementations (see `scripts/build-ffmpeg-lgpl.sh`). The full
license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Per LGPL section 6, the libraries are linked dynamically: on the
macOS / Windows bundles they ship beside the executable as ordinary
dylibs / DLLs you can replace and re-link against; on Linux they are
loaded from the distribution's FFmpeg packages.

### Cairo — LGPL-2.1 or MPL-1.1

> 2D drawing.
> <https://www.cairographics.org>
>
> Copyright Carl Worth, Behdad Esfahbod, and the Cairo contributors.

Dual-licensed under the GNU Lesser General Public License 2.1 or the
Mozilla Public License 1.1. See:

- <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- <https://www.mozilla.org/MPL/1.1/>

### librsvg — GNU LGPL 2.1 or later (Windows / macOS bundles)

> Not used by the browser: SVG is rendered in-engine by `src/svg.c`.
> librsvg reaches the Windows and macOS bundles only as a transitive
> dependency of the gdk-pixbuf loader directory and of GTK's symbolic
> icon loading, so its notice is reproduced here for redistribution.
> <https://gitlab.gnome.org/GNOME/librsvg>
>
> Copyright the GNU Project and contributors.

Licensed under the GNU Lesser General Public License version 2.1 or
later. See the LGPL section above for terms and obligations.

### Optional dynamic dependencies

These are linked only when present on the build host (meson
`required: false`). When a build bundles them, their notices apply:

- **libavif** — BSD 2-Clause, © the AOMedia / libavif authors. AVIF
  image decoding.
- **Fontconfig** — MIT-style license, © Keith Packard and contributors.
- **FreeType** — FreeType License (BSD-style with credit clause) or GNU
  GPL 2.0, at your option, © The FreeType Project.
- **wgpu-native** — MIT or Apache 2.0, © the gfx-rs authors. The
  experimental WebGPU backend. Its two C headers are vendored under
  `third_party/wgpu-native/include/webgpu/`: `webgpu.h` is BSD 3-Clause,
  © 2019-2023 the WebGPU-Native developers; `wgpu.h` carries
  wgpu-native's own MIT-or-Apache-2.0 terms. The library itself is never
  vendored — it is located at build time and, in the release bundles,
  ships beside the executable.
- **QuickJS** — MIT, © 2017-2026 Fabrice Bellard and Charlie Gordon.
  <https://github.com/bellard/quickjs>. Vendored and modified in
  `src/quickjs-bellard/` (license in `src/quickjs-bellard/LICENSE`) and
  linked statically into the default build; `-Dquickjs=quickjs-ng` links
  the quickjs-ng fork instead. Its license text is the MIT text in the
  quickjs-ng section above.

---


## NOTICE files

Apache 2.0 section 4(d) requires propagating any `NOTICE` files
shipped with the upstream sources. As of this release:

- Wuffs ships no `NOTICE` file.
- WAMR ships no `NOTICE` file.

If a future upstream release adds one, it will be included verbatim
in this section.

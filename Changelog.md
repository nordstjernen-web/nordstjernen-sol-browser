Changelog:
==Significant changes in each release:

1.0.39-dev:
======
* Layout: an image with an aspect ratio but no natural width or height
  (an SVG with only a `viewBox`) fills the available width when its
  `width` and `height` are auto, and contributes nothing to min- and
  max-content sizes, as in Chrome (CSS 2.1 10.3.2). It was laid out at a
  made-up 150 px height. Inside a float or inline-block it is 0x0, in a
  flex or grid item or a block it fills the container.
* Text layout: the stand-in glyph ids that kept icon-font ligatures
  working are built only against HarfBuzz 11.4 to 14.5. Those releases
  store coverage-cache entries without checking the glyph id, so the
  made-up id for a missing space overwrote the entry for the letter "s";
  HarfBuzz 14.6 checks the id again, and releases before 11.4 never
  skipped the check. Builds against HarfBuzz 8.3 (Ubuntu 24.04), 10.2
  (Debian 13) or 14.6 and later shape exactly as upstream Pango does.
* The shell's own SVG icons are drawn by the in-engine SVG renderer
  (`src/gtk/icons.c`) only when built against GTK older than 4.22. GTK
  4.22 draws SVG icons itself, symbolic ones included, without
  gdk-pixbuf's librsvg loader, so there the toolbar, tab and address-bar
  icons and the window icon go straight through GTK's icon theme.
  Ubuntu 24.04 (GTK 4.14) and Debian 13 (GTK 4.18) builds keep the
  in-engine path; Windows, which already requires GTK 4.22.1, no longer
  compiles it.
* Web Storage: `sessionStorage` lives for the tab, per origin, as the
  standard requires. Every navigation used to start with an empty
  store, so a login that saves its OIDC state on one origin, visits
  another and comes back (BankID's app.bankid.no callback) found its
  state gone and rejected the sign-in.
* Networking: response bodies are decoded by the browser instead of
  libcurl (gzip and multi-member gzip, zlib-wrapped and raw deflate,
  Brotli, zstd and stacked codings). A `Content-Encoding` value the
  browser does not know now passes the body through unchanged, as other
  browsers do; libcurl failed the whole request with "Unrecognized
  content encoding type", which broke BankID's callback server that
  sends `Content-Encoding: ISO-8859-1`. Decoded output is held to the
  response budget, `Accept-Encoding` advertises only what can be
  decoded, and libzstd is a new optional dependency.
* Content-Security-Policy: stylesheets and `@import`s inside an iframe are
  checked against the frame document's policy instead of the top page's,
  so a cross-origin frame's own stylesheet is no longer dropped when the
  embedding page restricts `style-src` (BankID's password step rendered
  unstyled and hid its input below the frame).
* Content-Security-Policy: an iframe now runs under its own policy. Its
  `fetch`, XHR, WebSocket and EventSource requests, redirects, `eval`,
  inserted scripts, preloads, inline handlers, workers and nested frames
  are checked against the CSP its document was served with (about:blank
  and srcdoc frames inherit their parent's). Until now every frame was
  judged by the top page's policy with `'self'` resolved to the frame's
  own origin, so a cross-origin frame was refused requests its own policy
  allowed, and the restrictions its policy did set were never enforced.
  The BankID client frame on login.bankid.no failed with a
  "kommunikasjonsfeil" because of this.
* HTTP cache: `stale-while-revalidate` (RFC 5861) is honoured. A cached
  response whose `max-age` has lapsed but that is still inside the window
  the server granted is served from disk at once, and a conditional
  request refreshes the entry in the background, so a page's CDN assets
  no longer block on a round trip every time their lifetime runs out.
  `must-revalidate` disables the window, and a reload bypasses it. The
  cache schema gains a column, so the on-disk cache is rebuilt once.
* Networking: `103 Early Hints` are honoured on HTTP/2 and HTTP/3
  navigations. `Link: <…>; rel=preload` entries start the stylesheet,
  script, font or image fetch the moment the interim response arrives,
  and `rel=preconnect` opens the connection, so a server that hints its
  critical assets while it is still rendering the page no longer waits
  for the HTML to parse before the browser asks for them. The hinted
  fetches land in the same preload map the document scan uses, so the
  scan does not request them a second time.
* HTTP cache: responses marked `Cache-Control: private` are now stored.
  The browser is a private cache, so the directive only excludes shared
  proxies; refusing it refetched every logged-in page's assets on every
  visit. The directive list is parsed as tokens instead of substring
  matches, and `no-store` on a `304` evicts the stale entry.
* HTTP cache: freshness follows RFC 9111. The `Age` header and the gap
  between the `Date` header and arrival are subtracted from the
  lifetime, `Expires` is measured from `Date` rather than from arrival,
  and a response with only a `Last-Modified` header gets the heuristic
  lifetime of a tenth of its age, capped at one day, instead of being
  revalidated on every use.
* Security: the `tls_allow_insecure_override` setting now lets only the
  page being navigated to proceed past a certificate error. Every other
  HTTPS request that failed verification — scripts, stylesheets, frames,
  `fetch()`, workers and media — was retried with verification off as
  well, so a user who turned the setting on for one broken intranet host
  handed an on-path attacker every script on every site. Those requests
  now keep failing; the retried navigation still sends and stores no
  cookies and is marked as not secure in the address bar.
* Security: the framebuffer the shell shares with each renderer is sealed
  against shrinking and growing before the renderer receives it. A
  compromised renderer could truncate the memory file it paints into and
  make the shell's next read of the mapping fault, taking down every tab.

1.0.38:
======
* CSS: `rem` inside nested math functions (`max(26.4rem, min(…))`,
  `round(down, …, .8rem)`) reached through `var()` now uses the root
  element's font size instead of 16px. chess.com's board, sized this way
  on a `font-size: 62.5%` root, came out at the minimum width instead of
  filling the window height.
* WebSocket: the opening handshake now carries the page's cookies, under
  the same cookie policy and SameSite rules as a subresource request.
  Servers that authenticate the socket by session cookie (chess.com's
  live updates) no longer reject every subscription with 401.
* Security: Nordstjernen no longer runs on a Linux kernel without the
  Landlock filesystem sandbox. It refuses to start, with exit code 78 and a
  message saying whether Landlock is missing from the kernel or only
  absent from its `lsm=` boot parameter, instead of continuing with the
  renderer unconfined. A renderer or media helper whose Landlock ruleset
  cannot be enforced exits the same way. `NS_NO_SANDBOX=1` remains the
  debugging override; Android is unaffected.
* `about:nordstjernen` has a Security section: the kernel's Landlock ABI,
  whether the Landlock filesystem sandbox, the seccomp syscall filter and
  `no_new_privs` are in force in the renderer process, root status and
  allocator hardening on Linux; the Seatbelt sandbox on macOS; the process
  mitigations on Windows; and the HTTPS-First, Global Privacy Control, Do
  Not Track and tracking-parameter settings. The seccomp row moved there
  from the feature list.
* The Fedora RPM is built for Fedora 45 and verified by the release
  workflow. Fedora 44 ships FFmpeg 8 (`libavcodec.so.62`) and Fedora 45
  ships FFmpeg 9 (`libavcodec.so.63`), so the 1.0.36 RPM built on Fedora 44
  would not install on Fedora 45 ("nothing provides libavcodec.so.62"). The
  RPM is now built in a Fedora 45 container against Fedora's `ffmpeg-free`
  libraries and installed there with `dnf` before it is published, the way
  the Ubuntu and Debian `.deb`s already were. The Fedora build compiles in
  GStreamer for H.264/AAC, which `ffmpeg-free` does not decode, and
  `nightly-distro-build.sh fedora` / `nightly.sh` build it locally too.
* Build: the alternate libnghttp2 HTTP client backend
  (`-Dhttp_backend=nghttp2`, `src/net_http2.c`) is removed, together with
  its HTTP/3-over-QUIC sub-feature (ngtcp2 + nghttp3 + gnutls). Page and
  subresource fetches always go through libcurl, which still negotiates
  HTTP/2, and HTTP/3 through the alt-svc cache when the linked libcurl was
  built with it. The `http_backend` meson option is gone, so a configure
  that still passes it fails, and the build no longer looks for libnghttp2,
  ngtcp2, nghttp3, gnutls or libssl.
* Security: `window.confirm()` now returns `false` and `window.prompt()`
  returns `null`, the cancel outcomes the HTML standard prescribes when
  no dialog can be shown, instead of silently approving the action;
  both log their message to the console like `alert()`.
* Security: the file name a page suggests for a download is sanitised
  before it is checked, shown or saved: control and bidi-override
  characters are dropped, Windows-reserved characters replaced,
  trailing dots and spaces trimmed and reserved device names prefixed,
  so `evil.exe ` can no longer skip the program-file confirmation.
* Security: a finished download is moved into Downloads only when the
  renderer's reported path is a regular file directly inside the
  download staging directory (`~/.cache/nordstjernen/downloads/`), and
  the move no longer follows symlinks. The origin recorded on the saved
  file is written only for clean http(s) URLs.
* Remote control: a new `console` command drains the tab's console log
  (page and worker `console.*` output, uncaught errors, unhandled
  rejections, resource errors) over the control channel, so a script or
  MCP client can see why a site's JavaScript or WebAssembly stopped
  without opening the developer console.
* WebAssembly SIMD: `v128` and the full SIMD opcode set now run in the
  WAMR fast interpreter through the header-only SIMDe library
  (`-Dwasm_simd`, auto: the system `simde` package or the pinned
  release fetched by `subprojects/simde.wrap`; the configure summary
  shows `WebAssembly SIMD (SIMDe)`). `WebAssembly.validate()` accepts
  SIMD modules, so sites that feature-detect load their SIMD build
  instead of a scalar fallback — chess.com's bot engine now loads its
  default build. A WAMR bug that made `local.set`/`local.tee` on a `v128`
  local in a function with many locals trap with "invalid local type"
  (which aborted that engine on first use) is fixed in the vendored
  interpreter. Debian, RPM and Arch packaging gain the simde build
  dependency.
* Windows: the browser and renderer processes now request a 1 ms system
  timer resolution (`timeBeginPeriod(1)`), like other browsers do, so a
  JavaScript `setTimeout` of a few milliseconds fires after a few
  milliseconds instead of at the next 15.6 ms tick. A worker that yields
  with `setTimeout(0)` between work slices — chess engines and other
  compute-heavy scripts do — ran about 3× slower than it needed to: 300
  chained zero-delay timers took 4.6 s and now take 1.4 s.
* Layout: a column flex container's content height is now the sum of its
  items' hypothetical main sizes (flex-basis clamped by min/max) instead
  of max(flex-basis, content height), so a `flex: 1 1 0; overflow: auto`
  child no longer forces its parent to its full scroll height and the
  parent can shrink as a flex item itself. A grid item with an explicit
  `min-height: 0` in a `0fr` row now collapses to 0 (the hide/show
  `grid-template-rows: 0fr` → `1fr` animation pattern). Both matched
  Chromium on the new `flex-column-hypothetical-size` render fixture; on
  chess.com the bot panel now fits the window, so its New Game / Play
  button is reachable and a game against a bot can be started and played.
* `scrollIntoView()` now clamps the viewport scroll to the document's
  scrollable range like `scrollTo()` does. A page whose content fit the
  window could end up with `window.scrollY` at a few hundred pixels while
  nothing had visibly moved, so every `getBoundingClientRect()` was offset
  by that amount; on chess.com this put click-to-move on the wrong square
  and left the consent banner's buttons unreachable by script.
* Security: the UI process's JSON parser for renderer and remote-control
  messages now rejects `\u0000`, which produced a string whose C length
  differed from its parsed length. `SECURITY.md` drops the stale
  "HttpOnly name collisions" known gap (the `document.cookie` setter has
  refused such writes since the 2026-10-23 audit fixes).
* Security: `document.cookie` on a `file:` page is now kept only in that
  document's memory. Every local HTML file used to share one persistent
  cookie jar, so a downloaded page opened from Downloads could read the
  cookies set by any other local page and leave cookies behind for them.
* Security: audio played through the `nordstjernen-audio` helper (`.mp3`,
  and `.ogg`/`.opus`/`.wav`/`.webm` with FFmpeg) is now fetched by the
  renderer through the normal network path — CSP `media-src`,
  mixed-content blocking, the cookie partition, proxy settings and HSTS
  all apply — and handed to the helper as a file under
  `~/.cache/nordstjernen/msaudio/`. Before, the helper fetched the URL
  itself with a bare libcurl handle, so an `https:` page could load
  `http:` audio and CSP never saw the request. The UI process now refuses
  to forward any helper `open` whose URL is not such a file.
* Security: downloads are now fetched by the tab's sandboxed renderer and
  streamed straight to disk (`~/.cache/nordstjernen/downloads/*.part`, no
  memory buffering, 16 GiB cap), with the UI process only polling progress
  and moving the finished file into Downloads. Before, the UI process
  fetched the whole body into memory itself — TLS, HTTP parsing and
  decompression of attacker-controlled bytes outside the sandbox, with a
  cap of half the machine's RAM. A download whose name ends in an
  executable or installer extension (`.exe`, `.msi`, `.sh`, `.desktop`,
  `.jar`, `.apk`, …) now asks for confirmation, naming the file and its
  host. A download is tied to its tab and stops if that tab is closed.
* Security: a settings change proposed by a renderer (the settings page is
  rendered in the sandboxed renderer) now needs the user's confirmation
  for every key except a short cosmetic allow-list (window size, layout
  viewport, GSK renderer, preload, async image decode, images, cache
  size). Before, only proxy/DNS/home-page-type keys prompted, so a
  compromised renderer could silently switch on WebGL and WebGPU (which
  widens the sandbox of every renderer started afterwards), the camera
  and microphone master switches, or disable JavaScript limits. The
  dialog also truncates and strips control and bidi characters from the
  values it shows.
* Security (Windows): a renderer process now inherits only its own
  framebuffer mapping, its two IPC pipe ends and stderr. The shell passes
  that explicit handle list to `CreateProcess`
  (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) and creates the mapping
  non-inheritable, duplicating it just for the spawn; before, every
  renderer inherited the live read/write mapping of every tab opened
  earlier and could read or overwrite their pixels. The renderer command
  line is also checked for truncation instead of being silently cut.
* Security: the remote-control port and token file (`remote-control.json`)
  moved from the user data directory to the XDG state directory
  (`~/.local/state/nordstjernen/`), which the sandboxed renderers can
  neither read nor write, so a compromised page can no longer read the
  token and drive other tabs, or plant a forged file that points an MCP
  client at itself. The `--mcp` client also refuses a control file that is
  not a regular file owned by the user with mode 0600, and the renderer
  seccomp filter no longer allows `listen`, `accept` or `accept4`.
* Packaging: the `nordstjernen-browser` npm package now builds Nordstjernen
  on Windows instead of opening the Microsoft Store page. Like the Northstar
  and Southstar packages, it finds MSYS2 (`C:\msys64` or `MSYS2_ROOT`),
  checks for the UCRT64 clang, meson, GTK 4, FFmpeg and the other build
  dependencies and names the `pacman` command for whatever is missing, builds
  the release source with that environment's programs, and starts the
  browser with the MSYS2 DLLs on `PATH`. The installer's `install.js`,
  `system.js` and `dependencies.js` are now the same files in all three
  packages, with the Nordstjernen specifics in `lib/product.js`.
* Packaging: on Linux, macOS and the BSDs the npm installer sets the
  executable bits on the installed programs, and restores them on an
  existing install when `nordstjernen` starts.

1.0.37:
======
* Packaging: Nordstjernen can be installed from npm as
  `nordstjernen-browser` (`npm/`). On Linux, the BSDs and Intel macOS it
  is built from the release source against the system's libraries, since
  a single prebuilt binary cannot match every distribution's GTK and
  FFmpeg. Apple silicon Macs get the notarised release build, checked
  against `SHA256SUMS`. On Windows the `nordstjernen` command opens the
  Microsoft Store page. Before a source build downloads anything, the
  installer checks every build tool and library and names whatever is
  missing, or too old because the distribution predates the one
  Nordstjernen needs. It shows the build's progress, keeps a log, and
  checks the source tarball against a checksum recorded in the package.
  Intel Macs get Homebrew instructions.
* Packaging: the Arch Linux package (`packaging/arch/`) builds release
  1.0.36. It was rebuilt in a clean Arch container and passes namcap.
* Performance: appending children to an element that is not in the
  document no longer slows down with every child. Each append ran the
  incremental style invalidation, which checks every existing child of
  the parent, so building a list of 20,000 items off-document took about
  8 seconds; it now takes about 30 ms. Elements outside the document have
  no styles to invalidate, and the subtree is restyled when it is
  inserted.

1.0.36:
======
* Build: `config_free_strings` casts the string slot through `void *`,
  so clang's `-Wcast-align` no longer fails the `--werror` Linux, macOS
  and Windows CI builds.
* Security (iframes): frames that are cross-origin to their embedder,
  sandboxed without `allow-same-origin`, or nested inside such a frame now
  get a JavaScript realm built natively, the same way as the page's main
  realm, instead of one cloned from the embedder. Their interface
  objects, prototypes, polyfill-implemented APIs, collections, events and
  location objects all belong to the frame's own context. The polyfill
  bundle is compiled to bytecode once per process and loaded per context.
  The engine helpers behind `URL`, `URLSearchParams`, `getComputedStyle`,
  `NodeList` and body consumers are kept per context, and events the
  engine builds for a node in another realm are adopted into that realm,
  which also fixes `click()` events inside same-origin frames.
* Interface member naming at realm setup runs in C instead of JavaScript,
  cutting about 20 ms from every page load and every new frame.
* Security (Linux sandbox): with WebGL and WebGPU both switched off, the
  renderer's seccomp filter refuses `mprotect`/`pkey_mprotect` with
  `PROT_EXEC` and anonymous executable `mmap`, and drops
  `remap_file_pages`. That enforces W^X for injected code, while
  file-backed executable mappings stay allowed for the dynamic loader.
  GStreamer's ORC is switched to its C fallback in that mode.
* Renderers defer configuration saves to the parent only when the GTK
  shell starts them (it sets `NS_CONFIG_VIA_PARENT`). Other embedders
  of `nordstjernen-renderer`, such as the Java front-end, which do not
  handle the new channel, keep writing the file themselves.
* Security: renderers can no longer change the configuration file. A
  settings save in a page, or a camera/WebGL decision, now goes to the UI
  process, which writes `nordstjernen.conf`. If the change touches the
  home page, search engine, proxies, DNS-over-HTTPS, cookie policy,
  HTTPS-first, allocator hardening, the TLS override, remote control
  or the AI model mirror, the UI process first shows a native dialog
  listing the changes and applies them only on "Apply". On Linux the
  renderer's Landlock grant on the config directory is read-only
  (except for the cookie jars). A renderer re-reads the file on every
  navigation.
* The browser's second command name is now `open-browser-nordstjernen`
  (it was `internet-browser`): the installed symlink, the `.deb`/`.rpm`
  alternative, the Alpine link, the Windows App Paths alias and the MSIX
  execution alias. Upgrading removes the old `internet-browser`
  registration, and the Arch package now ships the link.
* Docs: SECURITY.md now states that W^X is a property of the code
  rather than something seccomp enforces (GPU shader compilers need
  executable mappings). It also lists the developer environment knobs
  and notes the extension directory's location and the Landlock thread
  caveat on older kernels. Two new known gaps are documented: content
  scripts in the page's JS world, and a renderer that can write the
  config file.
* Security (sandbox): unpacked extensions load from
  `~/.local/share/nordstjernen-extensions` (an existing
  `nordstjernen/extensions` directory is moved there at startup), which
  the renderer cannot write, so a compromised renderer can no longer
  plant an extension that runs on every site. The renderer loses write
  access to the download directory, which only the UI process uses.
  Landlock asks the kernel to bind all threads and logs threads left
  unconfined on kernels that cannot. Profile directories created by
  older builds are tightened to `0700`. The renderer self-test is
  started with `--selftest URL` instead of an inherited environment
  variable, and the remaining `NS_NO_*`/`NS_PRIVATE`/`NS_DEELEVATED`
  switches treat `0`, `false`, `no` and `off` as unset.
* Security: SQLite profile databases (IndexedDB, history and the other
  stores opened through `ns_db_open`) are set to mode `0600` when
  opened. IndexedDB files were created with SQLite's default `0644`.
* Build: `scripts/build-ffmpeg-lgpl.sh` downloads FFmpeg only from
  ffmpeg.org and checks the tarball against a pinned SHA-256 (the
  mutable GitHub tag fallback is gone; other versions need
  `FFMPEG_SHA256`). `scripts/fetch-wgpu-native.sh` checks the release
  zip against the hash pinned in the matching `.wrap` (or
  `WGPU_NATIVE_SHA256`). Third-party GitHub Actions are pinned to
  commit SHAs.
* Security: `WebAssembly.Instance` reads every import before linking
  any global, so an import getter that instantiates the same module
  again can no longer swap the outer instance's global values. Camera
  permission decisions are guarded by a lock, and a decision for an
  opaque origin (`data:`, sandboxed or blank documents) is used once
  rather than stored for every such page. A huge `backdrop-filter:
  blur()` radius or `drop-shadow()` offset no longer overflows an
  integer conversion.
* Security (extensions): `chrome.storage.local/sync/managed` `get`,
  `set`, `remove` and `clear` run entirely in C and return the engine's
  own promises, defining result properties rather than assigning them.
  The shim no longer calls anything a page could have replaced
  (`Promise`, `Object.keys`, setters on `Object.prototype`). Files an
  extension loads are opened by their resolved path, and on Windows
  symlinks and junctions are resolved too, so a link inside an
  extension cannot point outside it.
* Security (CSP): an `EventSource` whose server redirects outside the
  page's `connect-src` is stopped before the redirected request is
  sent; it was only refused after libcurl had already followed it.
* Security (CSP): `img-src`, `font-src`, `style-src` (linked
  stylesheets and `@import`) and `connect-src` are enforced on every
  request, including each redirect hop before it is sent. Before, the
  engine loaded images, fonts and stylesheets without consulting the
  page's policy at all, and the speculative preloader fetched scripts
  and styles before any policy was known. The page's policy is now
  registered from its response header and `<meta>` tags before the
  first subresource request.
* Security: every in-page `blob:` lookup (scripts, module imports,
  workers, images, `fetch`) checks that the URL belongs to the
  document's origin, and the internal hook that refreshes a blob URL's
  contents only updates an existing URL of the page's own origin
  instead of accepting any `blob:` string.
* Security: a `<link>` stylesheet that redirects to another origin is
  held to that origin: it must be `text/css` even in quirks mode, and
  its text is not exposed to page script through `cssRules`. The checks
  used the URL the page asked for, so a same-origin redirect let a
  quirks-mode page read a cross-origin response.
* Security: an `<iframe>` appended as a DOM child of another `<iframe>`
  element belongs to the outer page, not to the outer frame's document,
  so its origin is compared with the page's and not with the outer
  frame's URL. Cross-origin and sandboxed frames get their own
  `document.location`, `document.defaultView` and
  `document.adoptedStyleSheets` instead of objects created by the
  embedding page.
* Security: three routes past the CORS preflight are closed. A
  307/308 redirect that keeps a non-simple method is preflighted again
  at its new origin. `navigator.sendBeacon()` with a non-safelisted
  content type such as `application/json` is preflighted. A fetch that a
  service worker hands back to the network now keeps the preflight.
* Security: cookies obey `SameSite`. On a navigation from another site,
  `Strict` cookies are withheld, and on a cross-site POST (or any other
  unsafe method) `Lax` ones too; a cookie without the attribute counts
  as `Lax`, as in other browsers. A form submitted by a user's click
  now also reports the page it came from: it went out with the target
  site's own `Origin` and `Sec-Fetch-Site: none`, which only
  script-driven navigations had been fixed for. Cross-site GET
  navigations no longer send `Origin`.
* Docs: SECURITY.md now matches the code. Service workers and
  extensions are implemented, and WebGL and WebGPU are on by default.
  The full codec set is listed. The Landlock list now includes the
  download directory, `/proc`, `/sys`, `/run`, the runtime directory,
  camera nodes and `/dev/snd`, and notes the thread caveat.
* Security (Linux sandbox): renderers get the XDG runtime directory
  (`/run/user/$UID`) read-only instead of read+write. The download
  directory is writable only when it is a real sub-directory of
  `$HOME`; when `XDG_DOWNLOAD_DIR` was `$HOME` itself (or merely
  started with the same characters), all of it became writable.
  The thread-dump helper thread starts after Landlock is applied
  instead of before it, so it no longer runs unconfined, and Landlock
  is applied to every thread where the kernel headers offer
  `LANDLOCK_RESTRICT_SELF_TSYNC`.
* Security: a `MessagePort` transferred into another frame through
  `postMessage` now belongs to the receiving realm: it takes that
  realm's `MessagePort.prototype`, and messages posted to it are
  delivered there. The port used to keep the sender's prototype and an
  internal `_realm` function, so a frame could reach the sender's
  `Function` through `port.postMessage.constructor`, and the port's
  messages were cloned into the sender's realm. The port's internal
  fields are hidden from page script.
* Security: the config file, session list, cookie jar, HSTS store,
  localStorage, extension storage and the font and media caches are
  written with mode `0600`, as SECURITY.md states. They were created
  with the default umask, usually world-readable `0644`.
* Packaging: `scripts/pack-appimage.sh` downloads pinned releases of
  linuxdeploy, linuxdeploy-plugin-gtk, appimagetool and the type2
  runtime and checks each against a SHA-256 before running it, also
  when the file is already cached. It used to fetch the moving
  `continuous`/`master` builds and run them unchecked.
* Security: each `new WebAssembly.Instance()` links its own values for
  imported globals and throws `LinkError` when one is missing or has
  the wrong type. A missing global used to keep whatever value an
  earlier instance of the same module had linked, and a NaN or huge
  number for an `i32` global went through an undefined float-to-int
  cast. A `filter: blur()` sigma is clamped (and NaN ignored) before it
  is turned into box widths. The Safe Browsing tables, read from
  network threads, are guarded by a mutex.
* Security: a cross-origin `fetch()` or XHR response shows script only
  the CORS-safelisted headers and those named in
  `Access-Control-Expose-Headers`. `Content-Disposition`,
  `Content-Security-Policy`, `X-Frame-Options` and
  `Access-Control-Allow-Origin` used to be readable on every
  CORS-allowed response.
* Security (XML): the XHTML well-formedness check and the XML parser
  find the end of a `<!DOCTYPE … [ … ]>` the same way, skipping quoted
  strings and comments in the internal subset. The check stopped at the
  first `>` and the parser at the first unbalanced `]>`, even inside
  an entity value like `"]>"`, so the two could disagree about where
  the document started.
* Security (CSP): keyword sources match case-insensitively (`'SELF'`,
  `'Strict-Dynamic'`, `'NONCE-…'`), as the spec says; an ignored
  `'strict-dynamic'` used to re-enable host allow-lists. A source
  with an empty host (`https://`) no longer matches everything without
  a host. Dedicated and shared workers inherit the page's policy, so
  their `fetch`, XHR and WebSocket obey `connect-src`, and
  `importScripts` obeys `script-src`. A classic `<script src>` and an
  `EventSource` are checked again after a redirect.
  `navigator.serviceWorker.register()` needs a secure context and a
  scope under the script's directory.
* Security (extensions): an Adblock cosmetic filter whose selector holds
  `}`, `/*` or a trailing backslash is ignored instead of being pasted
  into the generated stylesheet, where it could inject rules. Match
  patterns and `urlFilter` wildcards are matched without recursive
  backtracking, which was exponential in the number of `*`s. Files an
  extension loads must resolve, after symlinks, inside the extension
  directory. `chrome.storage` parses and serialises JSON in C and uses
  intrinsics captured when the shim starts, so a page replacing
  `JSON.parse` no longer sees or rewrites the extension's storage.
* Security: the sandbox and hardening switches (`NS_NO_SANDBOX`,
  `NS_NO_SECCOMP`, `NS_NO_WIN32_MITIGATIONS`, `NS_ALLOW_ROOT`,
  `NS_REQUIRE_SANDBOX`) count as set only for a value other than empty,
  `0`, `false`, `no` or `off`. `NS_NO_SANDBOX=0` used to turn the
  sandbox off. The undocumented `NS_EARLY_JS_FILE` (run a local file as
  script in every page) and `NS_CAMERA_ALLOW` (grant every site the
  camera without asking) are gone.
* Security: `<input type="file">` no longer reads a file from disk
  because of an internal `data-nd-file-path` attribute. Nothing in the
  browser sets that attribute; the code was only reachable if a page
  managed to set an internal attribute, and then it would have read any
  path the renderer can open into `input.files`.
* Security: a `blob:` URL only resolves for documents of the origin
  that created it. The network-side resolver searched every page's
  blob table, so one site could read another site's object URLs if it
  learned the URL.
* Security: a function passed into a `ShadowRealm` through a wrapped
  function now reaches the realm as a wrapper created in that realm, as
  the proposal specifies. It used to arrive unwrapped, so code in the
  realm could reach the page's `Function` constructor through
  `f.constructor`. `ShadowRealm.prototype.evaluate` and `eval` inside
  the realm also obey a Content-Security-Policy without
  `'unsafe-eval'`.
* Security: classic scripts inside an `<iframe>` get the same checks as
  scripts in the top document. A frame loaded `<script src>` without
  blocking mixed content, without checking `integrity`, and without
  any Content-Security-Policy. The frame's own CSP now applies to its
  external and inline scripts, including the URL a script redirected
  to; `srcdoc` and `about:blank` frames inherit the embedder's policy.
* Security: a page can no longer reach the raw window of a cross-origin
  frame through the proxy that stands in for it. The proxy was built
  after page scripts had run, with the page's own `WeakMap`, `Proxy`,
  `Array.prototype.concat` and `__nsPostMessageFrom`, so overriding any
  of them handed the page the real frame window. It is now built from
  intrinsics captured when the realm is set up and kept under an
  engine-private name. Sandboxed and cross-origin frames also throw
  their own realm's `DOMException` for denied storage, and no longer
  get the parent's `document.timeline`, both of which led back to the
  parent's `Function`.
* Security: `fetch()` and `XMLHttpRequest` send a CORS preflight
  (`OPTIONS` with `Access-Control-Request-Method`/`-Headers`) before a
  cross-origin request with a non-simple method, header or content
  type, and fail the request unless the server allows its origin,
  method and headers. Before, a cross-origin `PUT`/`DELETE` or a
  request with custom headers reached the server unchecked. Successful
  preflights are cached in memory for `Access-Control-Max-Age` (5 s by
  default, 2 hours at most).
* Security: a navigation a page starts towards another origin now
  reports that page as its initiator. The renderer dropped the
  cross-origin referrer so the target would load in its own cookie
  partition, which also made the request look browser-initiated: a
  cross-site form POST went out with `Sec-Fetch-Site: none`, counted
  as user-activated, and carried `Origin:` of the target itself. The
  initiator now sets `Origin` and `Sec-Fetch-Site`, while cookies are
  still those of the destination site.
* Security: `CSSStyleSheet.cssRules` of a `<link>` stylesheet only reads
  back the sheet's text when it is same-origin with the document, and a
  cross-origin stylesheet must be served as `text/css` even in quirks
  mode. A quirks-mode page could link a cross-origin HTML or JSON
  response as a stylesheet and read the body verbatim.
* Security: the Safe Browsing "continue anyway" scheme only leads on to
  an `http:` or `https:` URL. A page could wrap a `file://` URL whose
  host matched its own and open a local file, since the stripped target
  was loaded with no referrer. The referrer and user activation now carry
  through to the target.
* Security: a web font whose SFNT table directory lists the same tag
  twice is refused instead of being rebuilt with the first table's bytes
  in the second table's slot.
* JavaScript: QuickJS fork synced with quickjs-ng master `270b8c9`; fixes an
  infinite loop in `unicode_script()` on allocation failure.
* CI: Gradle 9.8.2 in the Android and Java workflows and the Android wrapper;
  Android Gradle Plugin 9.4.1.
* Packaging: Arch Linux package `nordstjernen-browser` in `packaging/arch/` (PKGBUILD and
  .SRCINFO, ready for the AUR), documented in `docs/Arch.md`. It builds
  without warnings in a clean Arch chroot and passes namcap.
* Build: the sources that define `_GNU_SOURCE` now define it as `1`. SDL2
  over SDL3 (sdl2-compat, the `sdl2` package on Arch and other current
  distributions) adds `-D_GNU_SOURCE=1` to the compiler flags, and the
  different definition gave a redefinition warning in seven files, which
  fails a `--werror` build.
* Layout: a box with `aspect-ratio`, a definite `height` and an auto
  width now takes its width from the ratio, as CSS Sizing says. Only
  the other direction (height from width) was done, so such a box
  stretched to its container in normal flow and shrank to 0 px wide as
  a flex item, a float or an inline-block. Booking.com's hotel rating
  stars (12 px tall spans with `aspect-ratio: 1`) were all invisible,
  and the New York Times promo images were 0 px wide. A percentage
  height transfers when it resolves against a definite height, but not
  against a grid area.
* Text: a line no longer breaks after a `/` that is followed by an ASCII
  character, as in Chrome. The Unicode line breaking rules allow a break
  there, so `and/or`, `km/h`, paths and URLs wrapped after the slash,
  and the min-content width of such text came out too small.
  On Le Monde a photo credit `TROUILLARD/RCR ARQUITECTES` wrapped at the
  slash, which made the lifestyle section 11 px narrower than in Chrome.
* Layout: `grid-template-columns: subgrid` now places the subgrid's
  items on the parent's columns. The subgrid placed them as if it had a
  single column, so every item went into its own row, and the parent
  sized its columns without looking at those items. Items now resolve
  their lines against the columns the subgrid spans (nested subgrids
  too), and they contribute their sizes to the parent's column tracks.
  Mozilla's news list, a table of rows built from nested subgrids,
  showed each entry as a 200 px tall stack of lines instead of one row.
* Layout: text now flows beside a float whenever its first word fits in
  the space left, as CSS says. A line was moved below the float when
  that space was narrower than 260 px (or 40% of the container), so a
  headline next to a 496 px image in a 760 px card started under the
  image. Every Washington Post story card with its photo on the right
  showed the headline, blurb and byline below the photo.
* Paint: an image that fails to load, or has no source, no longer covers
  its box with an opaque white or light grey placeholder, so the
  element's own background and whatever lies behind it stay visible, as
  in Chrome (the ESPN story cards with a dark backdrop showed a light
  grey block). A sized image without alt text gets a thin silver frame
  and the broken-image icon when its request failed; one with alt text
  shows the icon followed by the alt text in the element's own font and
  colour. An image that is still loading paints nothing.
* Paint: `clip-path` basic shapes now clip like Chrome. `inset()` used
  only its first value, as a share of the smaller side, so
  `inset(0 100% 0 0)` hid nothing and `inset(135% 0 -100%)` clipped the
  wrong band; it now takes all four sides, percentages of the box's
  width and height, `calc()` and `round` corners. `circle()`,
  `ellipse()` and `polygon()` now measure from the border box, with
  `at` positions and circle percentages as the spec defines them, and a
  reference box keyword alone (`clip-path: padding-box`) clips to that
  box. A box with both `filter` and `clip-path` is clipped after it is
  filtered, so a blurred glow no longer spills outside its clip (the
  purple glow GitHub draws under its carousel showed through the video
  above it).
* Paint: an absolutely positioned element also takes the opacity,
  `clip-path`, mask and blend mode of a static ancestor when an
  `overflow` box sits between the two. That box does not clip the
  element, since it is not in the element's containing block chain, so
  the painter left the element outside the ancestor's group: text inside
  an `overflow: hidden` card escaped an `opacity` set on an inner
  wrapper. The part of the element inside the clipping boxes is now
  drawn in the ancestor's group, grouped with its other content as
  Chrome does, and the part outside them is drawn at the element's own
  place through the same effects; a scrolled box's offset is undone for
  both.
* Paint: the `drop-shadow()`, `hue-rotate()` and `opacity()` filter
  functions now work on elements, `filter` works on inline `<svg>`
  elements, and every function of a `filter` runs in its place in the
  list, blurs and shadows included. The element filter knew only
  grayscale, sepia, invert, brightness, contrast, saturate and blur, and
  skipped `<svg>` boxes altogether, so a `filter: drop-shadow(...)` card
  drew no shadow and a grayscale or hue-rotated SVG icon kept its
  colours; images knew `drop-shadow()` but blurred it about half as much
  as Chrome. The third length of `drop-shadow()` is now the standard
  deviation of the blur, as in Chrome, and the filtered area grows by
  the shadow's offset and blur.
* Paint: `backdrop-filter` is now drawn. The property was parsed but
  never painted, so frosted glass bars, blurred overlays and dimmed
  caption strips showed the page behind them unchanged (the Guardian
  card captions, Discord, YouTube and Apple headers). The painter now
  copies what is already drawn under the element's border box, runs the
  filter list over it and draws the result inside the element's
  rounded border box, under its background, clipped and masked with
  the element. As in Chrome, the blur reads only the area under the
  element and mirrors it at the edges. Filter functions in `filter` and
  `backdrop-filter` now also run in the order written, so
  `blur(20px) saturate(180%)` blurs before it saturates.
* Paint: `double`, `inset`, `outset`, `groove` and `ridge` borders are
  drawn in their own style. They were all painted as one solid line in
  the border colour. Double borders now show two lines with a gap, as
  wide as Chrome draws them, and the 3D styles shade each side darker
  or lighter with Chrome's colour rules, including the grey pair Chrome
  uses when the border colour is `currentcolor`. Buttons and fields that
  keep their native appearance still get a flat border, as Chrome draws
  them natively.
* Paint: a horizontal rule (`<hr>`) is drawn once. Its default style was
  a 1px grey background with 12px margins, and the painter added a
  second rule 4px below it whenever the rule had no border, so every
  plain `<hr>` showed two lines, and so did styled ones such as the BBC
  section dividers (`border-width: 0; height: 2px; background: …`). The
  rule now uses the HTML default style (a 1px inset border in grey, 0.5em
  margins) and paints only what its CSS asks for. The `color`,
  `noshade` and `size` attributes map to the same properties as in
  Chrome.
* Performance: a web font that finishes loading no longer rebuilds the
  whole font map. Each arrival threw away every font, face and coverage
  table the text library had built and listed all installed fonts again,
  which was 12% of the renderer's busy time on The New York Times, where
  57 fonts arrive. The new font's faces are now added to the map that is
  already there, and only the cached font matches are dropped, so text
  still switches to the font as soon as it arrives. On frozen copies of
  CNN, The Guardian and The New York Times the main-thread CPU of a
  settled load drops by 16%, 15% and 13%. Builds that link the system
  Pango keep the full rebuild.
* Crash fix: a page parked in the back-forward cache could still run an
  async script, and the script's synchronous fetch runs a nested main
  loop. If the tab was closed or navigated during that fetch, the parked
  page was freed under the running script, and the browser crashed when
  the fetch returned (espn.com, which redirects to a regional edition,
  crashed on 3 of 5 headless loads). A tab's renderer session now counts
  as busy while any of its pages, parked ones included, is inside such a
  fetch, so closing or navigating the tab waits until the fetch ends.
* JavaScript: `TimeRanges.start()`, `TimeRanges.end()` and
  `XPathResult.snapshotItem()` now convert their index the way WebIDL
  converts an `unsigned long`: `undefined` and `NaN` are 0 and a
  fraction is truncated. They used `Number()`, so `start(undefined)`
  threw `IndexSizeError`. TikTok's player logs its buffered ranges with
  `Array.from(Array(n)).map(e => r.start(e))`, which passes `undefined`;
  the throw aborted `endOfStream()` and the player fell back to its
  slower path (3 errors on a load). Calling `start()` or `end()` with no
  argument is now a `TypeError`, as in Chrome.
* JavaScript: the `RangeError` thrown by `toFixed()`, `toExponential()`
  and `toPrecision()` for an out-of-range digit count now carries
  Chrome's message (`toFixed() digits argument must be between 0 and
  100`) instead of "invalid number of digits". The detectIncognito
  library, used by CNN, tells engines apart by the length of the
  `(-1).toFixed(-1)` message: 51 is Chrome, 44 Safari, 25 Firefox. With
  24 it recognised none and every CNN load ended in an unhandled
  rejection, "detectIncognito cannot determine the browser".
* Service workers: a service worker's global scope now has
  `self.serviceWorker`, the `ServiceWorker` object for the worker itself
  with its `scriptURL` and `state`. Its `registration` now moves the
  worker through `installing`, `waiting` and `active`, and the worker
  gets `statechange` events, as the page side already did. AliExpress's
  service worker reads `new URL(self.serviceWorker.scriptURL).hostname`
  at start-up and logged a `TypeError` on every load.
* Web storage: the `storage` event now reaches the windows the HTML
  standard names and no others. The main window received the events
  for its own `localStorage` and `sessionStorage` writes; listeners that
  a frame added with `addEventListener('storage', …)` were never called;
  and every window saw the writer's `Storage` object as `storageArea`
  instead of its own. Temu watches `storage` events for oversized
  values and reported each of its own writes as "storage set too long!"
  (one error on every load). The static webstorage event tests of WPT go
  from 14 of 21 to 27 of 30 subtests, with two files that never finished
  now running to the end.
* Frames: an iframe's window now gets the browser's own built-in
  functions even when the parent page has replaced them. A new frame
  copied its platform globals from the parent window as they were at
  that moment, so a parent's `window.Image = function (w, h) {…}`
  became the frame's `Image`, and as an anonymous function it was
  copied as a function that cannot be called with `new`. ESPN's consent
  manager replaces `Image` in the page and builds tracking pixels with
  `new Image` inside a frame: two "not a constructor" errors on every
  load. The original functions are now recorded when the document's
  globals are installed, and frames copy those.
* Layout: line boxes now follow the CSS inline model the way Chrome
  builds them. Every line in a standards-mode block starts from the
  block's strut, so a line whose text all sits in a smaller span, a
  `<sup>` or a `vertical-align: 3px` span grows like it does in Chrome
  (a 12px span in a 16px/20px block: 21 px, was 20). Ascent and descent
  are rounded to whole pixels and the half-leading above the text is
  floored, so a 21.6px strut beside a 22px link gives a 22.59 px line,
  not 22. Bidi isolate characters around an inline `<h3>` or `<p>` no
  longer carry that element's line height with the parent's font, and a
  `line-clamp` ellipsis keeps its line height. On the AP News front page
  every promo title was 1 px short per line and the page drifted 54 px
  by the photo section; it now lines up with Chrome.
* Web storage: a cross-origin frame now has its own `localStorage` and
  `sessionStorage` instead of a `SecurityError` on access. Each frame
  gets empty storage areas that live as long as the frame, are never
  written to disk and never shared with the page or another frame, and
  their changes fire no `storage` events elsewhere. Chrome gives such a
  frame (partitioned) storage, and frames that assume it is there broke:
  the my.foxnews.com account frame on foxnews.com threw "The operation
  is insecure" from its load handler. Sandboxed frames without
  `allow-same-origin` still get the `SecurityError`, and `indexedDB`
  stays unavailable in cross-origin frames.
* Frames: `document.write()` from a script that a frame's parser runs
  now inserts the markup after that script, and a written `<script>`
  runs before the frame's next script, as in a top-level page. The
  frame document treated it as a write after loading: it emptied the
  document and kept the markup for a `document.close()` that never
  came. The my.foxnews.com account frame writes the script that defines
  `FNC.ISA`, so it ended up with an empty document and no
  `ISA.provider`.
* Web storage: every `localStorage` key and value now survives a
  restart. Keys were written to disk as raw key-file names, so a key
  with `=`, a leading or trailing space or an empty name was dropped
  with a GLib warning (14 of them on an espn.com load), and a key with a
  line break corrupted the file, so the next session lost the origin's
  whole storage. A value that was not valid UTF-8, such as a string
  with a lone surrogate, was dropped on load. Keys and values are now
  percent-escaped in a new `[items]` group; files in the old format
  still load and are rewritten in the new one.
* Layout: `fr` rows in a grid whose height is auto now share one flex
  fraction. Each `1fr` row took the height of its own content, so
  `grid-template-rows: 1fr 1fr` gave rows of 108 and 117 px where Chrome
  makes both 117, and `1fr 2fr` did not keep the 1:2 ratio. The rows now
  grow to the largest base size per `fr`, as CSS Grid's "find the size of
  an fr" step does for an indefinite height. The Guardian's newsletter
  cards, PayPal's image frames and Le Monde's teaser rows line up with
  Chrome.
* Performance: reading `document.fonts` no longer lays out the page.
  The getter flushed layout on every access so that `ready` and
  `status` would know which web fonts the page needs, and the engine's
  own setup code reads `document.fonts` for every document before any
  script runs, so each load laid out the page once during setup and
  again for its first paint. `ready` and `status` are now getters that
  flush layout when they are read, so they resolve and report exactly
  as before; getting `document.fonts` itself, `check()`, `add()` and
  the event handlers no longer flush. `loadingdone` still fires when
  the pending web fonts finish loading. On frozen copies of AP News,
  claude.ai, CNN, ESPN and IMDb the main-thread CPU to first paint
  drops by 14%, 16%, 12%, 11% and 11%.
* JavaScript modules: a bare specifier such as `import 'react'` that no
  import map maps is now a `TypeError` ("Failed to resolve module
  specifier"), as in Chrome. It was resolved as a path relative to the
  importing module and fetched: ESPN's Disney player bundle imports
  `react`, so each load requested `…/dist/react` from the CDN, got an
  XML 403 page and failed with a MIME type error instead.
* Frames: a cross-origin window and its `location` now follow the HTML
  rules for cross-origin access. `then` and the `Symbol.toStringTag`,
  `Symbol.hasInstance` and `Symbol.isConcatSpreadable` keys read as
  `undefined`, and every other member outside the allowed list throws
  a `SecurityError`, including `in` checks and property descriptors.
  Reading `then` used to throw, so resolving a promise with a
  cross-origin window (`resolve(iframe.contentWindow)`) rejected with
  "Blocked cross-origin frame access"; Le Monde's forecast SDK does
  that with its storage frame. A cross-origin `location.hostname` read
  `undefined` instead of throwing, and `location.assign` was callable.
* JavaScript: the engine passes test262 main as of 2026-10-13
  (`d53ebc8b`, 102,988 test runs, 0 failures). The tests added since
  July, such as the Intl.Locale info getters, iterator chunking and
  `using` at the top level of a module, pass unchanged.
  `docs/quickjs-ecma-specification-compliance.md` states the current
  baseline and lists the staging and Atomics work in its table.
* Layout: an `<img>` with no source and only one specified dimension is
  now empty in the other, as in Chrome. Without `src`, `srcset` or a
  `<picture>` source with a `srcset`, there is no image to give it a
  size, but the engine made it square, so a lazy-loading placeholder
  with `width: 100%` was as tall as it was wide. On ESPN the video cards
  of the news feed grew from 353 to 628 px and the feed ran 64 px long.
* CSS: the `background` and `mask` shorthands now accept a three- or
  four-value position with edge offsets, such as
  `background: url(logo.svg) no-repeat right .5rem top .5rem, #0079c8`.
  The offsets were handed to the position longhands as text they could
  not parse, so the whole declaration was dropped: no background colour,
  and the images repeated from the top left corner (the El País sponsor
  labels showed scraps of their logos on white instead of a blue bar).
* Layout: in a column flexbox, an item with `width: fit-content` (or
  `min-content`, `max-content`) is no longer stretched to the line, and
  an item with `margin-left` or `margin-right: auto` shrinks to fit and
  is pushed aside by that margin. The stretch relaid the item and reset
  its `margin-top: auto` to 0, so on the Guardian front page every card
  timestamp sat under its headline instead of at the bottom of the
  card; cross-axis auto margins only moved the item to the start.
* CSS: `em`, `rem`, `ex`, `ch` and viewport units now resolve in every
  layer of a layered value, such as the second image of
  `background-position: 8px 8px, calc(100% - .5rem) .4375rem`. Only the
  first layer was converted to pixels, and the painter read the others
  as if the font units were zero, so a second background image sat in
  the corner instead of half a rem inside it and a `background-size` of
  `10em` in a later layer was drawn 10 pixels wide. A `background-size`
  with one `auto` side (`2em auto`) is now resolved as well.
* Paint: `overflow-x: clip` with `overflow-y: visible` (and the other
  way round) now clips only the axis that asks for it. The painter
  clipped both axes whenever either one clipped, so content that sticks
  out above or below such a box was cut off. On Canva the hero gradient
  starts 80px above its `overflow-x: clip` wrapper, behind the
  transparent header, and the header area came out white with its white
  menu text invisible. A `hidden`, `scroll` or `auto` axis still clips
  both, since the other axis then computes to `auto`.
* Layout: `letter-spacing` set on an inline element now applies. Only
  the block's own letter spacing reached the text, so a span such as
  AP News's `text-transform: uppercase; letter-spacing: 1px` timestamps
  ("45 MINS AGO") and the New York Times section labels were set tight
  and 6 to 11 px too narrow. Each element whose letter spacing differs
  from its parent's now carries its own, atomic inlines inside it keep
  none, and the shrink-to-fit width leaves room for the largest spacing
  so the text does not wrap early.
* Paint: an absolutely positioned element now takes the opacity,
  `clip-path`, mask, blend mode and stacking order of a static ancestor
  that sits between it and its containing block. The layout tree hangs
  such an element under its containing block, so the painter drew it
  outside the ancestor's group: text inside an `opacity: .3` wrapper
  showed at full strength, and an absolutely positioned caption inside a
  flex item with `z-index: 1` was drawn under a `z-index: 1` gradient
  that comes earlier in the page (the IMDb trailer captions looked
  dimmed). The painter now draws such an element inside that ancestor,
  unless a box in between clips, scrolls or transforms its contents.
* Text: the shape cache no longer serves a short item as a word inside
  a longer one. An item that has no space to be cut at, such as "Hub "
  before an element in another font, is stored whole, and it was stored
  under the same key as a word shaped between two others. Its trailing
  space was shaped with nothing after it, so served inside "q&nbsp;Hub
  To" it missed the kerning against the "T" and came out 0.6 px too
  wide at 20 px in AliExpress's TT Norms Pro, and
  `NS_PANGO_SHAPE_CACHE=verify` reported the mismatch. Items stored
  whole are now keyed apart from pieces, as the pieces at an item's
  ends already were.
* Text: the shape cache no longer serves a piece that ended an item in
  the middle of another one. A trailing space shaped at the end of an
  item ("See " before a link) has nothing to kern against, but the same
  bytes inside a longer item are kerned against the next word, and the
  cached piece brought the unkerned width with it. Text came out up to
  0.6 px too wide, enough to wrap a label that fits in Chrome ("FROM
  THE ATHLETIC" on the New York Times front page), and
  `NS_PANGO_SHAPE_CACHE=verify` reported mismatches on the New York
  Times and PayPal. Pieces at an end of their item are now keyed apart
  from the rest.
* Performance: a font family that is not installed no longer makes
  fontconfig compare every installed font. For each such name (a CJK
  font on a Western system, a vendor font, an entry deep in a fallback
  list) the engine asked fontconfig for its best match to learn whether
  a metric-compatible alias, such as Liberation Sans for Arial, is
  installed, and each match took about a millisecond. The engine now
  keeps an index of the installed family names, rebuilt when a web font
  is added, and answers from it whenever the match is decided by the
  family name alone; a pattern that asks for color or another property
  that fontconfig ranks above the family still goes to fontconfig. The
  alias found is the same for all 730 family names on the 58 frozen
  sites. The frozen claude.ai page needs 14% less main-thread CPU to
  first paint, and CNN, ESPN and The New York Times retire 1% to 2%
  fewer instructions.
* Layout: a float that follows a block now starts below that block's
  bottom margin. It was placed against the block's border edge, as if
  the margin did not exist until the next in-flow block claimed it, so
  on AP News the play icon of every video card rode 10 px up into the
  headline. Chrome resolves the margins collapsed so far before it
  places the float, which is what the engine now does too.
* Text: `letter-spacing` is now added after every character, the last
  one included, as Chrome does. The text library split it around each
  character and trimmed it at the start and end of every line, so
  letter-spaced text was one spacing narrower than in Chrome and began
  half a spacing to the right inside a line; "abc" at 3 px came out
  26.4 px wide instead of 28.4. The shrink-to-fit widths that added the
  spacing back as slack no longer need to.
* Performance: an absolutely positioned element inside another one no
  longer costs two hash lookups per box of its containing block. Its
  static position comes from a walk over that block's boxes, and each
  step looked up the node's place in document order and tested the box
  against a hash of the element's ancestors. Each box now keeps its
  node's preorder rank and subtree end for the current layout, so the
  ancestor test is two comparisons, and the position keyword is checked
  once per style. The Speedometer complex-DOM pages lay out about 690
  such elements per layout, close to 500,000 walk steps: on Speedometer
  3.1, TodoMVC-React-Complex-DOM is 22% faster, Svelte 21%, Preact 19%,
  ES6 14%, Lit 11% and Angular 10%.
* Layout: an absolutely positioned element inside a table section is
  now laid out. Positioning blockifies a `<tr>`, and the row collector
  queued an absolute element only while it still counted as a row, so
  an absolutely positioned row, or any absolutely positioned child of a
  `<tbody>`, got no box at all: it never painted and script saw a 0 x 0
  rect. Wikipedia collapses navbox rows with `hidden=until-found` plus
  `position: absolute; width: 0`, and Chrome still lays their cells out
  at their min-content width, which is what scripts and the sitebench
  probe read. The visible page does not change.
* Layout: an `<svg>` with a `viewBox` but no width or height now takes
  the width of its containing block and the height its aspect ratio
  gives, as in Chrome. When the containing block also had a definite
  height the engine stretched the SVG to fill both, so a square icon in
  a 186 x 136 tile became 186 x 136 instead of 186 x 186 and its
  drawing shrank to the tile's height. Bing's carousel icons and the
  icons on Apple's product pages now size like Chrome's.
* Performance: a regular expression made of alternatives, such as
  `/<script|<style|<link/i`, now skips the positions where none of them
  can start. The search already jumped to the next possible first
  character of a pattern with a single start, but an alternation ran the
  matcher at every position of the string. jQuery tests that pattern
  against every string given to `.html()`, and on Speedometer 3.1
  TodoMVC-jQuery this search was about a quarter of the run: the suite
  is now 18% faster and runs 30% fewer instructions.
* Performance: finding the `Attr` object of an attribute no longer
  scans every `Attr` object the page holds. The engine keeps each one so
  the same object comes back for the same attribute, but kept them in
  one list, scanned to find one and scanned again to drop an element's
  objects when the element is freed. `document.importNode()` gives every
  imported attribute its new owner document, which creates its `Attr`
  object, so on TodoMVC-WebComponents the list grew with every item and
  each iteration was slower than the one before. The objects are now
  kept per owner element. On Speedometer 3.1, TodoMVC-WebComponents is
  29% faster over its 10 iterations and runs 20% fewer instructions.
* DOM: `getBoundingClientRect()` and `getClientRects()` on an inline
  element inside a transformed block now include that transform. The
  inline path walked the boxes adding up offsets and scroll positions
  but never applied an ancestor's `transform`, `translate`, `rotate` or
  `scale`, so a link in a `translateY(-50%)` header or a scaled card
  reported the place it would have without the transform. Each rect is
  now mapped through the transforms of the boxes above it, the same way
  block boxes are. Across the frozen sitebench snapshots 252 more
  elements match Chrome, 85 of them on WhatsApp.
* Text: a `white-space: nowrap` link, form field or button no longer
  wraps after an en dash, a soft hyphen, a `|` or a zero-width space.
  The engine marks such ranges with Pango's `allow_breaks=false`, which
  removes ordinary breaks but keeps those after dashes and word
  dividers, so "Pale Moon 3.6.29–3.6.32" split at the dash. CSS `nowrap`
  suppresses every soft wrap opportunity, so the text library now keeps
  only mandatory breaks in such a range. On Wikipedia's collapsed navbox
  rows the cells now take Chrome's widths: 162 more elements match on
  top of the table row group fix.
* Layout: `filter`, `backdrop-filter`, `contain: paint` (also `layout`,
  `strict` and `content`), `content-visibility`,
  `transform-style: preserve-3d`, `offset-path`, and `will-change` naming
  one of these or a transform property now make an element the
  containing block of its absolutely and fixed positioned descendants,
  as in Chrome. Only `transform` and `perspective` did, so a positioned
  child of a blurred wrapper was placed against an outer box and painted
  outside the blur, sharp. As in Chrome, `filter` does not do this on the
  root element, the transform and containment properties do not do it on
  a non-replaced inline box, and `will-change: position` does it for
  absolutely positioned boxes only.
* Performance: `String.prototype.split()` with a regular expression no
  longer calls `exec()` at every position of the string. The
  specification splits with a sticky copy of the pattern tried at each
  index, and each try read and wrote `lastIndex` and built a match
  array. When the copy is a plain `RegExp` with the built-in `exec()`,
  the engine now searches for the next match directly, with the same
  first-character skipping as other searches. The pieces, captures and
  legacy `RegExp.$1` values do not change. CodeMirror splits its
  document of about a million characters into lines this way: on
  Speedometer 3.1, Editor-CodeMirror is 38% faster and runs 30% fewer
  instructions.
* Layout: a border whose style is `none` because nothing set one now
  takes no space. Layout zeroed a side only when `border-style` was
  written out as `none` or `hidden`; with no style value at all, which
  is what the initial value, `initial` and `unset` leave, the width
  still counted. `border: unset` (which also sets the width to
  `medium`) therefore added 3 px on every side, and a bare
  `border-width: 6px` added 6 px, although nothing was drawn. On El País
  a 12-column grid reset this way lost 6 px and every card moved;
  across the frozen sitebench snapshots 249 more elements match Chrome,
  117 of them on eBay and 105 on El País.
* Layout: an absolutely positioned child of a flex container resolves a
  percentage width against the container's padding box. Block layout
  took every child of a row flex container for a flex item and used the
  container's content width as the base, so a `width: 100%` overlay in
  a flex container with 8 px padding came out 16 px narrow. An absolute
  child is not a flex item, and its containing block is the padding
  box. Across the frozen sitebench snapshots 67 more elements match
  Chrome, 38 of them in IMDb's poster overlays.
* Performance: a regular expression that starts with a character class,
  such as `/[ \t\r\n\u000c]+/g` or the one Handlebars tests every
  value against before escaping it, no longer tests all 256 byte values
  against the class on every `exec()`. The search skips positions that
  cannot start a match, and building that table cost far more than the
  search itself on short strings. A class without the `i` flag now sets
  its ranges directly, and a case-insensitive first letter adds itself
  and its other case. On Speedometer 3.1, Editor-TipTap is 15% faster
  and TodoMVC-jQuery 26% faster.
* Performance: canvas colors written as hex or as `rgb()`/`rgba()` with
  plain numbers are parsed once instead of on every use. Each `fill()`
  and `stroke()` parsed `fillStyle`, `strokeStyle` and `shadowColor`
  again with the full CSS color parser, and each assignment to them
  parsed the value and printed it back with `printf`. A small
  per-thread table now remembers the last such colors; named colors,
  `light-dark()`, `currentcolor` and other forms that can depend on the
  color scheme or the element are still parsed every time. Chart.js
  sets these colors before every bar and line: on Speedometer 3.1,
  Charts-chartjs is 29% faster and runs 19% fewer instructions per
  iteration.
* Performance: `String.prototype.replace()` with a global regular
  expression and a replacement string no longer calls `exec()` once per
  match. Each call read and wrote `lastIndex` and built a match array,
  which the replace loop then read back. When the regular expression is
  a plain `RegExp` with the built-in `exec()` and the replacement has no
  `$` pattern, the engine now runs the matcher in one loop and appends
  the pieces directly; `lastIndex` and the legacy `RegExp.$1` values end
  as before. ProseMirror collapses the whitespace of every text node
  this way: on Speedometer 3.1, Editor-TipTap is 13% faster.
* Layout: an inline-block is never narrower than its min-content.
  Its shrink-to-fit width was the smaller of its max-content and the
  available width, without the min-content floor, so an inline-block
  holding a `nowrap` flex row or fixed-width items wider than the line
  was squeezed to the line width while its content overflowed. ESPN's
  scores strip is a row of such inline-blocks; each league was cut to
  1,120 px and every league after it moved left. Across the frozen
  sitebench snapshots 171 more elements match Chrome, 108 on ESPN.
* Paint: an element with both `opacity` below 1 and a `mask-image` is
  drawn with its opacity. The painter composited the masked group
  through the mask alone, so `opacity: .5` with a fading mask drew the
  element at full strength. The mask is now multiplied by the opacity.
* Layout: a link around an absolutely positioned image stays inline.
  An inline element that held block-level media became a block, and
  the test for media ran before the test for `position`, which
  blockifies an absolute `<img>` or `<video>`. The link then broke the
  line it sat in and its rect covered the whole containing block. An
  out-of-flow child no longer makes its parent block-level, and an
  inline left with nothing in flow reports the zero-height rect of its
  phantom line, as Chrome does. Across the frozen sitebench snapshots
  94 more elements match Chrome, 75 of them in eBay's video cards.
* Paint: an image, canvas or other box without children that has
  `transform-style: preserve-3d` or `perspective` is drawn again. The
  painter made such a box the root of a 3D scene, drew only the
  background of a block root and then the scene's children as 3D
  planes, so a childless root drew nothing at all: Discord's floating
  magnifier and other `preserve-3d` images were missing. A box without
  children now paints as an ordinary (possibly 3D-transformed) box,
  since neither property changes how it looks.
* Performance: finding an element's `id`, and checking whether a node
  is a shadow root, no longer scans the attributes of elements that
  have neither. Tree walks ask every element for its `id` and every
  child whether it is a shadow root, and each answer compared the names
  of all the element's attributes. A `window` property that is not a
  global, such as Lit's `litNonce`, falls through to a walk of the
  whole document for an element with that `id` whenever the tree
  changed since the last lookup. Each element now keeps two flag bits,
  set when an attribute named `id` or the internal shadow-root marker
  is added and recomputed when one is removed, and the walks test them
  first. On Speedometer 3.1, TodoMVC-Lit-Complex-DOM is 13% faster and
  TodoMVC-JavaScript-ES6-Webpack-Complex-DOM 5% faster.
* Performance: text measurements are now cached for pages that set
  `tab-size` and for runs that hold an inline-block or image. The cache
  of measured text left out every layout with tab stops, which a
  numeric `tab-size` gives to all text on a page that sets it on the
  root, as AP News does, and every layout holding the placeholder of an
  inline atomic box. The key now records the tab stops and the
  placeholder rectangles, which are all a measurement reads of them. On
  a frozen copy of AP News every text measurement was a miss; now 82% of
  them are hits and the main-thread CPU to first paint drops by 6%.
* Paint: a `background-image: url(...)` on an inline element such as a
  link or a `<span>` is drawn. Only gradients and the backgrounds of
  form controls reached the inline painter, so the icon Wikipedia puts
  after every external link, and any other inline icon set as a
  background, was missing. The inline element now records its first
  image layer like a form control does, and every such image of a line
  box is fetched, not only the first one.
* Paint: a positioned inline-block, inline-flex or other atomic inline,
  or one that starts a stacking context, is painted with the positioned
  boxes of its stacking context, in its `z-index` order, as CSS 2
  Appendix E orders them. It was painted with the line's text, before
  every positioned box of the stacking context, so a `z-index: 2`
  inline-flex holding Naver's AI search icon was covered by a white
  `transform`ed pill that comes before it in the page, and the
  headlines of CNN's video cards were hidden under the card image.
* Paint: an element with `transform-style: preserve-3d` that starts a
  3D scene now keeps its own 3D transform. The painter applied that
  transform only when it was a flat 2D one, so a card with
  `transform: rotateY(40deg)` and no perspective above it was drawn
  unrotated, at full width, with its faces. The element's background is
  now drawn as a plane of the scene through its transform, and its
  children are drawn through it as well.
* Layout: grid line names of 24 characters or more now work. Each name
  was kept in a 24-byte buffer and a longer one was dropped, so an item
  placed on it (`grid-area: content-toggleable / content`) fell back to
  the wrong row. On the Guardian front page the content of every section
  started below the section title instead of beside it, so each section
  was 20 to 100 px too tall and the page 1,190 px too long. Names are
  now interned strings with no length limit.
* Performance: reading a name that is not a global no longer searches
  the whole document. A property missing on `window` falls through to
  its named properties, the elements with that `id`, and each such miss
  walked every element of the page. The engine's own setup code reads
  about 500 of these names on every load. The document's id index now
  records when it was last rebuilt from the tree, and any insertion,
  removal or `id` change makes it stale; while it is current, a miss is
  answered from the index alone. `getElementById()` and
  `querySelector('#id')` misses take the same path. Which names resolve
  does not change. On frozen copies of IMDb, CNN, AP News and The New
  York Times the main-thread CPU to first paint drops by 21%, 19%, 10%
  and 9%.
* Frames: a `MessagePort` inside an iframe that is itself inside an
  iframe now receives proper `message` events. The engine records each
  frame's Event interfaces when the frame starts, but a nested frame
  copied that record from its parent frame instead of from the page the
  frame's globals were cloned from, so the copies did not match its own
  objects. Its port events then had no `MessageEvent` prototype: `data`,
  `type` and `target` read as `undefined`. reCAPTCHA, which talks to its
  anchor frame over a port, threw "SyntaxError: unexpected token:
  'undefined'" eight to ten times on PayPal, ESPN and AliExpress.
* Layout: a float that follows text ending in `<br>` now starts on the
  line after the break. It was placed beside the text's last line, so
  phpBB topic lists (the freeciv forum) drew each "by author » date"
  line on top of the topic title.
* Performance: a relayout no longer restyles the whole page because a
  container query container changed size. Only elements whose style read
  a container (a matching `@container` rule or a `cq*` unit) are
  recomputed, plus descendants whose inherited values change. An empty
  container map now counts as no map, so a page that has `@container`
  rules but no containers keeps its styles too. In the same way, when a
  web font finishes loading, elements whose style used `ex`, `ch`, `cap`
  or `ic` are recomputed; before, an incremental restyle kept the
  lengths measured with the fallback font. On frozen copies of
  nytimes.com, apnews.com and weather.com the main-thread CPU of a load
  drops by 42%, 48% and 53%.
* Performance: a rule whose subject is `:is()` or `:where()` is no longer
  tested against every element when some of its arguments can never
  match, such as `.btn:-moz-focusring`. The rule index skips those
  arguments and files the rule under the others' class, id, tag or
  attribute, and drops it when none is left. CSS nesting wraps parent
  lists in `:is()`, so one vendor-prefixed parent selector put the whole
  rule in the list checked for every element: 74 such rules on
  nytimes.com doubled the selectors tried per element.
* Performance: computed values of custom properties registered with
  `@property` are remembered during a style pass when they do not
  depend on the element. A value with no font-relative, viewport,
  container or `currentcolor` reference, such as an initial value of
  `0px`, was parsed again for every element, and each element first
  measured its font for the `lh` unit. Pages built with Tailwind 4
  register dozens of such properties: the style time of a weather.com
  load drops by about 14%.
* Layout: the min-content and max-content widths of a grid container now
  come from its column tracks, as CSS Grid sizes them. The min-content
  width ignored every track unless all of them had a fixed size and fell
  back to the widest item, so a `430px 1fr` grid could shrink to its
  text, while an item with `min-width: 0` and long `nowrap` text forced
  its `1fr` track to the full text length. The max-content width put
  items in columns by document order and ignored spans and `fr`.
  Percentage tracks now count as `auto`, an `fr` track takes its items'
  minimum contribution, a fixed track keeps its size and spanning items
  share their size across their tracks. On Le Monde the lifestyle
  section was 2005 px wide instead of 615 px and ran off the right side
  of the page.
* Network: a `HEAD` request from `fetch()` or XMLHttpRequest no longer
  fails when the server sends a `Content-Length`. The method was passed
  to curl as a plain custom verb, so curl waited for a body that a HEAD
  response never has and reported "end of response with N bytes
  missing" (or "transfer closed with N bytes remaining"), and fetch
  rejected with that `TypeError`. LinkedIn checks its static files with
  HEAD requests and retried each failure, which logged about a thousand
  unhandled rejections on one load. HEAD now asks curl for headers only,
  and the response resolves with its status and headers and an empty
  body, as in Chrome.
* Shadow DOM: setting `textContent`, `innerHTML`, `innerText` or
  `setHTMLUnsafe()` on a shadow host, or calling `replaceChildren()` on
  it, no longer throws its shadow root away. The engine keeps the shadow
  root next to the host's children, and these "replace all children"
  paths removed it with them, so the component's shadow content vanished
  and a MutationObserver saw the `ShadowRoot` in `removedNodes`. Reddit's
  observer called `matches()` on it and failed on every load. Only the
  light children are replaced now, as in Chrome.
* Layout: an absolutely positioned grid or flex container with a
  percentage `height` now gives its rows and items that height. The box
  itself got the right height from its containing block's padding box,
  but its own layout resolved the percentage again against the parent's
  `height`. Under the common aspect-ratio pattern (`height: 0` and a
  percentage `padding-top` on the parent) that came out as 0, so a
  `1fr` row, a `flex: 1` item and every `height: 100%` below them
  collapsed to nothing. On Canva the image of every promotion card
  collapsed this way. The percentage now resolves against the same
  containing block during the box's own layout, as in Chrome.
* Audio: pages now hear back from the audio player. The helper process
  that decodes `<audio>` reported duration, position and the end of
  playback, but the window threw those reports away, so a page never got
  `canplaythrough`, `timeupdate` or `ended`. `load()` also never started
  fetching, so a page that calls `play()` from its `canplaythrough`
  handler (the click sounds on kabesalod.de) stayed silent. `load()` now
  opens the file in the helper, and its reports reach the page as the
  usual media events, including for `new Audio()` elements that are not
  in the document. FLAC and WAV files play too, and the LGPL FFmpeg
  build used for Windows releases includes their decoders.
* SVG: `<use href="sprite.svg#icon">` now draws symbols from an external
  SVG file. Only references inside the same document worked, so icon
  sets served as one sprite file (the navigation icons on midzer.de, the
  arrow on kabesalod.de) were left blank. The sprite is fetched once per
  page, parsed and kept, and the page repaints when it arrives; as in
  other browsers, only same-origin http(s) sprites are followed.
* Intl: `Intl.NumberFormat` now picks the plural form of a unit or
  currency name from the whole number, not from the digits shown before
  the exponent. With scientific, engineering or compact notation,
  `1E4 bit`, `1K byte` and `1E4 euro` became `1E4 bits`, `1K bytes` and
  `1E4 euros`, as in Chrome. The formatjs polyfill checks exactly this
  (`"1E4 bits"`) to decide whether the browser's `NumberFormat` is
  complete; it replaced ours, and a mismatched polyfill then threw
  "TypeError: not a function" eight times on each IMDb load. The compact
  pattern itself still follows the shown digits (`1 million` in French).
* WebGPU: a page can no longer abort the browser with a bad descriptor.
  wgpu-native panics, and the whole process aborts, when it gets a
  texture format it does not know, a missing shader module, a
  bind-group-layout entry without a binding type, a zero view or binding
  size, an "internal" error scope and a few other values. The binding
  passed such values through unchecked, so TikTok, whose GPU probe tries
  the misspelled format `etc2-rgb8-unorm`, aborted on 7 of 10 loads.
  Texture formats, view dimensions, aspects, binding types and vertex
  formats are now checked like Chrome does, and an unknown value throws a
  `TypeError`. The other cases throw a `TypeError` or are handled before
  they reach wgpu-native.
* Layout: an image or video now contributes its `max-width`/`min-width`
  limited width to the intrinsic size of the box around it. A flex item
  holding a `<video width="2304">` with `max-width: 100vw` was sized to
  2304 pixels and, centred by its flex container, pushed the whole page
  half off-screen to the left (midzer.de). An image whose `max-width` is
  a percentage now also contributes nothing to its container's
  min-content width when it sits inline, as CSS Sizing requires, so
  `1fr` grid columns of thumbnails shrink to fit instead of staying at
  the images' natural width (the kabesalod.de gallery).
* CSS animations run at the display's frame rate again. The window
  capped page animations at 60 frames a second by dropping any frame
  that came less than exactly 1/60 s after the last one, but a 60 Hz
  display's frame times jitter around that value, so about every other
  frame was dropped and an animation alternated between 60 and 30 frames
  a second (around 40 on average). Sliding marquees and other moving
  animations stuttered visibly. The cap now allows a quarter frame of
  slack, so a 60 Hz display gets every frame.
* Text: a paragraph without a `dir` attribute or `direction: rtl` now
  runs left to right, as CSS says. The direction was guessed from the
  first strong letter, so a line that began with Arabic, Hebrew or
  Syriac text (even inside a `<bdi>`) was laid out right to left and
  pushed against the right edge. On the Wikipedia portal every
  right-to-left language name in the language lists sat at the right
  side of its column. Such lines now start at the left edge as in
  Chrome; `unicode-bidi: plaintext` still takes the direction from the
  text.
* Text: a fallback font no longer moves or stretches a line by the wrong
  amount. With a fixed `line-height`, the line height was centred on the
  fallback font's own ascent and descent, and every run after the first
  was moved onto the baseline of the first run's script (for example the
  ideographic baseline after Chinese text). A list item with a Syriac or
  Chinese name came out up to 2 px taller than its 23.8 px line height,
  so the language lists on the Wikipedia portal broke into columns at
  the wrong entries. The line height is now laid out around the
  element's first available font and all horizontal text sits on the
  alphabetic baseline. With `line-height: normal`, a taller fallback
  font (emoji, Myanmar or Syriac text) still grows the line to fit its
  glyphs, as in Chrome.
* Layout: a box with `contain: layout` (or `content` or `strict`) now
  has no baseline when it is aligned in a line or in a flex row, as the
  CSS containment spec says. An inline-flex or inline-block with
  `contain: content` was aligned on the baseline of its text, so its
  line was only as tall as the box. Chrome puts the bottom edge of the
  box on the baseline instead, which leaves room below it for the
  line's descent. GitHub's footer links, which are such boxes, were
  5 px closer together than in Chrome.
* Layout: a `<body>` with `overflow-x: hidden` (or any overflow other
  than visible) no longer keeps its first child's top margin inside it
  when the `<html>` element's overflow is visible. CSS moves the body's
  overflow to the viewport in that case, so the body itself is not a
  new formatting context and the margins collapse, as in Chrome. The
  page sat lower by the body's own top margin before, 4 px on the
  Wikipedia portal.
* Debian packaging: following the mentors.debian.net review, debhelper
  compat 14, an `execute_after_dh_auto_install` hook that fails the build
  when a file it removes is missing, no redundant `Rules-Requires-Root`,
  and a single `Initial release` changelog entry. `docs/Debian-inclusion.md`
  tracks the road into Debian. The Debian source and binary package is
  now called `nordstjernen-browser`; the commands it installs keep their
  names.
* Crash fix: a streamed XMLHttpRequest response could run the page's
  `progress` and `readystatechange` handlers on the network thread while
  the page's own script ran on the main thread. Both threads then used
  the same JavaScript heap, and the browser crashed in the garbage
  collector (Al Jazeera crashed on 9 of 10 cold loads). Each received
  chunk is now always queued to the page's thread and handled there.
* Layout: positive `letter-spacing` no longer spaces an inline-block, an
  image or another atomic inline as if it were a letter. The line
  breaker counted that extra spacing, so a shrink-to-fit heading with
  spaced text and a trailing icon (the BBC section titles with their
  chevron) was a pixel too narrow and pushed the icon onto a second
  line. Positive letter spacing now applies to the text only, as in
  Chrome.
* Layout: `line-height: 0` is now honoured. It was treated like an unset
  value, so a block holding only an image or an inline-block still got a
  line of about 1.2 times its font size, and the image card wrappers
  that use it (BBC, Washington Post) came out 4 to 5 pixels taller
  than the image. A line also always reached down to its
  baseline, so text with a line height much smaller than its font made a
  line a few pixels tall instead of the height the CSS asked for. Such
  lines now take exactly their line height, matching Chrome.
* Safe browsing: redirects and form submissions are now checked against the
  blocklist. Only a plain link or typed address was, so a link to a clean
  URL that redirected to a blocked site, or a form posting to one, loaded
  the blocked page without a warning. Each redirect hop of a top-level
  navigation is checked before it is fetched, and a POST navigation before
  its data is sent; a match shows the usual warning page.
* Security: a Content Security Policy without `'unsafe-eval'` now stops
  `eval()`, `new Function()` (and the async and generator variants) and
  `setTimeout`/`setInterval` with a string. The keyword was parsed but
  never enforced, so a page that relied on CSP to keep an injected string
  from becoming code got no protection. They now fail with an
  `EvalError`, or the string timer does not run, and the console logs
  "CSP blocked: eval". WebAssembly compiles only when the policy allows
  `'wasm-unsafe-eval'` or `'unsafe-eval'`. An `about:blank` frame follows
  its parent's policy; a frame that loaded its own document is unaffected.
* Security: `<link rel=stylesheet integrity=…>` is now verified. Only
  scripts were checked, so a stylesheet whose bytes did not match its
  Subresource Integrity hash (for example one altered on a compromised
  CDN) was applied anyway, although SECURITY.md promised the check. A
  mismatched stylesheet is now ignored and logged. An empty resource
  whose hash does match is accepted, where before any empty body failed.
* Web Audio: a looping `AudioBufferSourceNode` with an enormous
  `playbackRate` could push its read position to infinity during
  real-time rendering. The position then became NaN and was used as an
  array index, which is undefined behaviour. Such a source now stops
  instead.
* WebVTT: a cue timestamp with a very long fractional part, such as
  `00:01.` followed by thousands of digits, overflowed a signed integer
  while it was parsed. Only the first three digits are read now, which is
  all the millisecond value ever used.
* HSTS: a host's HTTPS-only policy no longer outlives its `max-age`. The
  engine's own HSTS check ignored the expiry date in the HSTS file, so a
  site was forced onto HTTPS, and the certificate-warning override refused
  for it, forever once it had sent `Strict-Transport-Security`. A site
  that later dropped HTTPS stayed unreachable, and the entry kept the
  visit recorded long after the site asked for it to end. Expired entries
  are now ignored, both when the file is read and for entries noted while
  the browser runs.
* Security: a cross-site iframe read and wrote its own site's first-party
  cookies through `document.cookie`. A tracker embedded on many sites could
  therefore follow the user from site to site, even though third-party
  cookies are blocked by default. The frame's cookies were looked up by the
  frame's own site, never by the page it was embedded in. A cross-site
  frame now gets no cookies under the default first-party policy, and
  under `cookie_policy = always` it gets a jar partitioned by the top-level
  site, as its network requests already did.
* Security: with the nghttp2 HTTP backend (`-Dhttp_backend=nghttp2`) and
  `cookie_policy = always`, third-party requests read and stored cookies
  in the request site's own first-party jar instead of the top-level
  site's partition. A cookie set on a third-party image was then sent with
  first-party visits to that site. The backend now uses the partition jar
  the request was given, as the curl backend does.
* Security: compiled JavaScript was cached by its source text alone. A
  page could therefore serve an exact copy of a script another site uses,
  from its own URL. When the other site later loaded its copy, the browser
  reused the cached bytecode, which still named the first URL, and the
  script's dynamic `import()` loaded modules from the attacker's site into
  the victim's page. The same reuse also put the first URL into
  `error.stack`. The cache key now covers the script's URL, position and
  compile flags as well as its source.
  The cache is also no longer written to disk. Every sandboxed renderer can
  write the cache directory, and QuickJS runs bytecode without verifying
  it, so a compromised renderer could plant code for renderers showing
  other sites to run. Each renderer now keeps its cache in memory.
* README brought in line with the source after a full review. It names
  1.0.35 as the current release and counts about 254,000 lines of engine C.
  It now lists the CSS, JavaScript, networking, privacy, UI and extension
  features the engine has, including Service Workers, Streams, Temporal,
  HTTPS-First, Developer Tools, the Task Manager and filter-list content
  blocking. Claims that were wrong are corrected:
  - the headless screenshot option is `--dump=png:FILE`, not `--screenshot=`;
  - Wuffs also decodes simple lossy WebP;
  - `<audio>` does not play AAC files;
  - WebGL is desktop-only, and WebGL 1 only on macOS;
  - WebAssembly covers the MVP plus reference types and bulk memory, not the
    full JS API;
  - 13 of the 40 UI catalogues cover more than the browser's name;
  - "session restore" is crash recovery;
  - the FreeBSD and NetBSD builds have no renderer sandbox;
  - Windows needs 10 version 1809 on x64.
  The README now also lists iOS as an experimental port, and FriBidi, zlib
  and libwebpdemux as required libraries.

1.0.35:
======
* nordstjernen --version prints the version and --help lists the main
  options; both exit without starting the browser. Before, either one
  opened a browser window.
* Linux: the Landlock sandbox lets GTK write its renderer cache in
  ~/.cache/gtk-4.0, so the Vulkan pipeline cache is saved between runs
  instead of failing with "Permission denied" and being rebuilt on every
  start.
* Debian package: built with -D_FORTIFY_SOURCE=3 as meson.build asks.
  dpkg's hardening flags appended -D_FORTIFY_SOURCE=2, which overrode it
  and printed a "_FORTIFY_SOURCE redefined" warning for every file.
  debian/changelog now carries 1.0.35~dev-1 to match the source version.
* The Debian / Ubuntu build instructions use apt build-dep, which installs
  the complete debian/control list. The hand-written package list missed
  the AVIF, JPEG XL, GStreamer, spell-check and PDF libraries, so a build
  that followed it silently left those features out.
* about:nordstjernen no longer shows the animated cartoon splash. The
  1.7 MB embedded GIF (src/about_splash_gif.h), its source images in
  data/ and the scripts/gen-splash.* generator are gone, which shrinks
  the binary and the repository.
* Text layout: the ns-pango Pango fork is now vendored in-tree as
  web-text-lib (src/web-text-lib/) and built with subdir() like lexbor and
  QuickJS, instead of being cloned from git at setup time. Offline and
  distribution builds now shape text through it too. The meson option is
  renamed -Dweb-text-lib (was -Dns-pango). Its sources sit directly in
  src/web-text-lib/ and are included as <web-text-lib/...>. The library
  is trimmed to what the browser calls: about 450 unreachable functions,
  13 files and the generated enum types are gone (some 12,800 lines), and
  the build no longer needs glib-mkenums.
* The repository is renamed to nordstjernen-web/nordstjernen-sol-browser.
  Every link to it follows: the README badges and release links, the
  AppStream metainfo, the Debian, RPM, OBS and Alpine packaging, the Java
  POM and Gradle metadata, and the build documentation. The Alpine recipe
  unpacks from the new archive root, with a checksum to match.
* The application is now called Nordstjernen Sol: the window and task
  manager titles, the title-bar name, about:nordstjernen, the Windows
  message boxes and installer display name, the Linux desktop entry and
  AppStream name, the Android label, the macOS and iOS display names and
  the Java shell, and the about:start heading, which now reads
  "Nordstjernen Sol Browser". Every UI catalogue carries the translated name with
  "Sol" appended. Package identifiers, install paths, the profile
  directory and the user agent are unchanged.
* Security: a review of the engine, its QuickJS fork and its lexbor fork
  for crashes and double frees fixed twenty-five bugs that a page, or an
  ordinary action in the shell, could trigger. Each commit carries a
  `Security-Fix:` trailer.
* HTML parsing: a named character reference whose value is longer than
  its name (`&nGt;`, `&nLt;`) ending exactly at the end of lexbor's
  16 KiB tokenizer buffer was written past the buffer. The bug is also
  in upstream lexbor.
* DOM: reading `element.attributes` followed a freed attribute when page
  script run during the walk removed it; the custom element upgrade walk
  read a registry slot that `customElements.define()` from a class
  getter had freed; `navigation.entries()` read a freed history entry
  when an `Object.prototype` setter called `history.pushState()`;
  image-load handling ran an `Object.prototype` getter that could free
  the load record still in use; `select.add(option)` left the option
  registered as an orphan node while the select owned it.
* DOM: `getAttributeNames()` and `querySelectorAll()` (on its class, tag
  and key index paths) stored results into the returned array while
  still walking the attribute list or index bucket, so a setter the page
  defined on `Array.prototype` could remove the attribute or call
  `document.open()` and free what the walk was reading; reading an SVG
  length such as `rect.width` let an `Object.prototype` setter free the
  attribute value it was still using.
* DOM: the `document.body` setter could append a node under its own
  descendant, linking a cycle into the tree that overflowed the stack on
  the next tree walk; a MessagePort whose forwarding target pointed back
  at itself recursed until the stack overflowed; SVG path data with a
  number after closepath (`M0 0Z1`) made `getTotalLength()` loop until
  memory ran out.
* JavaScript: `ShadowRealm.prototype.importValue` dereferenced a
  primitive as an object when the module's exported `then` resolved
  with one; Temporal date arithmetic past calendar year ±300000 hung the
  renderer instead of throwing `RangeError`; Temporal objects were
  declared 16-byte aligned on QuickJS's 8-byte allocator; Intl number
  formatting freed the same memory twice when an allocation failed.
* Rendering: painting SVG path data with a number after closepath grew
  the path until memory ran out; a page nested about 100k elements deep
  overflowed the stack when it later added a stylesheet or defined a
  custom element, and so did a slotted element under a shadow root
  nested about 170k levels deep.
* WebAssembly: calling an export with an `externref` argument followed
  by a numeric one whose `valueOf` made a nested call could free the
  externref's box before the outer call used it.
* Media: an MP4 box using the 64-bit size form in a buffer shorter than
  16 bytes was read past its end.
* Task Manager: "End task" used the view of a tab that had closed since
  the last refresh, and the window outlived the browser window that
  opened it.
* Keyboard: pages now receive `keydown` and `keypress` for letters,
  digits, punctuation and Space. The input method consumed those keys
  before the page saw them, so only `keyup` and the typed text arrived,
  and WebAssembly and canvas games that read the keyboard did not
  respond (#465). Text still reaches fields through the input method,
  a cancelled `keydown` or `keypress` now stops the character being
  inserted, and Space scrolls the page when no field has focus.
  `code` and `keyCode` now cover modifiers, function keys, the numeric
  keypad and punctuation.
* Idle CPU: a tab no longer polls its renderer on every display frame
  while it waits for something with a known time. The renderer reports
  when it next needs to run, from the next animated-image frame, the
  next JavaScript timer or a pending `<meta http-equiv=refresh>`, and
  the window sleeps until then; a page with a one-second
  `setInterval` now wakes about three times a second instead of sixty
  (#464). Pages that draw every frame still get every frame.
* Images: animated GIF, APNG, WebP and JPEG XL images play as many times
  as the file's loop count asks for and then rest on their last frame,
  as other browsers do; they looped forever before. The start page logo
  now plays its shooting star twice, so an idle new tab uses no CPU.
* Text fields: the caret stops blinking after ten seconds without
  moving and stays visible, as GTK's own fields do, instead of waking
  the browser twice a second for as long as a field has focus.
* Reload: pages appear sooner after a reload or any navigation. The
  first frame no longer waits out the whole 400 ms settle window for a
  JavaScript timer that cannot fire inside it, a `requestAnimationFrame`
  loop or a CSS animation; a 90 KB page with a five-second
  `setInterval` now opens in about 120 ms instead of 480 ms.
* Reload: the page is fetched again with `Cache-Control: max-age=0`
  and revalidated instead of being shown from a still-fresh cache
  entry, so it shows the server's current content as in other
  browsers. The replaced page is discarded rather than kept in the
  back-forward cache.
* Reload: a renderer stuck on one request for over a second (a page
  spinning in a script, or a load that never finishes) is restarted
  when you press Reload, so the page always comes back. Navigating
  drops queued work for the page being left, a load superseded before
  it starts is skipped, and Reload retries a page that is still loading
  or failed to load instead of the previous history entry.
* Mouse: the middle and right buttons now reach pages. Pages only got
  the left button's `mousedown`/`mouseup`/`click`; a right click sent
  just `contextmenu` and a middle click or `Ctrl`+click sent nothing, so
  games and drag tools that use other buttons did not respond. Every
  button now fires `pointerdown`/`mousedown`, `pointerup`/`mouseup` and
  `click` or `auxclick` with the right `button` and `buttons`, and
  `contextmenu` follows the right button's `mousedown`. `Ctrl`+click and
  middle click open a link in a new tab through the normal activation,
  so a page that cancels the click keeps the tab from opening.
* Mouse: `detail` carries the click count, a double click fires
  `dblclick`, and `mousemove` carries the modifier keys and every held
  button. A `mousemove` handler that changes the page is drawn at once;
  it used to wait for the next unrelated repaint.
* Pointer events: `setPointerCapture()` sends the pointer's later events
  to the capturing element, so a drag that leaves the element keeps
  moving it and its `pointerup` reaches it, followed by
  `lostpointercapture`. Pointer lock sends mouse events to the locked
  element.
* Text selection: a selection is no longer lost whenever the page
  changes its content, such as a ticking clock or a click handler that
  updates a counter.
* `internet-browser` is a second name for the browser. `meson install`
  links it to `nordstjernen` in the binary directory; the `.deb` and `.rpm`
  packages register it with `update-alternatives`, so Nordstjernen installs
  beside Northstar and Southstar and `update-alternatives --config
  internet-browser` picks which one the name starts; the Alpine package
  creates the link when the name is free; the Windows installer adds it
  as an App Paths alias and the MSIX package as an app execution alias.

1.0.34:
======
* Linux: WebGL and WebGPU render on the GPU again inside the sandboxed
  renderer. Its Landlock ruleset let it read `/dev/dri` but not open the
  render nodes read-write, so Mesa printed "Permission denied" for every
  `/dev/dri` node and fell back to software rendering. The renderer now
  opens `/dev/dri` and `/dev/nvidia*` read-write while WebGL or WebGPU is
  enabled; with both turned off in Settings it still gets no access.
* Linux: the browser starts again on NVIDIA's proprietary driver under
  X11. The UI shell's Landlock sandbox let it read `/dev/dri` but not open
  the render nodes read-write, and gave no access to `/dev/nvidia*` at all,
  so GTK's OpenGL and Vulkan drivers were refused the GPU ("Permission
  denied" on `/dev/dri/renderD128`) and the window died with an X
  `BadDrawable` error; the watchdog restarted it until it gave up. The
  shell now opens those device nodes; the renderer's sandbox is unchanged.
  (#463)

1.0.33:
======
* Alpine packaging: the APKBUILD, the musl CI job and the nightly Alpine
  build now install libjxl, GStreamer, enchant2, libvorbis, opusfile,
  brotli and freetype, so the Alpine package carries JPEG XL, H.264/AAC
  playback, spell-checking and WOFF2 like the other Linux builds. The
  nightly `.apk` derives its runtime `depends=` from the libraries the
  binaries actually load instead of a hand-kept list that had fallen
  behind.
* WebGL performance: a canvas that redraws every frame (a map being
  panned, a game, an animation) is read back asynchronously through
  pixel-buffer objects on desktop OpenGL, so painting no longer waits for
  the GPU to finish each frame; the page shows the previous completed
  frame, as compositors in other browsers do, and the last frame is
  always shown once drawing stops. Painting a full-window WebGL canvas
  that redraws each frame went from about 11.6ms to 3.2ms.
* WebGL: on desktop OpenGL (Windows, macOS) a frame is flipped and
  converted to the page painter's pixel format on the GPU and read
  straight into the canvas surface, instead of read as RGBA and
  flipped, swizzled and alpha-forced per pixel on the CPU. Translucent
  pixels of a `premultipliedAlpha` context (the default) are no longer
  premultiplied a second time, so a half-transparent WebGL background
  over a page now blends as in Chrome instead of coming out darker.
* Web Animations: `element.animate(keyframes, { pseudoElement:
  "::after" })` no longer animates the element itself. Animations of
  pseudo-elements are not rendered yet; such an animation now reports
  its `pseudoElement`, leaves the host untouched and fires `finish` when
  its duration ends. Google's Material buttons (Calendar, Gmail)
  animate their ripple on `::after`; the whole button layer was scaled
  up about twelve times and a large grey disc stayed on screen after
  every click.
* Scrolling: a row flex or grid container with `overflow: auto` or
  `scroll` scrolls when its items are taller than it, and an element
  with `overflow: hidden` accepts `scrollTop`/`scrollLeft` from script,
  as in Chrome (it still shows no scrollbar and ignores the wheel).
  Google Calendar's week view scrolls the hour grid in a row flex
  container and keeps the hour labels in step by setting `scrollTop` on
  an `overflow: hidden` column; the grid could not be scrolled past
  16:00 and the labels did not follow it.
* Remote control over MCP: `nordstjernen --mcp` is a built-in Model
  Context Protocol server (stdio, protocol 2025-06-18), so an agent such
  as Claude Code can drive the browser window you have open, much as
  Claude in Chrome drives Chrome (`claude mcp add nordstjernen --
  <path>/nordstjernen --mcp`). Its tools list, open, select and close
  tabs, navigate, read the page as an accessibility-style outline with
  element refs, find elements, click, double-click, hover, type, press
  keys, scroll, fill form fields, take viewport screenshots, run
  JavaScript and return page text. Input goes through the same path as
  the mouse and keyboard. The browser only accepts control when
  Settings → Privacy → "Allow remote control" is on or it was started
  with `--remote-control`: it then listens on 127.0.0.1 alone and every
  request must present a random per-session token kept in a user-only
  file. A "Controlled by …" toolbar badge shows while a client is
  connected; clicking it stops remote control. See
  `docs/remote-control.md`.
* Forms: an absolutely or fixed positioned `<input>` or `<textarea>`
  shows its value or placeholder and is one line tall, as in Chrome.
  The inline collector took the control for an out-of-flow child of
  itself, so the box held only a static-position placeholder and was
  0px tall. Gmail's search field is such an input and showed no "Search
  mail" placeholder.
* Paint performance: a subtree under `opacity: 0` is no longer painted
  into an offscreen group only to be composited fully transparent, and
  the group for a translucent box is clipped to the box's ink bounds
  instead of the whole visible area. Painting a Gmail screen took
  300-400ms and now takes about 100ms; Gmail has hundreds of icons at
  `opacity: 0.32`-`0.54` and many invisible hover layers, and each
  allocated and composited a viewport-sized surface.
* XMLHttpRequest no longer adds an `X-Requested-With: XMLHttpRequest`
  header to every request. Chrome never sends it (libraries such as
  jQuery add it themselves for same-origin requests), and servers that
  key on it answered our requests differently.
* CSS: a `position: fixed` box whose nearest positioned ancestor sits
  inside an `overflow: hidden` box is painted in the stacking context of
  the closest positioned ancestor that can hold it, instead of at the
  root under every `z-index`ed layer. Gmail's compose toolbar and Send
  button are such a fixed bar inside the `z-index: 3` compose layer and
  disappeared behind the message window.
* CSS: an element with `visibility: visible` inside a `visibility:
  hidden` ancestor is painted, as in Chrome. The painter dropped a hidden
  box's whole subtree; it now skips only the hidden box's own
  background, border and text. Gmail keeps its compose layer hidden and
  makes the open message window visible, so a new message showed no
  window, header, To or Subject field.
* CSS tables: a `table-layout: fixed` table with a percentage width has
  an unbounded max-content width, as in Chrome, so the float,
  inline-block, flex item, grid item or table cell holding it fills the
  space available instead of shrinking to its contents. Gmail's opened
  message is a flex item holding such a table; it was about half the
  width of the reading pane, with the time and reply buttons stranded
  in the middle.
* CSS tables: a table row's children that are not cells, such as a
  `<td>` restyled `display: flex` or loose text, are wrapped in an
  anonymous cell instead of being dropped. Gmail's message header puts
  the sender in a `display: flex` `<td>`, so an opened message showed no
  sender name, address or "to me" line.
* Web fonts: a face's optical size no longer takes part in choosing
  between the faces of an `@font-face` family, since CSS font matching
  never considers it. Google Sans's upright faces are a variable font
  whose optical-size axis spans 17-18pt, so fontconfig preferred the
  static italic face for any smaller text, and Gmail and other Google
  pages were drawn entirely in italics. An italic face that arrives
  before the family's upright faces also waits for them, so normal text
  uses the fallback font meanwhile, as in Chrome, instead of the italic.
* DOM: `table.rows`, a table section's `rows`, `table.tBodies` and
  `tr.cells` are live `HTMLCollection`s with `item()` and `namedItem()`,
  as in Chrome, instead of plain arrays copied when read, and
  `map.areas` exists. Gmail looks a clicked message up with
  `tbody.rows.item(i)`; the call threw "not a function", so clicking a
  message in the inbox did not open it.
* XMLHttpRequest: an asynchronous request now delivers its response as
  it arrives, as in Chrome: `readyState` 3, a growing `responseText` and
  `progress` events follow each chunk instead of all firing when the
  response ends. Streaming requests are no longer cut off after 30
  seconds, and `abort()` now cancels the network request. Gmail's chat
  and calling channel (`signaler-pa.clients6.google.com`) streams on a
  hanging GET; it saw no data, aborted after seven seconds and
  reconnected forever, and the inbox showed "A system problem might not
  let you make or get calls here".
* Floats: a line of `white-space: nowrap` or `pre` text stays beside a
  float and overflows, as in Chrome, instead of dropping below the
  float when less than about 120px is left beside it. Gmail's inbox
  floats the " - " before each message snippet, and a row whose
  subject left the snippet little room grew to two lines.
* CSS tables: a `<table>`, `<tr>`, `<td>` or `<th>` whose `display` is
  changed by CSS is laid out with that display, as in Chrome, instead of
  as a table part by its tag. A row group's children that are not rows
  (say a `<tr>` with `display: flex`) are wrapped in an anonymous row
  and cell rather than dropped. Gmail styles its inbox table
  `display: block` and each message row `display: flex` with 10px
  vertical padding; the rows were 20px tall and cramped, and are now
  Chrome's 40px.
* Build: `-Dquickjs=quickjs` builds Fabrice Bellard's QuickJS from its
  `master` branch (84e723f) instead of the 2026-06-04 release, so that
  engine gains `Array.fromAsync`, `Iterator.zip`, iterator `join`,
  `includes` and chunking, `Promise.allKeyed`, and the fixes since the
  release. Its experimental Temporal and Intl are left out.
* The in-tree HTML parser, lexbor, is refreshed to upstream master
  (f4cbbcd). The object arrays its tokenizer and tree builder keep were
  allocated eight times larger than needed on 64-bit systems, and removing
  entries moved eight times too many bytes; both now use the exact size.
* JavaScript: QuickJS is at quickjs-ng master 5271a40, the latest
  upstream commit. It changes only upstream's CI runners, so the engine
  itself is the same as at 9b06171.
* Build: the GCC build of `src/webgl.c` no longer warns about a null
  pointer dereference in `wgl_elem_shadow_load`, which read the
  context's WebGL version before checking that the element buffer had
  a size.
* CSS: a `calc()` with `anchor()` and three or more terms, where a
  later term cannot be read as part of the anchor expression, no longer
  aborts the browser. The anchor value parser freed the first term
  twice when it gave up, so a page like GitHub with
  `@position-try --f { bottom: calc(100vh - anchor(top) +
  var(--b, .25rem)) }` died as soon as a script read
  `document.styleSheets[0].cssRules`. The parser now drops the
  expression once, the rule shows up in `cssRules` as in Chrome, and
  an invalid value such as `calc(1px + anchor(top) + 2)` in a longhand
  such as `top` is simply rejected.
* Layout: an HTML comment, collapsible white space, a `display: none`
  element or a `<script>`/`<style>` between two `display: table-cell`
  siblings no longer splits the anonymous table that wraps them. Such a
  gap ended the table, or the anonymous row inside it, so the second
  cell started a new table or row below the first: a cookie banner with
  an icon cell, `<!-- -->` and a text cell grew from 80 to 133 pixels
  because its text dropped under the icon. These nodes generate no box,
  so the cells now stay side by side in one row, as they do in Chrome.
  White space that the parent keeps (`white-space: pre`, `pre-wrap`,
  `pre-line`, `break-spaces` or a `<pre>`) is real text: it now ends the
  anonymous table and takes its own line, as in Chrome, where it used to
  be swallowed into the table.
* Flexbox: a column flex item with a percentage or fixed height now
  shrinks to make room for its siblings when its content only fills it
  through percentage heights. The item's automatic minimum height was
  measured with its children's `height: 100%` resolved against the
  item's own height, so the minimum equalled that height and the item
  never shrank: on Trendyol a product image wrapper with `height: 100%`
  pushed the promotion strip below the image and the card's text block
  grew 300px taller than its content. An item that will shrink is now
  laid out first as if its height were auto, as Chrome does, and then
  once at its flexed height, which a flex or grid item now also uses for
  its own content (a `1fr` row or a stretched child ends with the item
  instead of 30px below it). The automatic minimum is capped by
  `max-height`, a percentage `max-height` or `min-height` limits the
  height children resolve percentages against, an `aspect-ratio` column
  container grows to fit its items, and a row flex container with a
  definite height lays out a stretched item once instead of twice, so an
  app shell of nested `height: 100%` rows and columns no longer takes
  seconds to lay out.
* Layout: percentage padding and margins on an item of a row flex
  container resolve against the container's width, as for any other
  box. The item's own width was used instead, so `padding-left: 5%` on
  an item in a 1000px flex row came out 7.58px instead of 50px, and
  locale switchers and cookie banners sat too tight. A table with a set
  width that is a flex item now takes the width the flex layout gives
  it: `width: 10%; flex: 1` in a 600px row grows to 500px instead of
  staying 50px.
* SVG: an SVG used as an image applies its own `<style>` sheets and
  draws `<pattern>` fills and `<image>` elements. A logo loaded through
  `<img>` or `background-image` whose colours come from class rules such
  as `.c{fill:#ff4500}` drew every shape black, and reddit's robot
  avatar (a rect filled with a pattern that shows an embedded PNG) was
  blank. The sheets cascade with specificity, `@layer`, `!important`
  and the `style` attribute; `@media`, `vw`, `em` and `rem` follow the
  image's own size and fonts, and nothing in them reaches the page. The
  image is drawn where its CSS animations settle, so a logo that fades
  in with `animation: show .3s forwards` shows; links and `:hover` never
  match inside it, and a pattern tile shrunk below a pixel is filled
  with its average colour. CSS also accepts the plain numbers SVG
  allows, in pages too: `fill-opacity: .5` and `stroke-width: 2` were
  dropped as invalid.
* CSS: `-webkit-text-stroke` outlines text. The shorthand and its
  `-webkit-text-stroke-width` and `-webkit-text-stroke-color` longhands
  were parsed but never painted, so outlined headings showed as plain
  filled text and `color: transparent` outline text vanished. Inline
  text now paints the stroke over its fill, centred on the glyph
  outlines, per element of the line, on relatively positioned inlines
  and on lines raised by inline blocks. `::before` and `::after` text
  takes its own stroke rather than its host's, and colour emoji are
  left unstroked as in Chrome. `thin`, `medium` and `thick` scale with
  the font size as in Chromium (1/48, 3/48 and 5/48 em) instead of
  being fixed at 1, 3 and 5px, `element.style` keeps them as keywords
  in lower case (as it now does for the keywords of
  `text-underline-offset`, `text-decoration-thickness` and
  `baseline-shift`), and the width animates in one step at the halfway
  point, as in Chrome. A `currentcolor` stroke (likewise
  `text-emphasis-color` and `-webkit-tap-highlight-color`) inherited by
  a child follows the child's own colour instead of its parent's, and
  transitions and animations to or from `currentcolor` blend through the
  element's colour instead of jumping to the end value.
  `getComputedStyle()` reads an unset `-webkit-text-fill-color` as the
  text colour instead of an empty string, and on `::before` and
  `::after` reports colours that are `currentcolor` (borders, outline,
  caret, fill and stroke) as the pseudo-element's own colour instead of
  an empty string. Netflix's "Top 10" numbers, grey digits with
  `-webkit-text-stroke: 0.25rem white` under a black copy, now show
  their white outline.
* Forms: a text field without a CSS width is as wide as in Chrome, and a
  drop-down `<select>` is as tall. Text inputs, selects and buttons now
  use Arial at 13.33px like Chrome's form-control font instead of the
  system UI font, and the default width follows the HTML rule for `size`
  (20 by default): size times the font's average character width plus
  the difference to its maximum character width, measured on the field's
  own font, instead of a fixed 0.75em per character. The same width is
  now the field's min-content and max-content size, so a field in a flex
  row, an `inline-flex` search bar, a grid `auto` column, a float or an
  absolutely positioned box no longer shrinks to its padding or grows
  with its value. A drop-down select's width from its widest option
  counts the same way, so in a flex row or a narrow box it keeps that
  width instead of shrinking under its own arrow (65px before, 190px
  now, Chrome 187). The options are measured once per layout with the
  select's own letter- and word-spacing, and the box is never narrower
  than the label it draws. As in Chrome, a select no longer inherits
  `letter-spacing`, `word-spacing` or `line-height` from the page. A
  drop-down select takes its height from its font, ascent plus descent
  plus 1px inside above and below, instead of from the line box of its
  label and arrow glyph, which came out 1px short with Arial; it ignores
  `line-height` like Chrome's drop-down, and with `appearance: none` it
  drops the inner 1px and follows `line-height`. Its text sits on
  Chrome's baseline. A bare `<input>` was 208px wide and is 153px now,
  and a default select is 19px tall instead of 18.7px, or 33.7px with
  `line-height: 30px`.
* Layout: an HTML comment, a `display: none` element, a float or an
  absolutely positioned element at the start of a block or table cell
  no longer pushes the text after it onto a new line. The comment ended
  the inline content opened by the element's `::before`, and a float
  left the `::before` text or an inside list marker on a line of its
  own, so `<div class="tag"><!-- x -->text</div>` with
  `.tag::before { content: "#" }` drew the `#` and the text on two
  lines, and `<summary><!----><span style="float: right"></span>Title
  </summary>` put the disclosure triangle above the title and made the
  box twice as tall. Such nodes now continue the line like they already
  did in the middle of a paragraph: the `#` and the text share one line
  as in Chrome, the triangle sits before the title beside the float,
  and the summary no longer loses its marker when a float comes first.
* Elements and documents carry the `onfullscreenchange` and
  `onfullscreenerror` handler attributes, so players that check for them
  before listening react to fullscreen; a YouTube video now fills the
  screen in fullscreen instead of staying at its page size.
* An inline-block or button with `margin: auto` no longer stretches the
  shrink-to-fit box around it to full width; cookie banners built with
  OneTrust show their close button in the right place again.
* A drop-down `<select>` is as wide as its widest option, as in other
  browsers, and its arrow sits at the right edge (and is not drawn with
  `appearance: none`).
* `getClientRects()` on an inline element that wraps returns one
  rectangle per line, as in other browsers, instead of a single bounding
  box.
* `text-wrap: balance` balances the lines of headings and paragraphs
  (up to six lines) instead of breaking them greedily.
* Images and inline-blocks in a line of text sit on the baseline with
  the font's descent below them, as in other browsers, so lines with
  images are no longer a few pixels shorter than in Chrome or Firefox.
* `em` and `rem` inside `min()`, `max()` and `clamp()` resolve against
  the element's real font size and the root's, as in other browsers,
  not against 16px. AP News sets the root to 62.5% and sizes its footer
  links `clamp(1rem, 1.4rem, 3.4rem)`; they came out 22.4px instead of
  14px and pushed the footer 250px down. The frozen sitebench pages gain
  142 matching elements (apnews.com). A `min()` or `clamp()` nested in a
  `calc()` still uses 16px for now.
* A flex row's max-content width counts its `gap` when the gap is a
  `calc()` or comes from a custom property, as it already did for a
  plain length. Booking.com's property cards size their price line,
  "2 nights €820 €684", to fit its content with a gap of
  `calc(var(--bui_stack_spaced_gap--s) * var(--bui_spacing_1x))`. The
  line came out 8px too narrow and wrapped until the current price fell
  out of the card. The frozen sitebench pages gain 382 matching
  elements (booking.com +287, facebook.com +55).
* `getBoundingClientRect()` and `getClientRects()` on a `<tbody>`,
  `<thead>` or `<tfoot>` return the area of its cells, as in other
  browsers, instead of an empty rectangle at 0,0. Row groups get no
  layout box of their own, so scripts that measure a table's body to
  stick its header or size a scroller saw nothing there. The frozen
  sitebench pages gain 212 matching elements (news.ycombinator.com
  +100, Wikipedia articles +91).
* A table row's background is painted only under its cells, as CSS
  says, and the row's box no longer covers the table's outer
  `border-spacing`. With the default 2px spacing, or any
  `cellspacing`, a striped or highlighted row filled the gaps between
  its cells and ran over the table's border, and `getBoundingClientRect()`
  on a `<tr>` was the spacing too wide.
* List bullets and numbers sit on the baseline of the first line of
  their item instead of floating above it when line-height is large.
* An absolutely positioned `::after` without offsets appears right after
  the text of its element, as in other browsers, instead of at the start
  of the line (the "/" separators in theverge.com's navigation).
* `@font-face` descriptors read back through CSSOM are validated and
  normalized like in other browsers (ranges, `calc()`, `oblique 0deg`).
* `<blink>` blinks: its contents are shown for half of each second and
  hidden for the other half, as in the browsers that introduced the
  element. It is a built-in CSS animation (`animation: -nd-blink 1s
  step-end infinite` toggling `visibility`), so a page can stop it with
  `blink { animation: none }`, and it stops under reduced motion like
  every other animation. The element stays an inline
  `HTMLUnknownElement`, as the HTML standard requires.
* Security: a web page can no longer read or overwrite the storage of
  installed extensions. Content scripts got their storage, manifest and
  install-path functions as properties of the page's global object, and
  the functions took the extension id as an argument. A page that
  trapped one of those names before a `document_idle` content script
  ran could read and rewrite every extension's `storage.local`, `sync`
  and `managed` data and learn each extension's install directory. The
  functions are now handed to the content-script wrapper as a call
  argument, never through the global object, and each one is bound to
  its own extension's id in C.
* Responsive images pick the same `srcset` candidate as other browsers
  for any `sizes` value: the attribute is now parsed with the HTML
  algorithm, so comments, escapes, `0`, unclosed `calc(`, negative
  `min()`/`clamp()` results and invalid media conditions are handled
  instead of falling back to the full window width.
* Security: `new FontFace(family, bytes, descriptors)` no longer reads
  freed memory. The constructor took a pointer to the font bytes and
  then converted the descriptors to strings, so a descriptor whose
  `toString()` detached the bytes' `ArrayBuffer` (with `transfer()`)
  freed them before the font was installed from them. The descriptors
  are now converted first.
* A page can script an iframe showing a `blob:` URL it created itself,
  as same-origin content, instead of getting `contentDocument === null`.
* `position: relative` offsets given in percent or `calc()` move the box
  even when its parent's height comes from its content or from `top` and
  `bottom`, as in other browsers. bing.com's homepage photo fills the
  window again instead of being covered by the news strip.
* Multi-column layout balances its columns like other browsers: it
  looks for the lowest column height at which the content fits in the
  column count, instead of cutting at the average height and leaving
  the rest to the last column. `column-gap: normal` is now 1em of the
  element's font size, not 16px. wikipedia.org's "Read Wikipedia in
  your language" list put most languages in its last column and was
  500px taller than in Chrome. The frozen sitebench pages gain 102
  matching elements (wikipedia.org +73).
* An inline-block (or other atomic inline) next to a float sizes a
  percentage width against its containing block, as CSS says, not
  against the line width the float leaves. When it then does not fit
  beside the float it moves below it, as in other browsers, instead of
  shrinking to fit. wikipedia.org's "other projects" grid, an
  `inline-block` of `width: 65%` beside a 35% float, came out 65% of
  65% wide and wrapped every tagline. The frozen sitebench pages gain
  100 matching elements (wikipedia.org +61).
* An iframe placed inside a shadow root keeps its own styles; MDN's
  "Try it" demos rendered as unstyled text.
* An inline `<svg>` in a flex row takes the width the flex algorithm
  gives it, and shrinks below its `width` attribute when the line is
  too narrow, as in other browsers. Its flex basis now follows its own
  sizing too: `height: 32px; width: auto` on a `270x40` viewBox gives
  216px, not the 270px of the attribute. BBC's content-type icons are
  `width="30"` SVGs in a 20px flex box and came out 30px wide. The
  frozen sitebench pages gain 175 matching elements (bbc.com +164).
* Security: a `fetch()` or `XMLHttpRequest` started in a web worker no
  longer completes on the page's main thread. When the response arrived
  while the main thread was waiting for a parser-blocking script, the
  delivery was postponed with a timer on the process-wide default main
  context, so the main thread then ran the worker's JavaScript runtime
  while the worker thread could be running it too. That is a data race
  on the worker's heap. It showed as a false "Maximum call stack size
  exceeded" rejection on open.spotify.com, because the main thread's
  stack lies outside the range the worker runtime checks. The retry now
  stays on the worker's own context, and a worker no longer waits for
  the page's blocking fetch at all.
* A service worker's `message` event has `waitUntil()`, as an
  `ExtendableMessageEvent` does. TikTok's service worker calls it for
  every config message from the page and threw three TypeErrors on each
  visit, so it never stored its A/B and consent settings.
* An iframe's document no longer runs the top page's
  `document.onreadystatechange` (or any other `document.on…` handler)
  when its own events fire. The handler was looked up on whatever
  `document` the running realm had, so every iframe load called the top
  page's handler twice more. washingtonpost.com's performance script
  adds an iframe from that handler when the page is complete, so each
  new iframe added another one: thousands of iframes, the memory grew,
  and the page never settled.
* Spelling mistakes are underlined only in text fields you have clicked
  into, like other browsers, instead of in every prefilled field (search
  result pages showed the query underlined in red).
* Inside an iframe, `document.currentScript` stays set while the
  promise callbacks a script queued run at the end of that script, as it
  already did in the top document and as other browsers do. It was
  `null` there, and Next.js chunks built with Turbopack, which read it
  during module evaluation after an `await`, threw "Expected
  document.currentScript to be a <script> element" (naver.com's
  shopping block).
* `querySelectorAll()`, `getElementsByName()` and the other calls that
  return a static `NodeList` now return a real `NodeList` object, like
  other browsers, instead of a decorated `Array`: `Array.isArray()` is
  false for it, it has no `map`/`filter`, its prototype is
  `NodeList.prototype` and `JSON.stringify` gives an object. OneTrust's
  cookie dialog treats an array argument differently from a `NodeList`,
  threw while it built its preference center, and its "Learn more" /
  "Cookie settings" button did nothing (netflix.com).
* Web and service workers run on a thread with a 16 MB stack, so their
  scripts can recurse about as deep as in other browsers. Workers started
  on the platform's default thread stack, 512 KB on macOS and 1 MB on
  Windows, and the engine stopped a worker's recursion at three quarters
  of it: about 380 nested calls on macOS, against about 7,700 in Chrome,
  so a recursive parser or tree walk in a worker threw "Maximum call
  stack size exceeded". A worker now reaches about 12,000 nested calls.
* Pressing Enter fires a `keypress` event like other browsers, and a page
  that cancels it suppresses the newline. Chat boxes that send on
  keypress, such as duck.ai's prompt, now send when Enter is pressed.
* A shrink-to-fit box holding a float next to text is wide enough for
  both, instead of only the wider one, so the float no longer covers the
  text (python.org's Donate button sat inside the search field).
* A `<button>` styled `display: inline` is laid out as an inline-block
  like in other browsers instead of disappearing with its label
  (python.org's search "GO" button).
* Transitions and animations between lengths in different units
  (`16px` to `50%`, `1em` to `32px`) now move smoothly through `calc()`
  instead of jumping at the halfway point, and lists such as several
  background positions interpolate layer by layer instead of collapsing
  to the first one.
* Underlines honour `text-underline-offset` and
  `text-decoration-thickness` (GitHub's links set an offset), and lines
  drawn by the engine itself take the link's colour instead of the
  paragraph's. Table cells with `empty-cells: hide` and no content no
  longer paint their background and borders.
* The motion-path properties `offset-path`, `offset-distance`,
  `offset-rotate`, `offset-position`, `offset-anchor` and the `offset`
  shorthand are parsed and computed, and animate: `ray()` angles,
  rotations, positions and distances interpolate, and shapes and paths
  interpolate as they do for `clip-path`. Elements are not yet moved
  along the path.
* About seventy standard CSS properties the engine did not know are now
  parsed, cascaded and reported by `getComputedStyle()`:
  `text-underline-offset`, `text-decoration-thickness`,
  `text-underline-position`, `text-emphasis-*`, `scroll-behavior`,
  `touch-action`, `color-scheme`, `overscroll-behavior`, the
  `font-variant-*` and `font-synthesis-*` families, `font-palette`,
  `font-optical-sizing`, `text-rendering`, `text-size-adjust`, `zoom`,
  `resize`, `empty-cells`, `line-break`, `hyphenate-character`,
  `print-color-adjust`, `forced-color-adjust`, `scrollbar-gutter`,
  `background-blend-mode`, `mask-mode`/`-origin`/`-type`,
  `box-decoration-break`, the SVG `flood-*`, `lighting-color`, `marker-*`,
  `baseline-shift` and `alignment-baseline` properties,
  `view-transition-name`/`-class`, `-webkit-font-smoothing` and more.
  Their declarations used to be dropped, so `CSS.supports()` and
  `@supports` said no, and feature-detecting pages took their fallback
  paths. Invalid values are rejected as the specifications say, and
  numeric values animate. `white-space` is now the shorthand of
  `white-space-collapse` and `text-wrap-mode`, so `white-space: pre;
  text-wrap: wrap` keeps preserving spaces instead of resetting them,
  and `font-variant` sets all of its longhands.
* `@font-face` rules keep `src`, `unicode-range`, `font-display` and the
  other descriptors in CSSOM (`rule.cssText`, `rule.style.src`). Pages
  using Modernizr's web-font test concluded fonts were unsupported and
  hid their icon fonts; python.org's icons are back.
* Floats whose percentage widths add up to their container's width (six
  tabs of `16.6667%`) stay on one line instead of the last one dropping
  to a new row over a rounding error. python.org's top bar is one row
  again.
* A `<video>` now takes its natural size from the decoded video once
  its metadata arrives, as the spec says, instead of staying at the
  300x150 default. A video sized only on one axis (`height: 100%;
  width: auto`, as apple.com's hero animation is) now gets the other
  side from the video's aspect ratio rather than coming out 300px wide.
* Security: a page title can no longer hide the address of the page it is
  on. The shell read the renderer's replies with a key lookup that also
  matched a quoted word inside a string value, and the reply carries the
  title before the address, so a title ending in `"url` made the shell
  read no address and keep showing the one it had asked for: after a
  redirect, the address bar showed the site the user came from while the
  page came from another. A title ending in `"security` turned "Not
  secure" into the neutral icon the same way. The lookup now skips string
  values and takes a name only when a `:` follows it.
* Security: the Landlock sandbox of the renderer and the shell no longer
  opens the whole home directory when `$XAUTHORITY` is `~/.Xauthority`,
  as LightDM sets it on X11 sessions (Linux Mint, Xubuntu). The sandbox
  granted read access to the directory holding that file, which was then
  `$HOME`, so `~/.ssh`, `~/.aws` and every other file there were readable
  from the renderer that runs page content. It now grants read access to
  the `$XAUTHORITY` file alone. SECURITY.md also says what the renderer
  can read under `~/.config`, `~/.local/share` and `~/.cache`. Same fix as
  Northstar's.
* Security: a local HTML file can no longer read other local files. Every
  `file:` document is now an opaque origin: `fetch()` and
  `XMLHttpRequest` from a `file:` page treated every other `file:` URL as
  same-origin, so a page opened from the downloads folder could read
  another program's tokens under `~/.config` and list directories, and a
  local image from another folder drawn into a canvas did not taint it. A
  local page still loads its own scripts, style sheets and images. The
  same fix lands in Southstar and Northstar.
* Cookies set by responses that arrive at the same time are all kept.
  Every libcurl transfer loaded the jar when it started and wrote its
  whole in-memory copy back when it ended, so concurrent responses
  overwrote each other: of eight cookies set by eight parallel `fetch()`
  calls, the next request carried one or two. libcurl now only reads the
  jar; each `Set-Cookie` header is stored by the engine into the
  request's partition jar under a process-wide mutex and an exclusive
  lock on `cookies/.lock`, which also covers the other renderer
  processes. `document.cookie` also follows the jar: it was a copy taken
  when the page loaded, so a cookie set or deleted by a `fetch()` or XHR
  response never appeared, which broke sites that copy a CSRF token from
  a cookie into a request header. A read now takes the jar again when its
  version (the process's write count and the jar files' size and
  modification time) has changed. Same fix as Southstar's.
* README: brought up to date with the source — per-platform renderer
  sandboxing (Seatbelt on macOS, mitigation policies on Windows),
  OS-decoded H.264/AAC, Web Audio and camera/microphone capture, WOFF2,
  the bytecode cache, watchdog, tiled renderer IPC, macOS `.dmg` and
  Debian/Ubuntu `.deb` downloads, the full optional-dependency list and
  the BSD/musl CI badges.
* Security: `new FontFace(family, arrayBuffer)` copies the font bytes
  before it runs any page script. A setter on `Object.prototype.status`
  could transfer the buffer while the constructor was still reading it,
  and the font loader then read freed memory.
* Security: `navigator.clipboard.writeText()` and
  `document.execCommand('copy'/'cut')` need a recent click, key press or
  touch, as in Firefox and Safari. A page or an ad frame could replace
  the clipboard on a timer, e.g. swap a copied command or wallet
  address.
* Security: the in-tree MP4 demuxer (builds without FFmpeg: macOS
  without libav, Android, iOS) no longer lets a sample whose offset plus
  size wraps past 2^64 pass its bounds check, and no longer reads 8
  bytes past a truncated version-1 `mvhd` box.
* Security: the browser window downloads only `http:`, `https:`,
  `ftp:`, `data:` and `blob:` URLs (and `file:` from a `file:` page) on a
  renderer's request, so a compromised renderer can no longer have it
  save `about:` or `view-source:` pages.
* Debian packaging: build dependencies follow the current build
  (version floors, GStreamer, JPEG XL, WOFF2, PDF), the GStreamer
  H.264/AAC plugins are recommended instead of mpv, the build date comes
  from the changelog for reproducible builds, the unversioned embedding
  library is no longer shipped, and `debian/copyright` lists every
  bundled license.

1.0.32:
======
* Google is now the default search engine for the address bar and the
  `about:start` search box. A config still holding the previous
  DuckDuckGo default moves to Google; DuckDuckGo stays selectable in
  Settings.
* `about:start` redesign: the heading reads "Nordstjernen Browser" with
  the version, the rotating tagline is gone, the page sits on a plain
  white background, and the four shortcuts are compact pill buttons. A
  hint under the search box says whether Enter will open the typed
  address or search for it, and with which engine.
* The status bar no longer shows "WebGL enabled" when a page creates a
  WebGL context.
* CSS: adding or removing a stylesheet no longer restyles the whole
  document. The cascade compares the sheet list with the previous pass
  and restyles only the elements the changed sheets' selectors can
  match (by id, class, attribute or tag of each selector's subject);
  sheets with `@layer`, `@property`, `@import`, `@container` or a
  selector without such a key still restyle everything. Sibling
  invalidation now follows selectors nested in `:is()`/`:not()` and
  `:nth-child(… of …)` instead of treating any of them as "every
  attribute change restyles the following siblings", and a class change
  counts only the classes that were added or removed. Inserting a child
  floods only the children whose position-dependent compound can match.
  Two invalidation gaps are closed: `customElements.define()` restyles
  the elements it defines (`:defined`), and a child-list change restyles
  the following siblings of an element used as `X:empty + Y`. On a
  YouTube watch page the full restyles during start-up drop from ~20 to
  ~5 and cascade time from ~1.7 s to ~0.6 s.
* Release: a version tag now builds the macOS `.dmg` too. `release.yml`
  calls `macos.yml` in release mode, which requires the Developer ID and
  notarisation secrets, fails on any signing or notarisation error
  instead of falling back to an ad-hoc bundle, and checks the stapled
  `.dmg` and the `.app` inside it with Gatekeeper (`spctl`) before the
  `.dmg` is attached to the draft release. Releases 1.0.29 to 1.0.31
  shipped no macOS package, and CI notarisation had been failing
  unnoticed behind `continue-on-error`. `pack-macos.sh` signs the
  `.dmg` itself with the Developer ID, and sets `LSMinimumSystemVersion`
  from the bundled binaries instead of claiming macOS 11.
* SVG `<use>`: the referenced content inherits the rest of its style
  from the `use` element too, not only `currentColor`. Shapes drawn
  through `<use>` kept the `fill`, `stroke`, stroke geometry,
  `visibility` and font they inherit where their definition sits in the
  document, so a stroke set on a sprite leaked into every icon and a
  sprite hidden with `visibility: hidden` drew nothing. An instance now
  keeps only what the referenced elements declare themselves, a
  `<symbol>`'s own properties reach its content, and `fill:
  currentColor` from a rule stays tied to the text colour on elements
  that share one computed style.
* JS: events the browser creates itself (`message` on ports, windows
  and workers, `error`, `unhandledrejection`, XMLHttpRequest progress
  events, a worker's error events and WebGL context events) take their
  prototype from the realm's own interfaces, recorded when the window,
  frame or worker realm is set up, instead of looking `MessageEvent`,
  `ErrorEvent` or `Event` up on the global object each time. A frame
  starts from its parent's original interfaces, so the events a frame
  receives keep the original prototypes. A
  worker that deletes its globals for sandboxing, or a page that
  replaces `window.MessageEvent`, no longer makes messages arrive
  without `type` or `data`, and a worker whose `ErrorEvent` is gone
  still runs `self.onerror`; ChatGPT's answer worker never replied and
  the page spun until its scripts were halted.
* Media: a renderer stall no longer seeks the audio. Audio keeps the
  clock and the video catches up to it, as other browsers do; before,
  any tick more than a second late clamped the video clock and seeked
  the audio back, which was heard as skips.
* Media: a playing video is composited by the window as its own layer
  instead of being painted into the page tiles. The page tile under the
  video is left transparent, so captions and player controls drawn on
  top still blend over it, and a new video frame no longer repaints the
  page. The renderer decodes ahead and hands the window a queue of
  frames with their display times on the shared monotonic clock; the
  window shows the due frame at every vsync, so a busy page (layout,
  scripts, tile repaints) no longer stalls the picture. How far it
  decodes ahead adapts to how long the renderer has recently stalled,
  within a memory budget, and playback starts once that queue is
  filled. YouTube went from about 3 to a steady 25 frames per second
  and keeps its 720p stream instead of dropping to 360p.
* Layout: a table cell lays out its content as a block formatting
  context, so a float inside a cell shrinks to fit and sits beside the
  text, and sibling margins collapse. A styled cell used to stack its
  children one under the other at full width: Wikipedia's navbox title
  row put its "show" button, its "v t e" links and its title on three
  lines (71px tall instead of 28px).
* Layout: an auto-width table in a container with no room (width 0)
  shrinks to its min-content width instead of growing to its max-content
  width, and a cell holding a percentage-width table counts that table's
  min-content. The min-content width of plain text is its widest word,
  not its word with the most letters ("alpha beta gamma" measured
  "alpha"). Wikipedia's collapsed navbox rows, which are zero-width
  absolutely positioned blocks, laid their cells out 400px wide or more
  where other browsers use about 230px.
* Layout: `white-space: nowrap` (and `pre`) on an inline element inside
  wrapping text keeps the element on one line, and spaces on either side
  of a bidi isolate (an inline `<li>`, `<bdi>` or `[dir]` element)
  collapse into one. A nowrap link such as Wikipedia's `.nowraplinks a` could
  break inside, and a list made inline (`.hlist`) kept a second space
  after each separator.
* DOM: an element laid out as more than one box (a table and the
  anonymous row around its cells, for example) reports the geometry of
  its first box from every lookup. The lookup cache, built after a few
  queries, kept the last box instead, so `getBoundingClientRect()` on a
  `display: table` element with bare cells switched from the table's
  rectangle to its inner row's halfway through a script.
* Layout: a table caption sits outside the table's border and padding,
  as wide as the table's border box, and the table's border and
  background frame only its rows. The longest word of a caption widens
  the table, and a table holding only a caption shrinks to it. The
  table's box, as `getBoundingClientRect()` reports it, includes its
  captions. Captions used to be laid out inside the border at the
  width of the cells, so a two-letter table squeezed its caption into
  one word per line.
* Paint: a `<video>` with no frame and no poster to show paints
  nothing, so its own background shows through, unless it has
  `controls`. Such a video used to cover its box with a dark grey
  rectangle, drawn 4px up and left of the box when the video had a
  margin. A video that had not loaded, or failed to, covered The
  Guardian's video cards and Apple's product tiles with a black panel.
* Layout: an empty inline element in a line that has no line box (only
  empty elements, collapsible spaces and absolutely positioned elements,
  such as the `<source>` elements before a block `<img>` in a
  `<picture>`) reports a 0x0 rectangle at the line's position, after the
  margins it collapses with, and follows `text-align`, `text-indent` and
  `direction`. It reported 0,0 at the page's top-left corner, or a
  rectangle as tall as its font. Such a line no longer stops the margins
  around it from collapsing, and an element holding only white space
  (`<span> </span>`) counts as empty.
* DOM: an empty inline element (`<span></span>`, an empty link)
  reports a zero-width rectangle at its place in the line from
  `getBoundingClientRect()` and `getClientRects()`, as tall as its
  font. It used to report an empty rectangle at the page's top-left
  corner, so scripts that measure such markers (Wikipedia's citation
  metadata spans, scroll anchors) saw them at 0,0.
* Layout: `<source>` and `<track>` elements generate boxes when their
  `display` is not `none`, as HTML's rendering rules have it: inside a
  `<picture>` they lay out like any other empty element, and inside
  `<video>` they stay invisible with the rest of the video's children.
  The engine dropped them from layout, so a page that positions a
  picture's children absolutely, as The Guardian does for every
  thumbnail, measured all of its 676 sources at 0,0.
* CSS: the `width` and `height` attributes of an outer `<svg>` element
  map to the CSS `width` and `height` properties, as SVG 2 has it, so a
  stylesheet that sets only one of them keeps the other from the
  attribute, and lengths in `em` or `%` resolve. An `<svg width="24">`
  styled `height: 1rem` was drawn 16px wide from its viewBox's ratio
  instead of 24px wide, which shifted IMDb's reaction counts 8px left.
* Layout: a line that holds nothing but absolutely positioned elements,
  empty inline elements and collapsible spaces takes no height, as CSS
  requires of such lines. An absolutely positioned element between two
  empty inline elements (`<span></span> <svg style="position:
  absolute"></svg> <span></span>`) gave its line a full line height,
  which pushed everything below it down; YouTube's `<body>` was 11px
  tall instead of 0px.
* Layout: a `line-height` given as `calc()` with a length
  (`calc(0.0625rem * 24)`, `calc(1.5em)`, `calc(150%)`) sets the
  line height. Such values fell back to `normal`, while
  `getComputedStyle` reported the right value. Booking.com sets its
  line heights this way, so its text lines were laid out at the font's
  normal height: a two-line hotel name was 38px tall instead of 48px.
* Layout: an inline element whose `line-height` equals its block's
  keeps that line height inside an outer inline element with a
  different one. `<a style="line-height: 18px"><span style="line-height:
  20.8px">` in a block with `line-height: 20.8px` laid its lines out
  18px tall, as if the span had no line height of its own; AP News
  headlines are built this way.
* JavaScript: in strict-mode code, `return f()` inside a `try` block
  with a `catch` is no longer turned into a tail call. The tail call
  reused the frame and dropped the live `catch`, so an exception thrown by
  `f` escaped the `try` and aborted the script: helpers such as
  `try { return fn() } catch (e) { return fallback }`, common in bundled
  code, stopped whole scripts (BBC's paywall script, among others). A
  `return` in a `catch` clause without `finally` is still a tail call.
* Service workers: `Client`, `WindowClient` and `Clients` exist in the
  worker scope, a message from the page arrives with a `WindowClient` as
  its `source` (with `id`, `url`, `type`, `postMessage()`, `focus()`), and
  `clients.matchAll()` and `clients.get(id)` return that client. A worker
  that checked `event.source instanceof WindowClient` threw a
  ReferenceError on every message: Amazon's service worker logged six
  errors per page load and never answered the page.
* JavaScript: an external classic script is decoded with the charset of
  its `Content-Type` (or the `<script charset>` attribute, or a UTF-16
  byte order mark) before it runs, and invalid UTF-8 becomes U+FFFD, as in
  other browsers. Scripts were handed over as raw bytes, so a script
  served as `charset=iso-8859-1` with an accented letter failed whole with
  "invalid UTF-8 sequence": ESPN's Spanish-language i18n script, and the
  scripts that depend on it, never ran. Module scripts stay UTF-8.
* DOM: `URLSearchParams.prototype.forEach` walks the live list, so an
  entry deleted or added inside the callback changes the rest of the loop,
  as the specification and other browsers do. It walked a copy, which
  failed core-js's feature test (delete during forEach must give "a1c3"):
  core-js then replaced `URLSearchParams` with its polyfill, and Roblox's
  page threw "Incorrect invocation" from it.
* JavaScript: an iframe's scripts get a microtask checkpoint after each
  script, as the top-level document's already did, so a promise callback
  queued by one script runs before the next script starts. Frame scripts
  ran back to back and their promise callbacks only ran after the last
  one: YouTube's embed defines `writeEmbed` in a promise callback and calls
  it from the next inline script, so every embedded YouTube player (on AP
  News and most news sites) stayed black with "writeEmbed is not defined".
* DOM: `<link rel="preload">` loads for every known `as` (style,
  script, font, image, track, fetch), not only `style`, in the
  background, and reuses what the engine's preloader already fetched; a
  preload that becomes a stylesheet takes its `load` from the preload's
  response and keeps one resource timing entry. A `media` that does not
  match or a `type` the `as` cannot use skips the preload without an
  event, CSP (with the element's nonce) and mixed-content blocking give
  `error`, and `fetch` and `track` preloads load on any HTTP response,
  the others on a 2xx status, each link by its own rule when two links
  share one fetch. A link also loads again when it is inserted again;
  switching `rel` back from `stylesheet` to `preload` fetches nothing,
  so a handler that toggles `rel` cannot loop. An `as=image` preload
  with only `imagesrcset` loads the chosen candidate, and a preload does
  not hold back the window `load` event.
* Cache: shutting the cache down now waits for a cache write that is
  still running on a network thread. The write could see the cache
  directory already freed, build a relative path from the cache key,
  and leave the response body as a file named by 62 hex characters in
  the directory the browser was started from. A body path is now never
  built without a cache directory.
* CSS: `mask-size`, `mask-position` and `mask-repeat` (and their
  `-webkit-` forms, the `mask-position` shorthand and the position, size
  and repeat parts of the `mask` shorthand) are supported, and an image
  mask applies to the whole element: its background, borders and
  content. Image masks used to fill only the background colour, scaled
  to fit and centred, whatever the mask's own size and position said. A
  mask on a transformed element is now built in the element's
  transformed space, so it stays aligned with what it masks. Google's
  chat assistant masks its background glow, a gradient, with half of an
  SVG, and covered the whole page in flat blue.
* Layout: percentages in grid column sizes (`50%`, `calc(100% - 380px)`,
  `minmax(30%, 1fr)`) resolve against the grid's content width, as CSS
  requires, not against the width left after the gaps. AP News's main
  column, `calc(100% - 40px - 340px)` beside a 40px gap, was 40px
  narrower than in other browsers, and every story in it wrapped
  differently.
* Paint: an element that starts a 3D rendering context
  (`transform-style: preserve-3d` or `perspective`) paints with its own
  2D transform. The transform was dropped while hit-testing kept it, so
  DuckDuckGo's off-canvas side menu, moved out of view with
  `translate3d`, covered the right of the page and could not be closed.
* Layout: the viewport takes its `overflow` from the root element, and
  from `<body>` only when the root's `overflow` is `visible`, as CSS
  Overflow specifies. Any `overflow: hidden` on `<html>` or `<body>`
  locked the page, so a page styling `html { overflow: auto }` and
  `body { overflow: hidden }` could not scroll (DuckDuckGo results).
* Layout: a grid container whose height comes from its parent, as a
  stretched grid item or a flex item with `flex: 1`, sizes its `fr` rows
  against that height. The rows were sized as if the height were
  unknown, so a `1fr` row took only its content and a footer row sat
  right under it instead of at the bottom (Gemini's sidebar "Sign in").
* Paint: CSS `filter` (`blur()`, `grayscale()`, `brightness()` and the
  other bitmap functions) applies to any block, table or cell, not only
  to images: the box and its descendants are painted into a layer with
  room for the blur, filtered, then composited. Large blurs are drawn at
  a lower resolution, as other browsers do, so a `blur(146px)` glow
  costs a few milliseconds. `blur()` is now a Gaussian (three box
  passes) instead of a single box pass. SVG `filter="url(#id)"` applies
  when the filter is made of `feGaussianBlur` primitives, so blurred SVG
  shapes and SVG mask images (Gemini's background glow) are soft.
* Layout: a `1fr` grid column (and `minmax(auto, 1fr)`) is never narrower
  than the min-content width of its items, as `1fr` means
  `minmax(auto, 1fr)`: when the columns do not fit, they keep that width
  and the grid overflows, instead of squeezing every column. Items that
  scroll or set `min-width` keep a zero floor. The Guardian's
  six-column highlights carousel squeezed its 300px cards to 140px.
* DOM: `<link rel="preload" as="style">` fires `load` (or `error`) once
  its fetch ends, and changing a `<link>`'s `rel`, `href` or `as` loads
  it again: a link that becomes `rel="stylesheet"` applies its sheet and
  fires `load`. The common async-CSS pattern,
  `<link rel="preload" as="style" onload="this.rel='stylesheet'">`,
  never applied its sheet, so chatgpt.com kept its narrow-screen styles
  and its cookie dialog covered the whole page in white.
* Layout: a table caption's `::before` and `::after` are built like a
  table cell's, so a floated or block pseudo-element inside a caption is
  a float or a block, and an absolutely positioned one is laid out.
  Captions turned every pseudo-element into inline text, so the floated
  magnifier slot in Wikipedia's thumbnail captions took no room and the
  caption text wrapped differently from other browsers.
* Performance: forced style updates after script writes to inline styles
  recompute far fewer elements. An element whose style an animation
  rewrites is recomputed alone, with its descendants kept unless the
  values they inherit change; a `style` attribute change no longer
  re-runs `:has()` invalidation when no selector tests that attribute;
  and custom-property maps are compared layer by layer instead of by
  flattening every inherited variable. On deno.com, whose GSAP code reads
  `getComputedStyle` after each inline write, a style pass went from
  about 300ms to about 21ms.
* Layout: an inline element with `position: relative` (or any other
  containing-block trigger) is the containing block of its absolutely
  positioned descendants, which were laid out against the page instead;
  an absolutely positioned `::before`/`::after` of an inline element is
  laid out as a box rather than having its text pasted into the line;
  and an absolutely positioned box whose position comes from where it
  sits in a line starts at that line's top rather than at the first
  line's. MDN's external-link icons, drawn by an absolute `::after` of
  each link, appear after the link text as in other browsers.
* Layout: text decorations propagate from a block to the text of its
  block descendants, as CSS requires: a `display: block` or `flex` link
  around a `<div>` or `<p>` underlines its text, and `text-decoration`
  on a flex or grid container reaches its items. Only the element whose
  inline content held the text was consulted. Propagation stops at
  floats, absolutely positioned boxes and inline-blocks, and an element
  with `display: contents` decorates nothing (MDN's banner link).
* Layout: `margin-left: auto` and `margin-right: auto` on an image, video
  or SVG flex item push it along the line as they do for other items.
  Laying out a replaced item recomputed its margins from the style and
  dropped the share of free space the auto margin had taken, so
  kotlinlang.org's menu arrows sat right after their labels instead of
  at the right edge of each card.
* CSS: the legacy `-webkit-linear-gradient()`, `-webkit-radial-gradient()`
  and their repeating forms are understood, with their old conventions
  (angles measured from east, counter-clockwise; a bare side naming where
  the gradient starts; `position, shape` for radials), as Chromium and
  Firefox still accept them. vuejs.org's tagline gradient and older
  sites' button and banner gradients were dropped.
* CSS: `background-clip: text` (and `-webkit-background-clip: text`)
  paints the background only inside the glyphs of the element's text,
  for blocks and for inline elements, and `-webkit-text-fill-color` sets
  the colour text is filled with. Gradient headlines (nodejs.org's "Run
  JavaScript Everywhere", Tailwind's `bg-clip-text text-transparent`)
  showed a solid gradient rectangle with the text cut out of view.
* Layout: the text of a block `::before`/`::after` collapses its white
  space like any other text, so the widespread clearfix
  `content: " "; display: block; clear: both` no longer adds an empty
  line under every container that uses it. NPR's front page came out
  700px taller than in Chromium, EFF's tagline sat 64px low, and apple.com
  and Smashing Magazine had gaps between sections.
* Layout: a line break from `<br>` drops the collapsible spaces around it,
  so the next line no longer starts with a space ("one <br> two" put
  " two" on the second line), and a `<br>` styled `display: block` or
  `flex` still breaks the line as in other browsers instead of joining
  the text on both sides: rust-lang.org's tagline read "everyoneto
  build" on one line.
* Layout: a block's negative horizontal margins reduce its parent's
  max-content width, as in other browsers. The shrink-to-fit measurement
  took the larger of the child's margin box and its content width, so
  negative margins never counted: Wikipedia's main-menu button
  (`margin: 0 -12px`)
  made its header slot 44px wide instead of 20px and pushed the logo
  24px to the right; inline-blocks, floats, table cells and flex items
  holding such a block came out too wide in the same way.
* SVG: icons drawn through `<use>` take `currentColor` from where they are
  used. A sprite's `<symbol>` lives in a hidden `<svg>`, and its paths
  resolved `fill="currentColor"` against that tree's colour, so every
  sprite icon (Bootstrap, svelte.dev and most icon sets) came out black
  instead of in the colour of the link or button around it.
* Layout: images and SVGs sized with `calc()` contribute that size to grid
  and table tracks. The min-content contribution of a replaced element read
  only its laid-out width and fell back to 200px, so Tailwind's `size-4`
  icons (`calc(var(--spacing) * 4)`) widened an `auto` grid column to 200px:
  tailwindcss.com's "Quick search" label wrapped onto two lines.
* Paint: an absolutely positioned `::before`/`::after` is painted in tree
  order with the other positioned boxes of its stacking context. Its box
  has no DOM node of its own, so the deferred-layer sort could not place it
  and it came last, above every later `position: relative` element: the
  hero heading, button and illustration on svelte.dev were hidden under the
  hero's `::before` gradient.
* JavaScript: the CSS Font Loading API loads fonts. `new FontFace(family,
  source)` kept neither its `url()` source nor its `ArrayBuffer`, `load()`
  only flipped `status`, and `document.fonts` ignored `add()`, so fonts a
  page loaded from script never rendered. `url()` sources now load through
  the `@font-face` loader (resolved against the document base), buffers are
  installed from memory, `status` and `loaded` settle to `loaded` or
  `error`, and `document.fonts` is a set: `add`, `delete`, `clear`, `has`,
  `size`, `forEach`, `values`/`keys`/`entries` and `for…of`. Faces added
  while unloaded start loading.
* Layout: `<em>`, `<i>`, `<cite>` and `<dfn>` follow their computed
  `font-style`. The inline builder made them italic by tag name even when a
  stylesheet set `font-style: normal`, so Stripe's hero heading (an `<em>`
  styled upright) rendered slanted. The user-agent stylesheet already makes
  them italic.
* HTML: `<base href>` applies to subresources. Scripts, stylesheets, images
  and the speculative preloader resolved relative URLs against the page URL,
  so a page with `<base href="/assets/">` (or an Angular app with
  `<base href="/">` opened on a nested route) loaded none of them. Link
  clicks, form actions, media and the favicon follow the base too. Only
  `a.href` and other script-visible URLs honoured it before. CSP `base-uri`
  still applies, and the referrer and CSP `'self'` stay the page URL.
* Paint: gradient backgrounds honour `background-size`,
  `background-position` and `background-repeat`. A gradient always filled
  its whole box, so a `linear-gradient(red, red) 0 100% / 100% 2px
  no-repeat` underline bar became a solid block and tiled dot or stripe
  patterns became one large shape. Inline elements (`<span>`, `<a>`) now
  paint their gradient backgrounds as well; only `url()` images were drawn.
* Windows: the build moves from MSYS2's MINGW64 environment to UCRT64.
  MSYS2 no longer ships GTK 4 for MINGW64, so `pacman` could not install
  the toolchain and the Windows CI failed before compiling. The packaging
  scripts look for the UCRT64 prefix first, and `docs/Windows.md` gives
  the UCRT64 package names.
* Layout: desktop builds ignore the width in `<meta name="viewport">`, as
  desktop browsers do. A page asking for `width=1120` (Wikipedia) was laid
  out 1120px wide in a narrower window, so its narrow-window media queries
  and `matchMedia()` never matched and the page scrolled sideways. Android
  and iOS keep honouring it.
* Headless runs look for `<video>` elements once per layout instead of on
  every settle tick, matching the interactive renderer.
* Style: elements whose style reads an attribute through `attr()` are
  restyled when one of their attributes changes instead of on every
  relayout, so forced layouts on image-heavy pages skip most of the tree.
* Layout: text with a CSS `line-height` no longer re-runs line breaking on
  every relayout just to read back its line heights; they are cached with
  the text measurement. Speedometer 3.1's NewsSite suites, which force
  ~150 layouts per run, are about 30% faster.
* Images created by script inside a frame resolve their relative `src`
  against the frame's document even before they are inserted, and image
  loads belonging to a removed frame's document are dropped instead of
  being refetched against the parent page (Speedometer's NewsSite suites
  no longer request hundreds of 404ing placeholders).
* JavaScript: `Atomics.waitAsync` is available, `Atomics` operations check
  detached and immutable buffers in the order the specification gives, and
  `Atomics.wait` no longer returns `"timed-out"` early on Windows. Exporting a
  binding under the string name `"*"` works. The engine now passes all of
  test262 (102,988 test runs) except two SpiderMonkey staging tests that
  contradict the specification.
* JavaScript: more fixes from the SpiderMonkey test suite: `Set` methods
  and typed-array `set`/`sort`/`with` follow the specified order of
  observable steps, class heritage and computed keys run in strict mode,
  `super[expr]` evaluates `expr` first, logical assignment to a `const`
  short-circuits, Annex B block functions in `if` clauses and duplicate
  declarations behave as in other browsers, several missing early
  SyntaxErrors are reported, and legacy `fn.arguments` works.
* JavaScript: fixes from the SpiderMonkey test suite: `new Function`
  rejects parameter lists that only parse together with the body, legacy
  `fn.caller` is `null` for strict, native and top-level callers, legacy
  date strings with `-07` offsets parse, `Array.from`/`Map`/`Set` close
  iterators only when the specification says so, `RegExp.prototype.source`
  escapes line terminators, and `Math.acosh`/`asinh` are accurate.
* JavaScript: decorators (`@dec` on classes, methods, getters, setters,
  fields and `accessor` elements, with `context.access`,
  `addInitializer` and `Symbol.metadata`) and auto-accessors
  (`accessor x = 1`) are supported.
* JavaScript: calls in tail position of strict-mode code reuse the
  caller's frame, so deep recursion written in tail form (including
  through `?:`, `&&`, `||`, `??` and catch clauses) no longer overflows the
  stack, and such calls are faster.
* JavaScript: `ShadowRealm` is available: `evaluate` runs code in a
  separate realm and `importValue` loads a module there, with only
  primitives and wrapped functions crossing the boundary.
  `Promise.allKeyed` and `Promise.allSettledKeyed` await the values of an
  object's own enumerable properties. The legacy `RegExp.$1`…`$9`,
  `lastMatch` and related statics are read-only (except `input`), throw for
  a receiver other than `RegExp`, and are cleared by a match of a `RegExp`
  subclass or another realm's regexp; a `String.prototype.replace` that
  deletes matches updates them too.
* JavaScript: Temporal is built into the engine and follows the current
  specification for the ISO 8601 calendar: `PlainDate`, `PlainTime`,
  `PlainDateTime`, `PlainYearMonth`, `PlainMonthDay`, `ZonedDateTime`,
  `Instant`, `Duration` and `Temporal.Now`, with `round()`, `total()`,
  `relativeTo`, every rounding mode, the RFC 9557 string formats and
  `Date.prototype.toTemporalInstant`. Arithmetic is exact to the
  nanosecond. test262's Temporal tests all pass.
* JavaScript: Temporal uses the system time zone database, so
  `ZonedDateTime` accepts IANA names such as `Europe/Oslo` and follows
  their daylight-saving rules, and `Temporal.Now.timeZoneId()` reports the
  system zone. On Windows the database is read from `TZDIR` or a
  `share/zoneinfo` folder next to the browser.
* JavaScript: Temporal supports the calendars of ECMA-402: `buddhist`,
  `chinese`, `coptic`, `dangi`, `ethioaa`, `ethiopic`, `gregory`,
  `hebrew`, `indian`, `islamic-civil`, `islamic-tbla`,
  `islamic-umalqura`, `japanese`, `persian` and `roc`, with `era`,
  `eraYear`, leap-month codes such as `M05L`, and date arithmetic in each
  calendar. On Windows, `Temporal.Now.timeZoneId()` maps the system zone
  to its IANA name, and the Windows package ships the time zone database.
  Zones in different countries, such as `Europe/Oslo` and `Europe/Berlin`,
  are never treated as the same zone.
* JavaScript: deferred module imports. `import defer * as ns from "m"`
  and `import.defer("m")` load and link a module but evaluate it only
  when a property of its namespace is first read. Its top-level-await
  dependencies are still evaluated ahead of time. Source phase imports
  (`import source x from "m"`, `import.source("m")`) parse, and
  `%AbstractModuleSource%` is available to hosts. JavaScript modules have
  no source form, so importing one in the source phase throws a
  `SyntaxError`. The test262 tests for both proposals pass.
* JavaScript: the browser's own `Intl` (`src/js_intl.c`) is gone; every
  page, worker and test262 run uses the engine's CLDR-generated `Intl`,
  whose default locale is now the page's `navigator.language`.
* JavaScript: `Intl.Segmenter` splits text into graphemes, words and
  sentences with the Unicode 17 UAX #29 rules (extended grapheme clusters,
  emoji ZWJ sequences, Indic conjuncts), runs of Thai, Lao, Khmer and
  Myanmar letters kept together as words, and `isWordLike`. It passes every
  case of the Unicode break test files.
* JavaScript: `Intl.ListFormat`, `Intl.RelativeTimeFormat`,
  `Intl.DisplayNames` and `Intl.DurationFormat` join the engine's `Intl`.
  Lists and relative times use CLDR patterns (with the Spanish `y`/`e`,
  `o`/`u` and Hebrew `ו` rules), display names cover languages with
  dialect names, regions, scripts, currencies, calendars and date-time
  fields, and `DurationFormat` handles every style including `digital` and
  fractional seconds. `Temporal.Duration.prototype.toLocaleString` formats
  through it.
* JavaScript: the engine's `Intl.DateTimeFormat` formats dates in the
  Gregorian, Buddhist, Japanese, ROC, Chinese, Dangi, Hebrew, Islamic,
  Persian, Indian, Coptic and Ethiopic calendars with CLDR patterns, the
  skeleton matcher, `dateStyle`/`timeStyle`, day periods, fractional
  seconds, every `timeZoneName` style (metazone names for 13 languages),
  `formatRange` with CLDR interval patterns, and Temporal values;
  `Date.prototype.toLocaleString` and the Temporal `toLocaleString`
  methods use it. Named time zones come from the system tz database, and
  on Windows from the registry when no tz database is installed.
* JavaScript: `Intl` moves into the QuickJS engine, generated from
  Unicode CLDR 48 without ICU. Language tags are canonicalized per UTS #35
  (aliases, likely subtags, extension values) and `Intl.Locale` has the
  full API including `maximize`, `minimize`, `getWeekInfo`,
  `getTextInfo` and the other info methods. `Intl.NumberFormat` supports
  every v3 option: exact decimal strings, nine rounding modes, rounding
  increments and priorities, compact, scientific and engineering notation,
  units, currencies with accounting signs, `formatRange` and
  `formatToParts`, in about 100 languages and their numbering systems.
  `Intl.PluralRules` covers every CLDR language, and `Intl.Collator`,
  `localeCompare` and Turkish/Lithuanian `toLocaleUpperCase`/
  `toLocaleLowerCase` are locale-aware.
* JavaScript: an assignment resolves its target before evaluating the
  right-hand side. A strict-mode assignment to an undeclared global throws
  even when the right-hand side creates it, and a `var` introduced by a
  direct `eval` in the right-hand side no longer captures the store.
* JavaScript: QuickJS is updated to quickjs-ng 9b06171. Template
  literals convert each substitution to a string as soon as it is
  evaluated, and engine-internal errors can no longer be caught by page
  scripts. Failures in WebGPU, WebAssembly, `getRandomValues` and the
  compression streams throw catchable `TypeError`/`RangeError` instead.
* JavaScript: a module's top-level `using` and `await using` bindings can
  be exported, and functions declared in the module see them.
  `Function.prototype.toString` of a built-in whose name is not an
  identifier, such as the `RegExp.$&` getter, prints the name as a
  computed key. test262 reports no failures on the enabled tests.
* DOM: the HTML insertion methods `appendHTML`, `prependHTML`,
  `beforeHTML`, `afterHTML` and `replaceWithHTML`, with their
  `…Unsafe` variants, are available on elements, shadow roots and
  character data. They parse with the right context element (the
  parent, the shadow host or the owning `<template>`) and support
  declarative shadow roots. The `Unsafe` variants run scripts when
  `runScripts: true` is given. The safe variants strip scripts,
  embedding elements, event-handler attributes and `javascript:` URLs,
  and do nothing on `<script>` elements. Both kinds honour a
  `sanitizer` option's `removeElements`/`elements` lists. They throw
  `HierarchyRequestError` where the specification requires it.
* DOM Ranges: exceptions thrown by `Range` methods are created in the
  range's own window. A range in an `<iframe>` document used to throw
  the top window's `DOMException`, so `e instanceof
  frame.contentWindow.DOMException` was false.
* README: the conformance table uses a full re-run with the corrected
  test runner. The earlier HTML figure missed about 88,000 subtests
  that the runner had wrongly recorded as crashes.
* CSSOM: `@position-try` rules appear in `cssRules` as
  `CSSPositionTryRule`, with `name`, `cssText` and a `style` of type
  `CSSPositionTryDescriptors`. The descriptors expose only the
  properties allowed in a position-try block (insets, margins, sizes,
  self-alignment, `position-anchor`, `position-area`) and drop any
  others. Editing them through `setProperty`, `cssText` or the
  property accessors updates the fallback positions on the page.
* CSS Masking: `mask-border` and its longhands (`mask-border-source`,
  `-slice`, `-width`, `-outset`, `-repeat`, `-mode`) parse, serialize,
  inherit, interpolate and composite the way the matching
  `border-image` properties do, with the mask-border initial values
  (`slice: 0`, `width: auto`, `mode: alpha`). Painting with a mask
  border is not implemented yet.
* HTML rendering: an `<iframe>` with no `width`/`height` attribute or
  CSS size now takes the 300×150 default size of a replaced element,
  as in other browsers. It used to stretch to its container's width
  and grow to its document's height.
* Forms: the `select` event of text controls now follows the HTML
  text-selection APIs. It is queued as a task instead of dispatched
  synchronously, fires only when the selection or direction actually
  changed, and several changes before it runs produce one event. This
  also applies to disconnected elements.
* HTML focus: a focused form control that becomes disabled, for
  example through a disabled ancestor `<fieldset>`, loses focus at the
  next rendering update, as the HTML focus-fixup rule requires.
  `document.activeElement` falls back to the body, and `blur` and
  `focusout` fire.
* Performance: a custom property whose `var()` expansion grows to the
  1 MB substitution limit no longer makes every restyle quadratic.
  The container-unit scan re-measured the remaining text at every
  digit, and the exponential-blowup test page took 19 s to style. It
  now takes 3.5 s, most of it the capped substitution itself.
* Security: `HTMLSelectElement.add()` did not check its argument.
  Adding an `<option>` that contains the select made the option its own
  ancestor, and the renderer then looped forever. `add()` now follows
  the HTML algorithm: it accepts only `option`/`optgroup`, throws
  `HierarchyRequestError` for an ancestor and `NotFoundError` for a
  `before` element outside the select, counts options inside
  optgroups for a numeric `before`, and inserts next to the reference
  even when it is in an optgroup.
* Security: deeply nested elements whose `min-width` or `max-width` is
  an intrinsic keyword (every `<fieldset>` has
  `min-inline-size: min-content` in the UA stylesheet) took exponential
  time to lay out. Each level measured its subtree twice, so 100
  nested fieldsets hung the renderer. The measurement is now reused,
  and that page lays out in under a second.
* Security: a module graph where a dependency failed to load and other
  modules imported the failed module through a cycle could crash the
  renderer. The failed module was freed while the cycle still pointed
  at it, and the next script that imported it ran into the freed
  memory. Modules whose resolution failed now stay registered and
  reject every later import with a fresh `TypeError`.
* Images: `img.width` and `img.height` report the rendered content box
  instead of the border box. An SVG image without a `width`, `height`
  or `viewBox` takes the 300×150 default object size, and a missing
  dimension falls back to that default, instead of a 512-pixel square.
  The window `load` event now also waits for images that scripts
  inserted before it fired.
* CSS Sizing / Containment: size containment now affects layout. The
  block size is contained by `contain: size`/`strict`,
  `container-type: size` or `content-visibility: hidden`, the inline
  size also by `contain: inline-size` and `container-type: inline-size`.
  A contained box is sized as if empty, or from the new
  `contain-intrinsic-width`/`-height` (logical
  `-block-size`/`-inline-size`, shorthand `contain-intrinsic-size`),
  which accept `none`, a length, or `auto` with either. Their lengths
  interpolate. Content that overflows a contained box still scrolls.
* CSS Text: `text-align-last` and `text-justify` are supported, both
  inherited. In justified text, `text-align-last: justify` stretches
  the last line too, `center`/`right`/`end` place it accordingly, and
  `text-justify: none` turns justification off. `distribute` is parsed
  as its alias `inter-character`.
* README: conformance numbers from the local web-platform-tests and
  test262 runs, and the new CSS Color, anchor positioning, Shapes and
  animation-composition coverage in the feature list.
* DOM: `offsetTop`, `offsetLeft` and `offsetParent` of elements inside
  an `<iframe>` are measured within the frame's own document. They
  used to include the frame's position in the embedding page, which
  sent scripts that pass those offsets to `elementFromPoint()` (image
  maps, drag handles, tooltips) far outside the frame.
* HTML rendering: the UA stylesheet now has the bidirectional-text
  rules from the HTML rendering section. Block containers, sectioning
  and heading elements, table parts, lists, `bdi` and `output` get
  `unicode-bidi: isolate`. So does any element with `dir=ltr`, `rtl`
  or `auto` (matched case-insensitively). `bdo` is `isolate-override`,
  and `dir=auto` search/tel/url/email inputs, textareas and `pre` use
  `plaintext`. Inline elements with a `dir` attribute are now isolated
  from the surrounding text, as in other browsers.
* Animations: `filter` and `backdrop-filter` interpolate the way
  Filter Effects specifies. Function lists interpolate per function,
  the shorter list is padded with identity functions, and `none` acts
  as the identity list. Results are clamped to each function's range.
  `drop-shadow()` colors interpolate premultiplied. Composition `add`
  appends the lists and `accumulate` sums matching functions. The
  computed value is now canonical: amounts as numbers clamped to
  their range, `hue-rotate()` in degrees, `blur()` in pixels,
  `drop-shadow()` as color + three lengths with `currentcolor`
  resolved, and quoted `url()`.
* CSS Fonts: `font-size-adjust` is a known, inherited property. It
  accepts `none`, an optional metric (`ex-height`, `cap-height`,
  `ch-width`, `ic-width`, `ic-height`) followed by a number, `calc()`
  or `from-font`, and leaves the default `ex-height` out when
  serializing. `from-font` computes to the primary font's aspect value.
  Values with the same metric interpolate and add as numbers without
  going negative. Glyph sizing does not use it yet.
* CSS Shapes: the `shape()` function (`from`, `move`/`line`/`hline`/
  `vline`/`curve`/`smooth`/`arc` commands with `to` or `by` endpoints,
  control-point anchors, arc sweep/size/rotation and `close`) parses
  for `shape-outside` and `clip-path`. Its computed value resolves
  positions and lengths, and it interpolates command by command,
  also against a `path()` that has the same command list. `clip-path`
  basic shapes now share the `shape-outside` parser, so invalid shapes
  are rejected, positions and radii serialize canonically,
  `border-box` drops out next to a shape, and `getComputedStyle`
  resolves lengths and percentages.
* CSS Shapes: `shape-outside` accepts `path()`, with an optional fill
  rule. Its path data keeps the commands as written in the specified
  value and becomes absolute in the computed value
  (`m 10 10 h 80` → `M 10 10 H 90`). Paths with the same command
  sequence interpolate segment by segment; any other pair animates
  discretely.
* Animations: basic shapes interpolate and add. `shape-outside` and
  `clip-path` transitions and keyframes between `circle()`,
  `ellipse()`, `inset()`/`rect()`/`xywh()` and `polygon()` values
  blend radii, positions, insets, corner radii and points component
  by component (mixed lengths and percentages become `calc()`), and
  `composition: add` sums them. Shapes of different kinds, different
  reference boxes or polygons with different point counts stay
  discrete. `polygon()` accepts the CSS Shapes 2 `round <length>`
  corner radius.
* CSS Shapes: `shape-outside`, `shape-margin` and
  `shape-image-threshold` are real properties now. Basic shapes
  (`circle()`, `ellipse()`, `inset()`, `rect()`, `xywh()`,
  `polygon()`) and shape boxes parse with the specification's
  validity rules and serialize in canonical form: positions reorder to
  horizontal-first, default radii and `margin-box` drop out, inset and
  corner-radius lists collapse like the `margin` shorthand, and
  `calc()` keeps relative units. Computed values resolve lengths
  against the element's font and the viewport, positions to
  percentages, and `rect()`/`xywh()` to `inset()`. Floats do not yet
  wrap around the shape.
* Animations: transform lists interpolate as CSS Transforms specifies.
  Lists of different lengths are padded with identity functions, so
  `translateX(0)` → `translateX(100px) rotate(90deg)` animates both
  functions instead of jumping. Functions that do not match, and
  `matrix()` values, interpolate through the 2D matrix decomposition
  (translate, scale, rotation, skew) instead of component by component.
  `perspective()` interpolates through 1/d. Singular 3D matrices stay
  discrete. Keyframes written as `initial`, `inherit`, `unset` or
  `revert` resolve to the matching computed value before interpolation.
  The individual `translate`, `scale` and `rotate` properties, and
  `transform-origin`/`perspective-origin`, compose by combining values
  rather than concatenating function lists. 2,376 more interpolation
  and composition subtests pass.
* CSS: the `columns`, `flex-flow`, `list-style`, `animation-range`,
  `scroll-margin-block`/`-inline`, `scroll-padding-block`/`-inline` and
  `position-try` shorthands accept CSS-wide keywords (`initial`,
  `inherit`, `unset`, `revert`, `revert-layer`) and expand them to their
  longhands. These declarations used to be dropped, so
  `CSS.supports('columns', 'initial')` was false and removing such a
  shorthand from an inline style left its longhands behind.
* JavaScript: module scripts fetched over HTTP must be served with a
  JavaScript MIME type, and `import ... with { type: "json" }` modules
  with a JSON MIME type (`application/json`, `text/json` or `*/*+json`).
  The Content-Type is parsed as the MIME Sniffing spec says, so a value
  with control characters, separators or non-ASCII characters in its
  type or subtype is rejected. Rejected and failed module fetches throw
  `TypeError`, as in other browsers, instead of `ReferenceError`.
* CSS: anchored boxes that overflow fall back through
  `position-try-fallbacks`. The flip tactics (`flip-block`,
  `flip-inline`, `flip-start`, `flip-x`, `flip-y`) mirror insets,
  margins, self-alignment, `anchor()` sides (percentages included) and
  `position-area`. `@position-try` rules supply replacement declarations,
  and `position-area` values can be options themselves. The first option
  whose box fits its inset-modified containing block wins; if none fits,
  the base position is used. `position-try-order` sorts the options by
  available width or height. 286 more css-anchor-position subtests pass.
* HTML: `<input type=color>` supports the `alpha` and `colorspace`
  attributes and their `alpha`/`colorSpace` IDL reflections. Its value
  now accepts any CSS color (named, `rgb()`, `oklch()`, `color()`, ...)
  through the CSS Color module. Without `alpha` it serializes as `#rrggbb`
  in `limited-srgb` and as `color(display-p3 ...)` in `display-p3`; with
  `alpha` the alpha channel is kept. Values that do not parse become
  opaque black. WPT `color.window.html` goes from 21 to 187 of 196.
* HTML: `<script>` decides whether to run from its `type` and `language`
  attributes as the HTML spec says. Every JavaScript MIME type essence
  now runs: `application/ecmascript`, `application/x-javascript`,
  `text/javascript1.0`–`1.5`, `text/jscript`, `text/livescript`,
  `text/x-ecmascript` and the rest, case-insensitively and with
  surrounding whitespace ignored. So does a `language` attribute naming
  a JavaScript type. Before, only `text/javascript` and
  `application/javascript` ran. `module` and `importmap` still have to
  match exactly. 618 more WPT subtests pass in scripting-1.
* CSS: anchor positioning. `anchor-name` and `position-anchor` tie an
  absolutely or fixed positioned box to an anchor element, the last one
  in tree order carrying the name. Its insets can use `anchor()`, its
  sizes, insets and margins can use `anchor-size()`, and both can appear
  inside calc(), min()/max()/clamp() and each other's fallbacks.
  `position-area` places the box in one cell or span of the 3x3 grid
  around the anchor. Auto insets resolve to 0 and normal alignment
  points toward the anchor. `justify-self`/`align-self: anchor-center`
  centers the box on the anchor. A function that cannot resolve falls
  back as the spec says, including in getComputedStyle. `anchor-scope`,
  `position-try-fallbacks`, `position-try-order`, `position-try` and
  `position-visibility` parse and serialize; fallback positions are not
  applied yet. css-anchor-position goes from 445 to 11,564 passing WPT
  subtests.
* CSS: gradient color stops keep their color space in computed values.
  `linear-gradient(in oklch, color(srgb 1 0 0), blue)` used to compute
  to `rgb(255, 0, 0)` for the first stop. It now serializes
  `color(srgb 1 0 0)`, and specified stops use the canonical CSS Color 4
  form. All 932 `gradient-interpolation-method-computed` subtests pass.
* Animations: keyframe composition is implemented. `composite: 'add'`
  and `'accumulate'` on Web Animations keyframes and effects, and
  `animation-composition` on CSS animations and their keyframes, combine
  the keyframe value with the underlying value before interpolating:
  lengths and calc() sums, numbers, colors, rects, shadow lists (added
  as concatenation, accumulated pairwise) and transform lists
  (concatenated, or accumulated function by function). Web Animations
  also extrapolate when the effect's easing pushes progress outside
  0..1, using the first or last keyframe interval as the specs require,
  instead of clamping. Animated integer properties (`order`, `z-index`,
  `column-count`, `orphans`, `widows`, ...) round and stay in range.
  2,328 more WPT interpolation and composition subtests pass.
* CSS: selectors follow the Selectors 4 rules for what may come after a
  pseudo-element. A class, attribute, type selector or combinator after
  one makes the selector invalid (`::before.a`, `::before div`), and so
  does a pseudo-element inside `:not()`, `:has()` or `:is()`. After a
  pseudo-element only user-action pseudo-classes are allowed, or
  `:only-child` on the view-transition tree. `::part()` and `::slotted()`
  may be followed only by the pseudo-elements they allow. The
  `::view-transition-*()` argument grammar is validated (`*`, a name, and
  `.class` tags), and `CSS.supports(selector(…))` reports known
  pseudo-elements that the engine never matches as supported. 1,136 more
  WPT subtests pass in css-view-transitions, css-pseudo and selectors.
* JavaScript: `Promise.try()` returns the callback's promise unchanged
  when it is already an instance of the receiver, as the current spec
  requires, instead of wrapping it in a new one. A unicode regular
  expression no longer finds a match that starts on the second half of
  a surrogate pair (`/\udf06/u.exec('\ud834\udf06')` is `null`), which
  the search prefilter allowed. `TypedArray.prototype.with()` reads
  elements past a buffer shrunk during argument coercion as
  `undefined`: `NaN` in float arrays and a `TypeError` in BigInt
  arrays. Nine more test262 tests pass.
* CSS: colors are parsed and serialized according to CSS Color 4 and 5
  by a new module, `src/css_color.c`. `lab()`, `lch()`, `oklab()` and
  `oklch()` used to be flattened to 8-bit sRGB. They keep their color
  space now, along with `color()` in every predefined space
  (`srgb-linear`, `display-p3`, `a98-rgb`, `prophoto-rgb`, `rec2020`,
  `xyz-d50`/`xyz-d65`) and `none` components. Relative color syntax
  (`rgb(from …)`, `oklch(from …)`, `color(from …)`) is supported, as are
  `color-mix()` with any number of colors in every interpolation space
  and hue method, missing-component carry-forward, powerless hues,
  `alpha()`, `contrast-color()` and `color-layers()`. Specified and
  computed values serialize the way the specs describe, for example
  `color-mix(in srgb, red, blue)` computes to
  `color(srgb 0.5 0 0.5)`. WPT `css/css-color/parsing` passes 11,914
  of 12,154 subtests, up from about 5,200.
* Media: a <video> removed from the document, or whose source is
  replaced or cleared, stops its sound. The player record kept playing
  - and the audio helper kept looping its track - whenever the page
  took the element away without calling pause() first, as single-page
  sites do when they swap content on an in-page navigation; the
  cleanup for abandoned players only ran when two or more were open.
* Media: a video's `videoWidth`/`videoHeight` follow the frames being
  shown and a `resize` event fires when they change. They used to be
  fixed by the first stream, so after an adaptive-quality switch to a
  stream of another shape (YouTube serving a 4:3 video after a 16:9
  start) the page kept sizing the player for the old aspect. libav
  frames are also no longer stretched to the size the decoder opened
  with when the stream's resolution changes, and anamorphic sample
  aspect ratios are applied.
* Media: libavcodec decodes video with frame and slice threads again
  (up to 8 per decoder), as the removed video helper did, and opening a
  WebM/MP4 decoder no longer runs avformat_find_stream_info when the
  container already describes the stream. A YouTube seek that rebuilds
  the decoder now resumes in under a second. The in-tree MP4 demuxer
  caps a fragment run without per-sample fields at 65536 samples and a
  track at two million, so a malformed file cannot make it allocate
  hundreds of megabytes.
* Media: builds without libav - Android, iOS, and macOS without
  FFmpeg - play MP4 and H.264 MSE video through a small in-tree MP4
  demuxer (sample tables, edit lists and fragmented MP4) feeding the
  platform decoder (MediaCodec, VideoToolbox). Their audio has no MP4
  path yet.
* Media: H.264 video and AAC audio are decoded by the operating
  system's own codecs: Media Foundation on Windows, VideoToolbox and
  AudioToolbox on macOS and iOS, MediaCodec on Android, and GStreamer
  (VA-API, V4L2 or an installed software decoder) on Linux and the
  BSDs. The browser ships no H.264 or AAC decoder, so it pays no codec
  licence; the LGPL FFmpeg bundled with the macOS and Windows packages
  gains only the MP4 demuxer and the H.264/AAC parsers. MP4 and HLS
  video and H.264 MSE players now play there, and `avc1`/`mp4a`
  report as supported. Media Foundation loads at runtime, so Windows N
  editions without the Media Feature Pack still start. Frames are
  converted to BGRA only when shown, so a seek no longer converts
  every frame between the keyframe and the target.
* Media: MSE video decodes in the renderer again, and the separate
  `nordstjernen-video` process is gone. The renderer keeps each
  appended segment with its time range and feeds the decoder only the
  contiguous run that holds the playhead, so a stream starts on its
  first media segment, a seek rebuilds the decoder at the segment
  holding the target instead of seeking through a byte stream whose
  offsets do not match the original file, and a seek past the buffer
  waits at the target until the page appends it. `SourceBuffer.buffered`
  now lists every disjoint range rather than one range from the first
  to the last byte, so YouTube fetches the segment a seek lands in
  instead of believing the gap is buffered. Evicting segments with
  `remove()` no longer restarts the decoder. Headless runs now drive
  `play()`, `pause()`, `currentTime`, `muted` and `volume` like the
  shell does.
* JS: Streams follow the WHATWG Streams standard. `ReadableStream`,
  `WritableStream` and `TransformStream` queue chunks by size, apply
  backpressure through `highWaterMark`, call `pull()` only when there
  is room, and `pipeTo()`/`pipeThrough()` honour `preventClose`,
  `preventAbort`, `preventCancel` and an abort `signal`. `tee()`,
  `ReadableStream.from()`, async iteration, BYOB readers over byte
  streams (buffers are transferred, not copied), the queuing strategies
  and the controller classes are there too. A stream that pulled
  lazily, as ChatGPT's answer stream does, never delivered a chunk
  before. Not there yet: `byobRequest` is always null and
  `autoAllocateChunkSize` is ignored, so a byte source has to
  `enqueue()` its chunks.
* Fetch: `Response.body` and `Blob.stream()` are byte streams, so
  `getReader({mode: 'byob'})` works on them. The body stream of a
  buffered response is made on first use and copies nothing until it
  is read. `clone()` tees a streamed body and gives the clone its own
  copy of each chunk, and `bytes()` returns its own copy, so
  transferring one buffer no longer empties the other. A body whose
  stream is locked or was already read makes `text()`, `json()`,
  `arrayBuffer()`, `bytes()`, `blob()` and `formData()` reject with a
  TypeError, and `clone()` throws for it; a locked body keeps
  `bodyUsed` false, and reading the body stream of a buffered response
  sets it. Of the streams, fetch body, encoding stream and compression
  web-platform tests, 1802 of 2289 subtests pass, up from 629 of 2039.
* CSS: scroll-driven animations. `scroll-timeline-name`,
  `scroll-timeline-axis` and the `scroll-timeline` shorthand are parsed,
  serialised and reported by `getComputedStyle`, and `animation-timeline`
  takes `none`, a named timeline or `scroll(nearest|root|self
  block|inline|x|y)`. Such an animation follows its scroller's scroll
  position instead of the clock, with iteration count, direction, fill and
  easing applied, and updates on the next frame after a script, wheel or
  scrollbar scroll, laying the page out again when it animates a layout
  property. The nearest scroller is looked up along the containing block
  chain, a `body` whose overflow belongs to the viewport counts as the
  root scroller, and an animation without a declared duration spans the
  whole scroll range. `none` and a name that no element declares hold the
  animation at time zero, with its delay, fill and an auto duration of
  zero applied as on the document clock. `view-timeline-name`,
  `timeline-scope` and the names in the `view-timeline` shorthand are
  parsed, and a name that only they declare runs on the document clock
  like `view()` does, since view timelines are not implemented yet.
  Changing `animation-timeline` between the clock and a scroll timeline
  restarts the animation. A sidebar whose grid columns are driven by a
  named horizontal scroll timeline now opens at its full width. Not yet
  supported: view timelines, `view-timeline-axis` and
  `view-timeline-inset`, scroll timelines shared through
  `timeline-scope`, `animation-delay` and `animation-range` on scroll
  timelines, and the scripting API of a scroll-driven animation.
* Layout: a grid item stretched to its row (`align-self: normal` or
  `stretch`, `height: auto`) has a definite height, so `height: 100%`
  on its children resolves against the stretched size, as in Chrome. A
  child with `height:100%` used to collapse to its content, so content
  meant to sit centred between `1fr` rows stuck to the top instead. A
  stretched item that is itself a flex container is laid out against the
  stretched height, so `justify-content`, `align-items` and `flex-grow`
  inside it use the row height. The stretch respects the item's
  `min-height` and `max-height`, percentages against the grid area, and
  skips items that behave as `start` (`align-self: normal` with an
  `aspect-ratio` or an intrinsic `height`).
* Layout: a column flex container whose height comes from its flex
  parent (a stretched row item or the main size of a column) no longer
  subtracts its padding and border a second time with
  `box-sizing: border-box`, so its children are no longer placed too
  high, and a `height: 100%` card in such a column keeps its size when
  the row is laid out twice.
* Paint: gradients interpolate in premultiplied alpha, as CSS requires.
  A stop that is `transparent` (transparent black) no longer pulls the
  colour of its neighbour towards black, so
  `linear-gradient(transparent, #fcfcfc)` fades without a grey band and
  `linear-gradient(red, transparent)` stays red as it fades. This covers
  linear, radial, repeating and conic gradients and mask gradients; a
  transition hint takes the premultiplied average of its neighbours at
  its position (its curve is still a straight segment through that
  colour). Colour stops placed before the previous stop, for example
  `red 50%, blue 20px`, are clamped to it instead of scrambling the
  colours.
* Layout: a `::before` or `::after` whose `content` is `none` or `normal`
  generates no box, also when it is absolutely positioned or an
  inline-block. Such pseudo-elements used to paint their background, so
  a decorative `content:none; position:absolute; inset:0` gradient
  covered the text around it. Such a pseudo-element also no longer
  changes counters with `counter-increment`, `counter-reset` or
  `counter-set`.
* Layout: percentage `max-width`, `min-width`, padding and margins on a
  floated box, a column flex item or a grid item that is not stretched
  (images, tables and absolutely positioned boxes included) now resolve
  against the containing block instead of the box's own fitted width.
  A chat bubble with `max-width: 70%` in a column flex container used to
  be squeezed to 70% of its own text width and wrapped mid-word; it now
  keeps the width of its text. A floated image with `padding: 0 5%` in a
  600px container is now 160px wide, not 126px. A grid item that is
  fitted by `justify-self` or `justify-items` other than stretch now
  includes its padding and border in the fitted width. A float with
  `max-width: 50%` inside a zero-width parent is 0px wide. Images with
  `box-sizing: border-box`, padding and `max-width: 100%` no longer
  overflow their container by the padding.
* Layout: an absolutely positioned box inside a scrolling element
  (`overflow: auto` or `scroll`) now counts toward how far that element
  can scroll when the element or a box between it and the positioned box
  is the containing block, including the box's own descendants and boxes
  inside inline-blocks. Before, `scrollWidth` reported the overflow but
  `scrollLeft` stayed at 0 and no scrollbar appeared, so a 410px-wide
  positioned bar in a 200px scroller could not be scrolled; it now
  scrolls up to 210px like Chrome, rounded to whole pixels. Transformed
  boxes and everything inside them, fixed boxes, zero-size boxes,
  `content-visibility` subtrees and subtrees clipped by an inner
  `overflow` are not counted, so a centered `translate(-50%, -50%)`
  modal adds no scrollbar. In right-to-left and reversed (`row-reverse`,
  `column-reverse`, `wrap-reverse`) scrollers the flipped axis gets no
  range and boxes lying past its start edge are ignored on the other
  axis; the page's own `html`/`body` scroller is untouched.
* Paint: elements with a negative `z-index` paint and receive clicks
  below the in-flow content of their stacking context (CSS 2.1
  Appendix E step 3), not inside their nearest positioned parent. A
  `z-index: -1` background inside a `position: relative` wrapper with
  no `z-index` used to sit on top of the text after the wrapper and
  swallow its clicks, which is how chatgpt.com backgrounds covered
  text. Positioned elements and flex or grid items qualify. A layer
  inside an overflow clip stays in the clip, the layers of the root
  context paint above the background propagated from `html` or `body`,
  and clicks on `body` outside its children still reach `body`.
  Layers with the same `z-index` keep document order, with `::before`
  ahead of the element's children and `::after` behind them. Not
  lifted yet: positioned inline elements and inline-blocks with a
  negative `z-index`, which still paint with the line that holds them.
* Paint: paint and hit testing share one stacking-context test: the
  root, `fixed` and `sticky`, positioned or flex/grid items with a
  `z-index`, opacity below 1, transforms, `perspective`,
  `preserve-3d`, `filter`, `backdrop-filter`, `clip-path`, masks,
  `mix-blend-mode`, `isolation: isolate`, `will-change` of those,
  `contain` with layout or paint, and `content-visibility` other than
  `visible`. Such an element keeps its positioned descendants inside
  it and, like a positioned element, paints and takes clicks above
  the in-flow content that follows it. A dropdown inside a wrapper
  with `opacity`, `transform` or `will-change` was drawn under the
  next block and clicks on it went to that block. Inline-blocks and
  images that form a stacking context still paint and take clicks in
  their line, as before.
* Input: hit testing skips the contents of `content-visibility:
  hidden` elements and elements hidden by `clip: rect(0 0 0 0)`, as
  painting does, and boxes outside the point are dropped before they
  are queued as layers. `elementFromPoint` on a list of 4000
  `position: relative` rows took 20 ms and now takes 0.3 ms.
* CSS: `isolation`, `will-change`, `contain` and `backdrop-filter` are
  parsed from style sheets (they were only read from inline styles by
  `getComputedStyle`). Invalid values are dropped, so a later invalid
  declaration no longer overrides a valid one, and `CSS.supports`
  answers for them; `backdrop-filter` is checked as a filter list.
  `will-change` keeps the case of the names it lists. Integer
  properties such as `z-index` clamp to the 32-bit range, so
  `z-index: 99999999999` stays on top (on x86-64 it turned negative).
* JS: microtasks, `requestAnimationFrame` callbacks, ResizeObserver and
  IntersectionObserver callbacks and the rendering tick no longer run in
  the middle of another script. The microtask checkpoint after a script
  inserted by other script (`appendChild()` of an inline script,
  `document.write()`) is left to the outer task, as the HTML standard
  says. While a script waits for a stylesheet that `getComputedStyle()`,
  `offsetWidth` or `getAnimations()` needs, or for a synchronous
  `XMLHttpRequest`, the rendering tick, animation frames, animation
  events and observer deliveries wait until it returns. Observer
  callbacks forced by a layout read run after the script, resize
  observations before intersection ones, each followed by a microtask
  checkpoint. ChatGPT spun in its render loop at startup until its
  scripts were halted, because its own queued flush ran inside the
  flush that was waiting for a stylesheet.
* SVG `foreignObject`: scripts can measure the HTML inside it.
  `getBoundingClientRect()` returned an empty rectangle and `offsetWidth`
  0 for every element in a foreignObject, so diagram labels that size
  themselves by measuring their `<div>` (Mermaid, draw.io, D3 charts)
  collapsed. The rectangle now follows the foreignObject's position, the
  `viewBox` scale and its transforms, and `elementFromPoint` reaches that
  HTML before the page is first painted.
* Security: a regular expression with a named backreference to more than
  255 groups of the same name (`(?<a>…)|(?<a>…)|…\k<a>`) is a syntax
  error again. Since 1.0.31 raised the capture limit, 256 such groups
  compiled with the group count cut to one byte, so the matcher ran the
  group indices as instructions: the expression matched wrongly and could
  read past the engine's opcode table or hang.
* Security: page script no longer runs while a restyle waits for a
  stylesheet. 1.0.31 held timers and other page tasks during blocking
  fetches, but `fetch()` and `XMLHttpRequest` completions still ran there,
  and restyling (from `getComputedStyle()` after adding a `<link>`) kept
  using the page's own URL string. A completion handler that called
  `history.replaceState()` freed that string while the restyle still read
  it. Restyle now works on its own copy, and fetch and XHR results wait
  for the stylesheet like the other page tasks.
* Security: a WebGL 2 page can no longer crash the renderer or read its
  memory through `texImage3D`/`texSubImage3D` from an image, canvas or
  video. Those uploads reset the row length and skip settings before
  handing GL the decoded pixels but kept `UNPACK_SKIP_IMAGES` and
  `UNPACK_IMAGE_HEIGHT`, so GL read past the pixels into the texture.
* Security: `WEBGL_multi_draw` rejects list offsets past the end of the
  page's array. The bounds check multiplied the offset before comparing,
  so an offset near 2^62 wrapped around and the draw read its first and
  count values from memory before the array.
* Web Audio: a sound started with `start()` or `start(0)` plays from its
  beginning. The start time was kept as given, so once the context had run
  longer than a clip, the real-time output treated the clip as already
  finished and stayed silent (most game sound effects, Howler.js), looped
  music began part-way through, and even `start(ctx.currentTime)` lost the
  first tenth of a second. A start or stop time in the past now means the
  next sample the context renders, and audio nodes carry their `context`.
* When a tab's renderer cannot start, the status bar keeps saying "Failed
  to load page". The message was replaced by "Done" at once, so the tab
  showed a blank page with no hint of what went wrong (#186). The reason is
  also printed to the terminal: the framebuffer could not be created, the
  renderer could not be executed, or it did not start.
* macOS: the app carries its own fontconfig configuration. The bundled
  fontconfig looked for Homebrew's `/opt/homebrew/etc/fonts`, so on a Mac
  without Homebrew it ran on a built-in fallback and, unable to save a
  cache under the renderer sandbox, every tab rescanned all system fonts.
  It now reads the system font folders and caches them in
  `~/.cache/nordstjernen/fontconfig`. CI now also loads pages through the
  packaged renderer process with Homebrew moved out of the way, the
  setup issue #186 was reported from.
* Build: `-Dquickjs=quickjs` compiles again. The WebGL `multi_draw` fix
  called a QuickJS function the original engine does not export; the
  adapter now provides it.
* Code cleanup: removed engine functions, macros and struct fields that
  nothing used (found by relinking every executable with
  `--print-gc-sections` and checking the Android, iOS, Java and macOS
  hosts), including fields that were written but never read, such as the
  pre-parsed `@keyframes` values and a shadow copy of WebGL vertex
  attribute state. Merged duplicated code: the inline-text setup, font
  attributes, text alignment and form-control sizing that layout and
  painting each carried a copy of; the OffscreenCanvas 2D method table;
  the Wuffs image decoder setup; the `lab()`/`oklab()` colour parsers;
  the inline `style` attribute scanners; the file:// and ftp:// folder
  listings; the hardened SQLite open used by history, the HTTP cache and
  IndexedDB (now `src/db.c`); and the helper and executable path
  lookups. Rendering is unchanged.

1.0.31:
======
* Android: the app no longer crashes when a page is opened while another
  is still loading or animating, as when going to google.com from the
  address bar. Each page's renderer runs as a thread in the app, and two
  of them could run scripts, layout and text shaping at the same time;
  the engine now serves one renderer at a time.
* Input: pages receive `wheel` events (and the legacy `mousewheel` when
  only that is listened for) from the mouse wheel and touchpad, one per
  notch with `deltaY` 100, at the pointer. Over an element with a
  non-passive wheel listener the browser waits for the page before
  scrolling, so `preventDefault()` keeps the page still; maps zoom and
  custom scrollers work. Elsewhere scrolling starts at once as before.
* Input: `pointerup`, `mouseup` and `click` carry the position where the
  button was released, not where it was pressed, and moves while a
  button is held report `buttons = 1`. Dragging a map pans it with the
  pointer instead of flinging it back.
* DOM: XPath 1.0 is implemented: `document.evaluate`,
  `document.createExpression`, `createNSResolver`, `XPathResult`,
  `XPathExpression` and `XPathEvaluator`, with all axes, the core
  function library and every result type. `XPathResult` was undefined
  before, so scripts that locate nodes by XPath threw.
* Canvas: `ctx.font` is parsed in CSS shorthand order. A numeric weight
  such as `'500 14px Roboto'` was taken as the font size (text drew at
  500px), and `bold`/`italic` were read by Pango as part of the family
  name. Map labels rendered through a 2D canvas now appear.
* Layout: percentages on an absolutely positioned, shrink-to-fit box
  (`max-width: calc(100% - 740px)`, padding, margins) resolve against
  its containing block instead of its own content width, and a box with
  only `left` or `right` set fits into the space beside that inset.
* Layout: an inline-block inside an inline run contributes its
  min-content width to the run's min-content width. Flex items holding
  inline-block buttons with multi-word labels no longer refuse to shrink
  and overlap their neighbours.
* Performance: runs of small `<style>` elements are merged into sheets
  of roughly 32-128 KB split at content-defined boundaries, instead of
  one sheet per run. A page that adds or edits one CSS-in-JS style
  element now re-parses only the group it belongs to, not every
  neighbouring style (368 KB of CSS per change on one page).
* Build: the clang builds on Linux, macOS and Windows compile again.
  The merged-stylesheet-run flush cast a `GArray`'s byte buffer straight
  to its element type, which clang's `-Wcast-align` rejects under
  `--werror`; it now takes the first element through `g_array_index`.
* Performance: the renderer remembers the page's scrollable size until
  the next relayout or viewport change. Every tick it reports to the
  browser window walked the whole box tree to find the page bottom,
  which was about a tenth of renderer time on a large, busy page.
* Performance: computing values of custom properties registered with
  `@property` no longer measures the element's and the root's fonts
  for `ex`, `ch`, `cap` and `ic` units unless a value might use one.
  Those eight font-metric lookups ran for every element that sets a
  registered property, and were half of that work on a page that
  registers many properties.
* Performance: collecting a page's stylesheets for a relayout no longer
  re-reads every small `<style>` element. Each element's final CSS text
  and its traits (whether it can join the merged run of neighbouring
  styles, whether it has viewport-dependent `@media` rules) are
  remembered against a fingerprint of its text, and the merged run is
  looked up by a digest of its parts, so its text is only assembled when
  it changed. A page with ~760 CSS-in-JS `<style>` elements (1.5 MB)
  that forces hundreds of layouts by reading element geometry no longer
  copies and hashes all of it several times per layout.
* DOM: setting an inline style property to the value it already has
  (`el.style.display = 'none'` when it is already `none`), or removing
  a property the element does not have, no longer rewrites the `style`
  attribute. Each rewrite queued a mutation record, so a page whose
  MutationObserver re-applies a style on every `style` change fed
  itself forever; the page's JavaScript never went idle and was halted
  by the runaway-script monitor a minute later, leaving its content
  (such as a grid of live rooms) unrendered.
* Layout: `position: fixed` and absolutely positioned elements inside
  an iframe are placed against the iframe's own viewport. The lookup
  for their containing block walked out of the frame's document into
  the host page, so an ad frame whose `<body>` is `position: fixed;
  left: 0; width: 100%` was laid out across the whole window, and the
  video inside it was drawn at the page's left edge instead of in its
  slot. Fixed boxes inside a frame also no longer follow the top-level
  scroll.
* Media: pages with more than four playing videos (a main video
  next to several video ads) no longer leave the main video black.
  The video helper held four players and silently dropped the oldest
  paused one, which was often the main video before its pre-roll
  finished. It now holds eight, and an evicted video is reopened at
  its position when it is played again. Videos are also no longer
  stopped as "not painted" while the page sits still, a regression
  from noting video positions only after a relayout or scroll, and
  looping `<video loop>` clips played by the helper restart from the
  beginning instead of freezing on their last frame.
* Media: in-process playback of a fragmented-MP4 Media Source stream
  (HLS players such as hls.js transmux into one) no longer stalls
  once the decoder catches up with the bytes it was opened over. The
  MP4 demuxer treats its first end of data as the end of the file, so
  the player fired `waiting` every second and repeated frames while
  minutes of video sat buffered. It is now reopened over the grown
  buffer and resumes after the last packet it decoded.
* Performance: the renderer no longer walks the whole box tree on every
  animation tick to find `<video>` boxes; it does so after a relayout
  or when the scroll position changes. Text-measurement cache keys are
  built from the Pango attributes' values directly, instead of
  printing the attribute list to a string for every measured run.
* Media: video no longer turns into a blank white rectangle when the
  browser window switches to tiled page rendering. In that mode the
  window drew only videos whose position the page reported in page
  coordinates, so one positioned relative to the viewport showed the
  page background through its hole. The video helper also makes every
  frame fully opaque, as the window now composites it.
* Performance: video frames from the helper process are handed to GTK
  as premultiplied BGRA, the layout they already have, instead of
  BGRX. GTK converted every BGRX frame on its worker threads before
  uploading it, which cost the browser window about a core and a
  half while a 720p video played next to a few video ads; the window
  now uses about 40% less CPU.
* Service workers: the `fetch` event a worker receives carries its
  `request`, `clientId`, `resultingClientId`, `preloadResponse` and
  `handled` as the event's own members, built as a `FetchEvent`.
  Workbox's router read `event.request.url` and threw for every
  request the page made.
* CSS: a `@keyframes` rule's `cssText` lists its keyframes as rules
  (`0% { … } to { … }`). It used to read like a declaration block with
  a stray `;` after the last keyframe. A page that copies stylesheet
  text through the CSSOM fed that back to the parser, which then lost
  the rule after every `@keyframes`. The parser now also recovers from
  a stray `;` among keyframes as the syntax specification says: the
  next keyframe rule is dropped and the block still closes there.
* Media: `buffered` on a `<video>` or `<audio>` playing a Media Source
  stream starts where its media does (the later of the audio and video
  starts, after any `remove()`), not at 0. Players read the range to
  decide what to fetch after a seek and when to evict old segments.
* Media: a Media Source stream whose audio is muxed into the video
  SourceBuffer keeps its sound past the first few seconds. The audio
  helper's copy was written once from the bytes present when playback
  began and never grew, so the sound ended after a segment and the
  picture, which follows the audio clock, froze with it. The copy now
  grows with the stream, as a separate audio SourceBuffer's already
  did.
* Media: the browser window composites every video the helper process
  decodes, not just the most recently opened one. Each tab had a
  single video slot, so a muted video ad that started after the main
  player took the slot and the player (and every other video) showed
  the blank page background. A finished or stalled video that was not
  in the slot also never told the page it had ended.
* Media: setting a `<video>` or `<audio>` element's `src` runs the
  media element load algorithm, as `load()` already did. The element
  pauses, rewinds to 0 and fires `abort`, `emptied` and `loadstart`.
  A player that reuses one element for a pre-roll ad and then the
  programme used to start the programme at the ad's end time, already
  "playing", so its `play()` raised no `play`/`playing` events. The
  player stayed in its buffering state and gave up after half a
  minute.
* DOM: reading `contentDocument` on a `<div>` threw "Illegal
  invocation", which broke jQuery's `.contents()` on every div. The
  polyfill copied the frame-only native getter onto
  `HTMLDivElement.prototype`; it now leaves the native accessor alone.
* `navigator.webkitTemporaryStorage` and `webkitPersistentStorage`
  (`queryUsageAndQuota`, `requestQuota`) exist, as in the Chrome the
  user agent names, and report what `navigator.storage.estimate()`
  does.
* Security: fixes from Northstar's audit and a use-after-free that
  AddressSanitizer found there on github.com, ported here.
  - Page script no longer runs in the middle of layout. Layout waits for
    stylesheets by running the main context, and the lifecycle tick
    (deferred and module scripts), the async-script timer,
    `AbortSignal.timeout()`, `FileReader` and the IntersectionObserver
    tick could fire there. A `history.replaceState()` from such a script
    freed the page URL the stylesheet walk was still passing to its
    requests. Those tasks now wait for the fetch to finish, and layout
    works on its own copy of the URL.
  - WebAssembly: an export returning an `externref` that no table or
    global held had its host box freed by the periodic reclaim before the
    result was converted, so every 64th such call read freed memory. The
    results are converted first.
  - Appending a node to a descendant more than 512 levels below it
    slipped past the cycle check and hung the next tree walk; the check
    now walks every ancestor.
  - A `conic-gradient()` with an enormous `from` angle looped forever
    while painting; the angle is normalised with `floor()`.
  - `Intl.DateTimeFormat` formats an infinite time as `Invalid Date`
    instead of converting it to `time_t`.
* WebAssembly: a JavaScript exception thrown into a wasm call that
  JavaScript made from inside another wasm call now reaches that
  JavaScript caller. It used to surface only at the outermost call, so
  the `try`/`catch` around the inner call never ran. Emscripten's
  `setjmp`/`longjmp` and C++ exceptions rely on catching it there.
* `new ImageData(data, width)` throws `IndexSizeError`, as the HTML
  standard requires, when the pixel count is not a multiple of the
  width (it threw `InvalidStateError`), and rejects empty data.
* The parsed-stylesheet caches evict only sheets no cascade has used
  for a long while, so a page with hundreds of stylesheets keeps them
  all parsed while sheets from pages navigated away from are freed.
* Pages with many stylesheets no longer re-parse all of them on every
  relayout. The parsed-stylesheet caches were emptied whenever they held
  more than 256 sheets; Discord links about 600, so each style change
  re-parsed every one and a relayout cost 100-200 ms (growing as more
  loaded) instead of about 16 ms, keeping the renderer at full CPU.
* `transform-style: preserve-3d` is treated as `flat` on elements whose
  opacity, overflow, filter, clip-path, mask or blend mode force
  flattening, as CSS Transforms specifies. Such elements used to go
  through the 3D path, which ignored their opacity and transform;
  Discord's user panel drew the hidden username on top of "Online".
* Content of a scrolled element is no longer culled from painting as if
  it were still at its unscrolled position. Anything more than a screen
  below the top of a scroll container's content disappeared once
  scrolled into view, while it could still be clicked.
* A scroll container that gets its height by being stretched (an
  absolutely positioned box with `top`/`bottom` set, or a row flex item
  stretched to its line) can scroll through all of its content. Its
  scroll range was computed before the stretch and stayed 0, so setting
  `scrollTop` did nothing; Discord's message list stayed on its loading
  placeholders instead of showing the newest messages.
* Clicks, hovers and `elementFromPoint` reach the HTML inside an SVG
  `<foreignObject>`, mapped through the SVG's viewBox and transforms.
  Discord's server icons, whose click handlers sit inside a
  `foreignObject`, now switch servers.
* A row flex item no longer keeps a stretched height from an earlier
  layout pass. When the container's height only became definite in a
  later pass, an item with `height: 100%` kept the content height it was
  stretched to before, and its own `height: 100%` children grew to it;
  Discord's chat column ran 800 px past the window, hiding the messages
  and the message box.
* Changing an element that is not in the document (setting its `style`
  or other attributes, appending children to it, editing its text) no
  longer marks the page for a full restyle. React-style code that builds
  detached nodes and then reads `getComputedStyle` paid a whole-document
  cascade (about 300 ms on Discord) for every such read.
* An absolutely positioned element sized by `top` and `bottom` (or
  `inset: 0`) with an auto height now gives its children a definite
  height when it is a flex container, so `flex: 1` columns and
  `height: 100%` children inside it fill it instead of growing to their
  content.
* Inline SVG `<foreignObject>` renders its HTML content: the subtree is
  laid out at the element's width and painted inside the SVG's
  transforms, clip and mask. Masks also resolve ids defined in another
  `<svg>` of the document and honour `maskContentUnits="objectBoundingBox"`.
  Discord's avatars and server icons, which are images inside masked
  `foreignObject`s, now show up with their round shape and status cut-out.
* Typed arrays and other ECMAScript built-ins no longer get an own
  `Symbol.toStringTag` data property. The interface-tagging pass meant for
  DOM interfaces also tagged `Uint8Array.prototype` and its siblings,
  hiding the `%TypedArray%.prototype` getter that libraries such as
  `which-typed-array` read; Discord's login page threw "a function is
  required" and never rendered.
* Regular expressions may have up to 65535 capture groups instead of 255.
  Discord's login bundle contains a pattern with more than 255 groups and
  failed to load with `SyntaxError: too many captures`.
* WebGL supports `WEBGL_multi_draw`: `multiDrawArraysWEBGL`,
  `multiDrawElementsWEBGL` and their instanced variants issue one draw
  per list entry with `gl_DrawID` set, after checking every range
  against the bound buffers. three.js's `BatchedMesh` (the mesh-batch
  and batch-LOD examples) needs it and drew nothing without it. The set
  of attributes a program reads is now cached per program instead of
  queried from the driver on every draw.
* WebGL 2's sized texture uploads accept every image source:
  `texImage2D`/`texSubImage2D` with explicit width and height, and
  `texImage3D`/`texSubImage3D`, now take an `<img>`, canvas, video,
  `ImageBitmap` or `ImageData` (and a byte offset into a bound
  `PIXEL_UNPACK_BUFFER`) as well as a typed array. They used to upload
  nothing for those, so three.js's WebGL fallback backend for
  `WebGPURenderer` drew every texture black.
* A playing `<video>` can be drawn into a 2D canvas and uploaded as a
  WebGL or WebGPU texture: `drawImage`, `texImage2D`/`texSubImage2D` and
  `copyExternalImageToTexture` take its current frame instead of only
  its poster, including when the video is `display:none` (the usual way
  three.js feeds a `VideoTexture`). The three.js video panoramas and
  video-material examples now show the video.
* `<video>`/`<audio>` pick the first `<source>` whose `type` the browser
  can play, as the HTML resource-selection algorithm says, instead of
  the first `<source>` of any kind; a page listing an Ogg Theora file
  before an MP4 used to get the Ogg and play nothing.
* Ogg Theora video (`.ogv`) plays inline wherever FFmpeg's Theora
  decoder is present, and the bundled LGPL FFmpeg build enables it.
* `videoWidth`/`videoHeight` are set before `loadedmetadata` fires,
  `loadeddata` fires, and `requestVideoFrameCallback` waits for the
  first frame instead of firing at once on a 0×0 video (three.js sized
  its GPU texture 1×1 from that and then failed every upload).
* A `crossorigin` video served with `Access-Control-Allow-Origin` no
  longer taints the canvas it is drawn into.
* The address bar shows the URL being loaded as soon as the load
  starts. It stayed empty (or kept the previous page's address) until
  the renderer finished the first layout, which on script-heavy pages
  such as the three.js WebGPU examples took many seconds.
* JPEGs with 16-bit quantization tables decode. The vendored Wuffs
  decoder refused them as "unsupported precision", although the samples
  are 8-bit and only the table entries are wide; Photoshop writes such
  files, and the three.js MilkyWay skybox is one, so pages using it
  rejected their textures and drew nothing behind the scene.
* `<input type=range>` is drawn as a slider (a track filled up to the
  value and a round thumb, in `accent-color`) instead of a boxed row of
  dashes with the number printed after it, and clicking or dragging it
  sets the value and fires `input` and then `change`. The settings
  panels of the three.js examples were showing "50" beside every
  slider and could not be adjusted. `<input type=color>` shows a colour
  swatch for its current value; it used to print the hex text of the
  `value` attribute, so a colour set from script read `#000000`.
* The Windows, Linux (clang) and Java builds compile again, and the
  macOS, Windows, Linux and AppImage packages and the Java native jar
  are produced again. The Windows link lost the no-op
  `ns_gl_prepare_for_sandbox` because MSYS2 ships `epoxy/egl.h`, clang
  rejected a seccomp socket rule with an incomplete argument comparison,
  and the release-build check in the pack scripts read meson's empty
  sanitizer list `[]` as a sanitizer and refused every release tree.
  The musl build retries its Alpine package install, so one CDN edge
  failing the TLS handshake for the package index no longer fails it.
* Builds with the nghttp2 HTTP backend (`-Dhttp_backend=nghttp2`) follow
  the cookie settings and learn HSTS. That backend sent and stored
  cookies on every request, so cross-site requests carried the user's
  session cookies and third parties could set cookies whatever the
  cookie policy said, and it ignored `Strict-Transport-Security`, so
  http:// links to HSTS sites were never upgraded. It now attaches and
  stores cookies only when the policy allows them for the request, and
  records HSTS from verified HTTPS responses in the same store the curl
  backend uses.
* `EventSource` follows CORS. A page could open an event stream on any
  other origin, including services on localhost or the local network,
  and read every event; a cross-origin stream now opens only when the
  response's `Access-Control-Allow-Origin` is `*` or the page's origin.
* The JavaScript bytecode cache can no longer load a half-written file.
  Every process writing the same script used one temporary file name, so
  two tabs compiling the same library at once could publish a file
  spliced from both, and the bytecode loader does not validate what it
  runs. Each write now uses its own temporary file and the entry carries
  a SHA-256 of its bytecode, checked before loading; old entries are
  ignored.
* WebGL, WebGPU and Web Audio no longer touch memory a page has freed.
  `readPixels`, `compressedTexImage*`/`compressedTexSubImage*`,
  `setBindGroup` and realtime `AudioBufferSourceNode` rendering took a
  pointer into a page's ArrayBuffer and then converted a later argument
  (or read a property) whose `valueOf` or getter could transfer or
  resize that buffer, leaving the pointer dangling for a GPU write or
  read. Arguments are now converted before the pointer is taken. A
  numeric data argument to `compressedTexImage*` was handed to OpenGL as
  an unpack-buffer offset even with no unpack buffer bound, where the
  driver reads it as a raw memory address; that now raises
  `INVALID_OPERATION`.
* WebGPU no longer reads past the end of a page's typed array. A
  length-tracking view (`new Uint8Array(rab)` over a resizable
  `ArrayBuffer`) kept reporting its original byte length after the buffer
  was shrunk with `resize()`, so `queue.writeBuffer` and
  `queue.writeTexture` copied the old length out of the now-smaller
  allocation and the page could read the adjacent heap back out of a GPU
  buffer. The engine now reports such a view's live length, and the two
  WebGPU uploads clamp the copy to the backing buffer's current size.
* A page can no longer abort the WebGPU renderer through
  `queue.submit()`. Resolving the command-buffer list ran page getters
  that could resubmit or free a command buffer while the outer call was
  still reading it, and a command buffer listed twice was handed to
  wgpu-native twice; both reach a wgpu-native path that aborts the
  process on the resulting validation error. The whole list is now read
  before any command-buffer handle is used, and an invalid,
  already-submitted or repeated command buffer raises a WebGPU validation
  error instead of reaching that path.
* `GPUBuffer.getMappedRange()` can no longer abort the WebGPU renderer.
  Calling it on a buffer that is not mapped, with an offset or size that
  is not correctly aligned (offset a multiple of 8, size a multiple of
  4), or with a range outside the region the buffer was mapped with
  handed those arguments straight to a wgpu-native call that aborts the
  process on the resulting validation error. The arguments are now
  checked against the buffer's live mapped range and bad input throws an
  `OperationError` in JavaScript instead.
* The Linux renderer sandbox no longer lets a compromised renderer talk
  to local services. Landlock does not cover `connect()` on UNIX
  sockets, and the seccomp filter allowed `socket()` of any family, so
  code running in a renderer could reach the session D-Bus and ask it to
  start programs outside the sandbox. The renderer may now only create
  internet and netlink sockets (DNS falls back from local resolver
  sockets to `/etc/resolv.conf` as libc does when they are missing), and
  `clone` can no longer create namespaces.
* A web page could read local files. The XML parser kept the engine's
  private `data-nd-*` attributes, so a document from `DOMParser`,
  `responseXML` or an XHTML/SVG page could forge the path behind an
  `<input type=file>` and read anything the renderer can open,
  including the cookie jars. The XML parser now drops those attributes,
  as the HTML parser already did.
* Frames fetch with their own origin. A frame's `fetch()` called the
  embedding page's `window.fetch` with the frame's window as `this`, so
  an embedder that replaced `fetch` received a cross-origin frame's
  window and every request it made, and the request was checked against
  the top page's origin instead of the frame's. Each frame now has its
  own native `fetch`.
* Cached pages keep their security headers. A response served from the
  HTTP cache (fresh, or revalidated with a 304) lost its
  `Content-Security-Policy`, `X-Frame-Options`, `X-Content-Type-Options`,
  `Content-Disposition`, `Content-Language` and `Refresh`, so a page's
  second load ran without its CSP. They are now stored with the cache
  entry; the cache is rebuilt once on upgrade.
* Smaller hardening: cookies from an IP-address host can no longer set a
  `Domain` that is a suffix of the address; newlines in the home page or
  search engine setting can no longer add lines to the config file;
  `NS_HTTP3_INSECURE`, which turned off certificate checks for HTTP/3,
  is gone; `XMLHttpRequest.send()` re-checks its method and headers.
* Web pages can no longer change settings or wipe browsing data. A page
  that navigated to `about:settings-save` (a link, `location`, a form
  POST, a `Refresh`) reached it with no referrer, which the about-page
  handler took for the browser's own UI, so any site could set the home
  page and search engine, turn off HTTPS-First, or clear history and
  cookies through `about:settings-clear`. Navigations from web content to
  `about:settings*`, `about:config`, `about:history` and `view-source:`
  are now refused unless they start from a browser page, and the save
  and clear endpoints only act on a POST. The safe-browsing "continue
  anyway" link likewise only works from the warning page for that host.
* The Windows diagnostic log no longer records browsing. The log file
  that is on by default on Windows also received every document address,
  every connection's origin and every `console.log` line, in private
  windows too, and grew without limit. By default it now keeps only
  warnings and errors and starts over past 4 MiB; `NS_LOG_FILE=<path>`
  still writes the full event log.
* WebGPU is on by default, like WebGL. Pages got `null` from
  `navigator.gpu.requestAdapter()` unless WebGPU was switched on in
  Settings or the browser was started with `--enable-webgpu`, so three.js
  fell back to its WebGL2 backend ("WebGPU is not available, running
  under WebGL2 backend"). The Settings switch still turns it off. The
  setting is now saved as `webgpu`; the old `webgpu_enabled` line that
  earlier releases wrote out with every save is ignored, so existing
  installs get the new default too.
* Google Maps draws its map on NVIDIA graphics. WebGL 1 shaders have
  no `#version` line, and on desktop OpenGL they went to the driver
  as-is; Intel's driver read them as GLSL ES anyway, but NVIDIA's read
  them as desktop GLSL 1.10, where `precision highp float;` and
  `invariant gl_Position;` are errors. Every map program failed to link
  and the map stayed an empty beige page, while laptops whose browser
  ran on the integrated GPU were fine. Shaders without `#version` are
  now marked `#version 100` (GLSL ES 1.00) on every desktop OpenGL
  driver, as on macOS before.
* Web Audio plays sound. `AudioContext` was a stand-in: `resume()` did
  nothing, `currentTime` stayed at 0 and no sample reached the speakers,
  so games and players built on it, Qwasm's Quake among them, were
  silent. A running context now renders its graph in 256-frame blocks
  about 120 ms ahead of its clock and streams the result as PCM to the
  `nordstjernen-audio` helper, which resamples it into its mixer.
  `ScriptProcessorNode` calls `onaudioprocess` once per buffer (the path
  Emscripten's SDL2 audio uses), `AudioBufferSourceNode`s and oscillators
  start and stop at their scheduled times, and stereo buffers keep both
  channels. As in Chrome, a context starts suspended until the page has
  had a user gesture; `suspend()` and `close()` work. Offline rendering
  is unchanged.
* A YouTube video no longer skips its first seconds. The window took
  frames from the video helper only while drawing the video, and while
  the page was still loading it drew no page layers and so no video.
  The helper's eight-frame queue filled, its clock kept running, and
  when the page appeared about two seconds of video were dropped at
  once. The window now takes the current frame on every display tick,
  whether or not it draws.
* YouTube's player controls work while a video plays. When the video
  helper was playing, the window ignored the page changes the renderer
  reported and repainted the page only every 300 frames (2.5 to 5
  seconds), so hovering the video never showed the control bar, and the
  progress bar, the time and captions stood still. Page changes now
  repaint during playback, at most 30 times a second; the video itself
  still advances at full rate in the compositor.
* Opacity and colour animations no longer make scripts relayout the
  page. Each animation tick marked the computed styles stale, and the
  next geometry read (`clientWidth`, `getBoundingClientRect()`, ...)
  restyled and laid out the whole page. YouTube reads `clientWidth`
  twice in every animation frame while its ambient-mode canvases fade,
  so the renderer spent 40% of its time in full relayouts during
  playback. These animations are painted from the animation engine, so
  they now only mark computed styles stale for `getComputedStyle()`.
* An animation tick counts as a page change only when an animated value
  moves. Every running transition or animation reported a change on
  every tick, so the page was repainted 60 times a second even when the
  value stood still, as with `steps()` easing or the hold before a
  delayed start. YouTube's ambient-mode cross-fade runs with
  `steps(75)` over five seconds and now repaints 15 times a second
  instead of 60.
* The video helper decodes on all CPU cores. libavcodec decodes on one
  thread unless asked otherwise, which is not enough for 1440p or 4K
  VP9 and AV1 at 60 frames a second.
* YouTube videos no longer jump back and stutter about 20 seconds in.
  The video helper decodes the MSE stream from a file that grows as the
  page appends segments, but the MP4 and WebM demuxers stop at the size
  they saw when the file was opened. The helper then reopened the file,
  sought to a keyframe a few seconds back and decoded those frames
  again, so up to 230 frames were dropped and the picture went back in
  time. Now only the demuxer is reopened: the decoder keeps its state
  and continues with the packet after the last one it received.
* Videos decoded by the video helper are scaled on the GPU. The window
  drew every frame through cairo, which scaled it on the CPU and then
  uploaded the result, about 20 ms per 1080p frame, so a 60 fps YouTube
  video showed 40 to 55 frames a second and the window process used
  64% CPU. Each frame is now a texture that GTK scales while it
  composites: all 60 frames reach the screen and the window process
  uses 25%.
* Canvases drawn from timers update on screen by themselves. The
  renderer only reported a new frame after `requestAnimationFrame`
  callbacks, animations, images or layout changes, and the repaint a
  WebGL draw asks for went nowhere, so a game whose main loop runs on
  `setTimeout` (Qwasm, Emscripten builds with vsync off) showed a new
  frame only when a click forced a repaint. The same request now also
  lets media play, mute and volume changes repaint at once.
* A `MessagePort` that leads to a worker can carry other ports and
  `File`s. Such a port encoded every message as if nothing were being
  transferred, so a port in the message failed with "DataCloneError:
  value could not be cloned", and ports transferred the other way arrived
  missing. Comlink sends its callbacks exactly that way, so libarchive.js
  never opened an archive and Qwasm (qwasm.m-h.org.uk) stopped before
  downloading its game data; both LibreQuake and the original shareware
  Quake now unpack and run.
* Release packages work with immutable GitHub releases. The release
  workflow ran when a release was published and then attached the .deb
  and .zip packages, which an immutable release refuses, so the 1.0.30
  release got no Linux packages. Pushing a version tag now builds the
  packages and attaches them to a draft release, with the changelog
  section as its notes; publishing that draft is the last, manual step.

1.0.30:
======
* WebGL points take their size from `gl_PointSize`. On desktop OpenGL
  the vertex shader's point size is ignored unless `PROGRAM_POINT_SIZE`
  is enabled, and compatibility contexts also need `POINT_SPRITE` for
  `gl_PointCoord`; WebGL and OpenGL ES always behave as if both were on.
  Neither was enabled, so every particle drew as a single pixel sampled
  at one texel and most point-sprite scenes came out empty
  (`webgl_points_waves`, `webgl_custom_attributes_points`,
  `webgl_interactive_points`). Both are now enabled when a desktop GL
  context is created.
* Worker messages may be up to 256 MiB. `postMessage()` between a page
  and its workers threw `DataCloneError: message is too large` above
  16 MiB, which a decoded mesh easily exceeds: three.js's Draco decoder
  posts the vertex arrays of a large model back from its worker, so
  `webgpu_postprocessing_sss` never showed its statue.
* Type selectors with capital letters match case-sensitive elements
  wherever they sit in a selector. Selector type names are stored
  lowercased, and elements outside the HTML namespace (XML documents,
  SVG) are compared case-sensitively, so `Link href` or `Folder Link`
  never matched in a `DOMParser` XML document, nor `linearGradient stop`
  in SVG; a lone `Link` only worked through a fast path. three.js's
  `KMZLoader` finds its model with
  `querySelector('Placemark Model Link href')` and failed with
  "Couldn't find .dae file". The name as written is now kept beside the
  lowercased one and used for those comparisons.
* WebAssembly runs on WAMR's fast interpreter instead of its classic
  one, many times faster: three.js's `webgpu_loader_gltf_compressed`
  model (Meshopt geometry, five KTX2 textures transcoded by the Basis
  wasm module) loads in about one second instead of 27, and
  `webgl_loader_ifc`, whose web-ifc parse used to run into the script
  time limit, now shows its building. The fast interpreter precompiles
  each function into a register-based form at load time; the WebAssembly
  JavaScript API tests pass exactly as before.
* An antialiased WebGL canvas shows everything drawn to it when the page
  leaves the scissor test on. The canvas renders into a multisampled
  buffer that is resolved with a framebuffer blit before it is shown or
  read back, and blits honour the scissor test, so only the last scissor
  rectangle reached the screen. Pages that draw several views into one
  canvas with `setScissor()` (three.js's multiple-views and
  `webgl_loader_texture_ktx2` examples) showed a single tile. The
  resolve now runs with the scissor test off.
* WebGPU supports the `subgroups` feature. wgpu-native implements
  subgroup operations on Vulkan, Direct3D 12 and Metal but offers them
  as a native feature rather than the standard one, so `subgroups` was
  never listed, and its WGSL front end rejects the `enable subgroups;`
  directive that WGSL requires before the subgroup built-ins. The
  feature is now listed and requestable when the adapter has the native
  one, and `subgroups` is taken out of a shader's `enable` directives
  (blanked, so error positions stay put) before naga sees it.
  `subgroupAdd()` and the other built-ins run, and three.js's
  `webgpu_compute_reduce` runs every kernel without validation errors.
* A WebAssembly function has one JavaScript function object.
  `table.get(i)` and `instance.exports` built a new wrapper on every
  access, so `table.get(0) === table.get(0)` was false. Emscripten
  checks its JavaScript copy of the function table against
  `table.get()` and aborts with "JavaScript-side Wasm function table
  mirror is out of date!", which stopped web-ifc and three.js's
  `webgl_loader_ifc` at start-up. Wrappers are now cached per instance.
* WebGL 2 `readPixels()` reads into a bound `PIXEL_PACK_BUFFER` and
  honours the destination offset, and `IMPLEMENTATION_COLOR_READ_FORMAT`
  / `_TYPE` describe the bound read framebuffer. The offset form of
  `readPixels()` was dropped, and with a pack buffer bound a typed-array
  destination was passed to GL as a buffer offset; the read format was
  queried from the driver, which answers 0 for integer render targets.
  three.js's `readRenderTargetPixelsAsync()` uses all three, so GPU
  picking (`webgl_interactive_cubes_gpu`) threw on every frame. Integer
  targets now report `RGBA_INTEGER` with `INT` or `UNSIGNED_INT`, float
  targets `RGBA`/`FLOAT`, others `RGBA`/`UNSIGNED_BYTE`.
* `toDataURL()` and `toBlob()` export what a WebGL or WebGPU canvas
  shows. Both read the canvas's 2D drawing surface, which a 3D canvas
  never draws to, so screenshots of a WebGL or WebGPU scene (three.js
  "save image" buttons, thumbnail generators) came out fully
  transparent. They now take the same surface `drawImage()` uses: the
  GL framebuffer read back for WebGL, the presented texture for WebGPU.
* `canvas.toBlob()` hands its callback a real `Blob`, from a task. The
  callback ran synchronously, before `toBlob()` returned, and received a
  plain object carrying `size` and `type` but not `Blob.prototype`, so
  `instanceof Blob`, `blob.arrayBuffer()`, `URL.createObjectURL()` and
  `createImageBitmap()` rejected it — three.js's screenshot and texture
  export code paths failed.
* New WebGL renderbuffers start cleared, as the WebGL specification
  requires: depth to 1.0, color and stencil to 0. The storage came from
  the driver uninitialised, usually zero, so a render target whose depth
  buffer was never cleared rejected every fragment. three.js's
  `PMREMGenerator.fromScene()` renders the six cube faces that way, so a
  `RoomEnvironment` environment map came out black and models lit only
  by it — `webgl_animation_keyframes`, most GLTF viewers — drew as black
  silhouettes. A renderbuffer is cleared the first time it is attached
  to a complete framebuffer after its storage is (re)allocated.
* WebGL `drawBuffers([gl.BACK])` and `readBuffer(gl.BACK)` work on the
  default framebuffer. A WebGL canvas draws into an offscreen
  framebuffer object, where `GL_BACK` is not a valid draw or read
  buffer, so the call failed with `INVALID_ENUM` — three.js makes it
  whenever it switches back from a multiple-render-target pass to the
  canvas. `GL_BACK` now maps to the canvas's color attachment.
* WebAssembly runs in more than one thread at a time on Windows. The
  WAMR runtime marks its per-thread state `__declspec(thread)`, which
  MinGW GCC ignores, so the stack boundary of the first thread to run
  WebAssembly was used for every other one, and the "current execution
  environment" was shared between threads. A module called from a second
  worker failed at once with "native stack overflow" — three.js's Draco
  mesh decoder, which runs in a pool of workers, never decoded a model
  (`webgl_animation_keyframes` stayed empty). GCC builds now use
  `__thread`.
* WebGPU adapters and devices report `core-features-and-limits`.
  wgpu-native is a full core implementation on Vulkan, Direct3D 12 and
  Metal but does not list the feature, so three.js took the device for a
  compatibility-mode one and generated compatibility WGSL: depth
  textures bound as `texture_2d<f32>` and gathered without a component,
  which naga rejects. `webgpu_postprocessing_ao` and other depth-reading
  passes failed to build their pipelines. The feature is now listed on
  every non-OpenGL backend, and a `requiredFeatures` entry for it is
  accepted there.
* Large progressive JPEGs decode. Wuffs needs a work buffer of about
  nine bytes per pixel for a progressive JPEG, and the decoder refused
  any work buffer over 64 MiB, so a 4096x2048 texture such as three.js's
  `earth_day_4096.jpg` failed with "could not decode image". The cap is
  now 256 MiB, matching the largest pixel buffer the decoder accepts.
* SVG elements that never draw anything report no box to scripts.
  `getBoundingClientRect()` of an `<animate>`, `<set>`,
  `<animateTransform>` or `<animateMotion>` returned a 0x0 rectangle at
  its parent's origin and `getClientRects()` one entry, so about 520
  animation elements in IMDb's icons had boxes that Chrome does not give
  them. The same held for `<mpath>`, `<discard>`, `<view>`, unknown SVG
  elements and everything inside them, and a `<tspan>` hidden by
  `display: none` or `systemLanguage` reported the box of its whole
  `<text>`. Now only the element types SVG 2 lists as renderable, with
  rendered ancestors, get a box; the others report `0,0 0x0` and an
  empty list, as in Chrome.
* Superscripts, subscripts and inline text with `vertical-align` are
  placed as in Chrome. `<sup>` and `<sub>` were shrunk twice, to 0.75em
  by the default style and by three quarters again when drawn, and
  raised by a fixed amount chosen by tag name, so footnote markers came
  out a third too narrow and their line did not grow; `vertical-align`
  on other inline elements was ignored. The default style now gives them
  `font-size: smaller` and `vertical-align: super` or `sub`. An inline
  element (not an inline block or image) is shifted from its parent
  box's baseline by `super` (up a third of the parent's font size plus
  one pixel), `sub` (down a fifth plus one), a length, a percentage of
  its own `line-height`, `text-top` or `text-bottom`; shifts nest, and
  the line grows to hold the element unless its `line-height` is 0.
  Images and inline blocks with `super` or `sub` use the parent box's
  font size too; `top`, `middle` and `bottom` on inline elements are not
  handled yet. In 16px Arial at `line-height: 1.5`, `<sup>raised</sup>`
  is 36px wide instead of 25px, and a line holding it and a `<sub>` is
  30.5px tall instead of 26.4px, as in Chrome.
* `position: relative` with `top`, `right`, `bottom` or `left` now moves
  an inline element such as a `<span>`, `<a>` or `<sup>` with
  `display: inline`. The offset was ignored, so an icon label nudged
  with `top: -2px` or a link shifted sideways stayed where the line put
  it. The element's text, background, border, underline, the images and
  inline blocks inside it, and an absolute box that sits at its static
  position inside it are now drawn at the offset, above the rest of the
  line like other positioned boxes (below the line's text with a
  negative `z-index`). Clicks on the moved text, selection,
  `getBoundingClientRect()` and `scrollWidth`/`scrollHeight` follow the
  moved element, and nested offsets add up, while the line keeps its
  height and the text after the element stays in place, as in other
  browsers. For example `<span style="position: relative; top: -10px">`
  is drawn 10px above the line.
* A site benchmark compares Nordstjernen with Chrome on sixty of the most
  visited sites (`scripts/sitebench/`, see `docs/sitebench.md`). It
  captures each site in headless Chrome and in Nordstjernen, scores
  visual parity from screenshots, component placement and text, and
  compares first paint with Chrome's First Contentful Paint, main-thread
  CPU and peak memory, writing an HTML and Markdown report with
  side-by-side shots and filmstrips. Sites that show headless Chrome a
  bot challenge are reported but kept out of the averages.
  `nordstjernen --headless --timing` now prints per-phase timings
  (fetch, parse, style, script, layout, images, paint) and the time of
  the first painted frame.
* Restyling after a DOM change touches only what the change can affect.
  A class, id or attribute change used to restyle the changed element's
  whole subtree whenever any selector named it in an ancestor position,
  so toggling a theme class on `<body>` recomputed every style on the
  page; inserting a child under a parent that `:first-child`-style
  selectors watch did the same. Changes now follow Chrome's
  invalidation sets: only descendants matching the affected selectors'
  subjects are restyled, an element whose computed style comes out
  unchanged leaves its subtree alone, and an insertion restyles the
  parent's children rather than its subtree. `:has()` rules without an
  ancestor key no longer switch incremental restyling off for the page.
* An incremental restyle no longer crashes when an element marked for
  restyling is freed before the next style pass. The marked set kept
  the freed pointer and the pass walked its ancestors; a page that
  replaced marked elements while a blocking script was still loading
  (Yahoo Japan) crashed about one load in three. Freed nodes now leave
  every restyle set.
* A full restyle costs half as much on design-system-heavy pages. Rules
  whose subject is `:root` or an `:is()`/`:where()` of keyed selectors
  are indexed instead of being tried against every element, stylesheets
  whose keys cannot match an element are skipped, `var()`-substituted
  declarations are parsed once per pass, identical custom-property maps
  are shared instead of chained as deep as the DOM, and custom-property
  values are no longer expanded just to check for CSS-wide keywords. A
  full restyle of GitHub's front page drops from 230 to about 110 ms and
  Stack Overflow's from 108 to 56 ms.
* A `:has()` rule no longer makes every DOM change restyle the whole
  page. Stack Overflow's `html:has(.disable-document-scroll)` made each
  inserted node - including every node the parser hands back after a
  script - mark the entire document dirty, and CNN's 600 `:has()` rules
  did the same to whole page sections. Like Chrome, a mutation now
  invalidates a `:has()` anchor only when it touches an element carrying
  a class, id, tag or attribute named in that anchor's own `:has()`
  argument (arguments with sibling combinators, structural
  pseudo-classes or unkeyed compounds stay conservative), and only what
  the rule can reach is restyled: the anchor itself when it is the
  rule's subject, the descendants matching the subject's key when only
  descendant combinators follow it, and its following siblings only
  when sibling combinators do. Anchors are looked up by key instead of
  being tested one by one against every ancestor of every change.
  Removing an element between `.a` and `.b` now re-matches
  `.a:has(+ .b)`. A script that inserts nodes and reads a computed style
  200 times under such a rule went from 4.2 s to about 0.7 s; on CNN's
  front page script time drops from 14 to 9 s and first paint from
  15.6 to 10.6 s.
* Changing an element's style no longer restyles everything inside it.
  When an element's computed style changed, every descendant was
  recomputed even if only a non-inherited property such as `padding`
  changed. Now, like Chrome, children are recomputed only when an
  inherited property, `display`, a custom property, or a property a
  child explicitly `inherit`s changed, and the check repeats one level
  down, stopping wherever a child's style comes out the same. An inline
  `style` change marks only its element unless a selector tests
  `[style]` outside its subject. A script that sets `body.style` and
  reads `offsetHeight` spends 6-7 ms on styles instead of 110 ms on
  CNN's and GitHub's front pages.
* A style flush after a small DOM change costs a fraction of what it did.
  Subtrees with nothing dirty in them are no longer walked element by
  element - their previous styles are carried over in one pass - the
  table of previous styles is updated in place instead of being copied,
  the stylesheet scan skips elements that cannot be a `<style>`,
  `<link>` or frame, pages without `@font-face` skip the font-usage walk,
  and the animation check is cached per style. Together with the `:has()`
  change, 200 insert-and-`getComputedStyle()` cycles on a 3,000-element
  page drop from 4.2 s to about 0.23 s.
* `getComputedStyle()` lays the page out only when it has to. Reading a
  property whose value does not depend on layout (colour, display,
  fonts and most others) now recomputes styles alone, so a script that
  toggles a class and reads a computed colour in a loop runs about five
  times faster; width, height, margins, paddings, insets, transforms and
  grid tracks still trigger layout as in other browsers.
* Scripts inserted by other scripts are fetched in parallel and run when
  they arrive, instead of being fetched one after another and run
  inline; component stylesheets are no longer re-parsed on every
  relayout, CSSOM rule insertion is batched, CSS property names are
  looked up in a hash table, libcurl keeps the CA store between
  connections, and font-family and `lang`/`dir` lookups are memoised per
  pass. BBC's front page loads in 3.3 s instead of 25 s.
* Fonts resolve the way Chrome resolves them on Linux and Windows:
  generic `serif` and `sans-serif` map to Times New Roman and Arial (or
  their metric-compatible Liberation equivalents), so text wraps at the
  same widths as in Chrome.
* Yahoo's front page no longer goes blank. `onerror` was missing from
  the event-handler accessors on `HTMLElement.prototype`, so an
  ad-recovery script that wraps that accessor threw on every script's
  `onerror` assignment and wiped the page.
* Reddit renders. A `<slot>` inside an inline element of a shadow tree
  (`<devvit-wrapper><slot></slot></devvit-wrapper>` around the whole
  app) dropped every block-level element slotted into it, and inline
  slotted text was lost too; slots now pass their assigned content
  through, and an inline shadow host joins the line it sits in.
* `var()` substitution keeps token boundaries, so a minified value such
  as `var(--weight)var(--size)/var(--line-height)var(--font)` no longer
  fuses into one invalid token: Roblox's headings were 16px instead of
  56px. A custom property declared empty (`--x:;`) substitutes to
  nothing instead of invalidating the declaration, and a `font`
  shorthand containing `var()` keeps its weight and line height.
* Netflix's "Get Started" button and email field are styled. Rules
  inserted through `insertRule()` (CSS-in-JS libraries) lost a
  shorthand set with `var()` after earlier values of the same property,
  and `cssText` folded `border-color: var(--b)` into a resolved colour.
* Twitch's search box has its rounded corners. The CSSOM rebuilt an
  `insertRule()` sheet from each rule's declarations one property at a
  time, and a `var()` shorthand that a later longhand partly overrides
  (`border-radius: var(--r); border-top-right-radius: 0`) has no value
  of its own, so it came back as `border-radius: ;`. Rules now keep
  their serialized text. `style.length` and `style[i]` are answered
  from a cache instead of re-serializing the whole style per call, and
  a value containing `;` (`url(data:...;base64,...)`) no longer adds a
  bogus property name.
* Pages that keep their theme in one long `style` attribute and read it
  back property by property are fast again: Twitch holds about a
  thousand custom properties in a 47 KB style and read each one by
  rescanning the text. Custom-property reads on long styles go through
  a per-string index, and other reads are memoized; Twitch's headless
  CPU time drops from 22-26 s to about 17 s.
* Inset box shadows are drawn as specified: inside the padding box, with
  the offset and spread shaping the unshadowed hole, its corners
  following the shrunken radii, and blur softening the edge. They were a
  fixed 4px stroke along the border edge that ignored spread, so the
  common `inset 0 0 0 1px` focus or field ring came out 2px wide.
* Form controls look and measure like Chrome's. A text input that is a
  box of its own (`display: block`, `flex` or `inline-block`) no longer
  applies its padding twice, so text starts at the content edge, and a
  `display: flex` or `grid` input, select or textarea lays its text out
  as a field rather than as a flex item stuck to the top (Twitch's
  search placeholder). An unstyled text field draws Chrome's 1px
  rounded frame instead of a 2px inset border. Checkboxes and radio
  buttons are 13px widgets with Chrome's default margins and look - an
  accent-filled box or ring and dot - independent of the font size, and
  no longer drawn over a ballot-box glyph whose fallback font set the
  width of the space next to it; `getBoundingClientRect()` reports
  their 13x13 box.
* `getComputedStyle()` resolves colours that default to `currentcolor`
  - the border, outline, column-rule and text-decoration colours, and
  `caret-color: auto` - to the element's colour, where it returned an
  empty string or `auto`, and serializes the `border`, `border-top`
  (and the other sides) and `outline` shorthands as Chrome does
  (`2px solid rgb(10, 20, 30)`, `rgb(10, 20, 30) solid 1px`, an empty
  string for `border` when the sides differ). Shorthands such as
  `border` and `outline` read as strings instead of `undefined`.
* `<slot>` is `display: contents`, as in every browser's default style
  sheet, and lays out the nodes assigned to it in place: slotted
  elements become flex or grid items of the shadow tree's container, a
  slot's own padding no longer applies, and fallback content shows when
  nothing is assigned. Slots used to make a block of their own, so a
  web component's flex row stacked vertically and absolutely positioned
  media in Reddit's posts sized itself to the whole page, covering the
  feed with one video.
* JavaScript modules load their import graphs in parallel. Each import
  used to be fetched only when the engine reached it, one round trip
  after another; now every module source is scanned for its static
  imports as soon as it arrives and those are requested at once, and a
  page's module scripts start fetching their graphs when parsing ends.
  The scan skips comments, strings, template literals and regular
  expressions, and a prefetch follows the page's Content-Security-Policy
  and never fetches http: modules for an https: page.
  Deferred scripts that are ready run back to back instead of one per
  event-loop turn with a relayout between each. On microsoft.com, which
  has 224 module scripts, 27 of them run within the benchmark's settle
  window instead of 17, and its main-thread CPU drops from 9.9 s to
  7.4 s.
* Calling `Intl.DateTimeFormat()` and the other Intl constructors
  without `new` no longer leaves a stray TypeError behind. The
  constructor read `prototype` off its undefined receiver, failed
  silently and returned a working object, and the pending error then
  surfaced in whatever script ran next: on microsoft.com, Microsoft
  Clarity's call aborted a web-component module mid-evaluation.
  `MutationObserver`, `IntersectionObserver`, `ResizeObserver` and
  `PerformanceObserver` called without `new` now throw a TypeError as in
  Chrome, and module load failures are logged with their stack.
* Headless page loads fetch images while the page settles, as the
  browser window does: each new layout requests the images it now needs,
  and their `load` events fire as they arrive. Images used to be fetched
  only once, just before the first screenshot, so a page that swaps a
  placeholder for the real picture in an `onload` handler (BBC's lazy
  photos) often had not done so when the final shot was taken.
* Unquoted font family names may contain a generic family keyword after
  their first word, as in `font-family: BBC Reith Serif, serif`. Any
  such word made the whole declaration invalid, so BBC's headlines and
  body text fell back to the default font instead of BBC Reith Serif. A
  name is now rejected only when its first word is `serif`,
  `sans-serif`, `cursive`, `fantasy`, `monospace`, `system-ui`, `math`
  or `-webkit-body`, which is where Chrome draws the line.
* `querySelectorAll` and `querySelector` with a selector list over the
  document use the class and tag indexes when every selector ends in a
  class or a type, checking only the elements that carry them instead of
  matching the whole list against every element. Google's ad library
  queries the selector text of every CSS rule this way; on ESPN that was
  10,000 calls and 3.4 s, and the page's main-thread CPU drops from
  about 20 s to 16 s.
* Blurred shadows paint up to 16 times faster. Inset shadows now blur
  a strip of the box and stretch its uniform middle, as outer shadows
  already did, instead of blurring the full box - Reddit draws one on a
  1280x2300 element - and the blur's vertical pass runs along rows
  instead of down columns. A page of assorted inset and outer shadows
  paints in 49 ms instead of 801 ms, pixel for pixel the same, and
  Reddit's paint time drops from 1.3 s to 0.1 s.
* A web component whose host is `display: inline-block`, `inline-flex`
  or `inline-grid` sits in the line like any inline block. Elements with
  a shadow root were treated as blocks whenever their display generated
  a box of their own, so a row of such components stacked one per line,
  and MSN's shortcut icons rendered as empty circles.
* Identical CSS declaration blocks are parsed once. Web components that
  carry the same stylesheet in every shadow root - Reddit puts one
  239 KB sheet in 93 of them - had that sheet rewritten for each host
  and every declaration reparsed, a quarter of the page's CPU time.
  Parsed blocks are now kept by their text and shared between rules;
  Reddit's main-thread CPU drops from 18 s to 13.5 s, and a page with 40
  such components loads in 1.2 s of CPU instead of 4.7 s.
* Image requests advertise only the formats the build can decode. The
  `Accept` header always listed `image/avif`, so in a build without
  libavif, CDNs that negotiate formats (AliExpress served 50 of its
  images this way) sent AVIF that then failed to decode.
* `text-decoration: inherit` works, so Tailwind's preflight removes the
  underline from links (Yahoo's navigation and headlines were all
  underlined), and outlines follow `border-radius` like Chrome's (pill
  search boxes drawn with `outline` were square).
* `text-overflow: ellipsis` only clips text that cannot wrap. Wrapping
  text (`normal`, `pre-line`, `pre-wrap`) wraps as in other browsers
  instead of being cut to one line with a visible line-break glyph, and
  a `<br>` in non-wrapping ellipsized text starts a new line.
* A column flex item with `margin-top: auto` moves its content down with
  it (Google's logo sat at the top of its box), stylesheet `<link>`s
  inserted by the HTML parser fire `load`, and `document.fonts` is a
  single per-document `FontFaceSet` that dispatches loading events, which
  claude.ai waits for before showing its app.
* Pages with WebM background video (claude.com) no longer crash the
  renderer: the in-process video frame buffer is padded for swscale's
  vector writes. The sandbox lets the renderer read a CA bundle named by
  `CURL_CA_BUNDLE` or `SSL_CERT_FILE`.
* quickjs-ng is at 0.17.0 plus upstream master (`60984dc`). It fixes a
  use-after-free in `AsyncDisposableStack`, a reference-count bug in
  `Promise.withResolvers`, an out-of-bounds read when creating typed
  arrays and a use-after-free when a context runs out of memory.
  `Reflect.set` with an undefined receiver returns false instead of
  throwing, bound functions keep their target's prototype, `Date.parse`
  accepts the narrow no-break space that locale-formatted times contain,
  and `Iterator.from` gives a string's or plain iterable's iterator the
  helper methods. Integer-keyed objects no longer pile into a few hash
  buckets, and the interpreter stays fast when built with GCC 14 or later.
  IndexedDB records stored by earlier builds remain readable, although
  the engine's serialization format changed.
* WebGPU can be switched on in Settings, next to WebGL, instead of only
  by starting the browser with `--enable-webgpu`. It stays off unless
  turned on, and `about:nordstjernen` says how it was enabled.
* WebGL can upload compressed textures: the S3TC/DXT (also sRGB), RGTC,
  BPTC, ETC2/EAC and ASTC extensions are offered wherever the graphics
  driver decodes those formats, and `compressedTexImage2D()`,
  `compressedTexSubImage2D()` and their 3D forms work instead of always
  failing. Games, map viewers and three.js scenes that ship DDS or KTX2
  textures show them instead of untextured surfaces.
* A WebGPU canvas is copied back from the GPU only when the page has
  drawn into it since the last paint, through a buffer kept for the
  canvas, instead of a full new copy into a freshly allocated buffer on
  every repaint of the page. `getCurrentTexture()` returns the same
  texture until the canvas is next presented, as the specification
  requires.
* Mistakes in a page's WebGPU commands no longer abort the renderer. A
  command buffer that failed validation, for example because a shader
  did not compile, or one submitted a second time used to stop the
  process inside wgpu-native, which took three.js's ambient-occlusion
  example down; they are now reported as validation errors and skipped.
  Destroying an occlusion query set, as three.js's occlusion example
  does every frame, also aborted the renderer.
* WebGPU reports errors the way pages expect. `pushErrorScope()` and
  `popErrorScope()` capture real validation, out-of-memory and internal
  errors as `GPUValidationError`, `GPUOutOfMemoryError` and
  `GPUInternalError` objects (they always reported "no error"), errors
  outside a scope fire an `uncapturederror` event on the device (the
  device is now an `EventTarget`, with `onuncapturederror`), and only
  unhandled ones are written to the log, at most twenty per device.
  `device.destroy()` destroys the device and resolves `device.lost`, and
  popping an empty error-scope stack rejects instead of aborting the
  renderer.
* WebGPU render bundles work: `device.createRenderBundleEncoder()`, the
  `GPURenderBundleEncoder` drawing commands and `executeBundles()` on a
  render pass. three.js records the passes that build texture mipmaps as
  bundles, so every textured three.js WebGPU scene, including its
  sprite, material and model examples, stopped at the first texture with
  "not a function"; they now render. Textures also report
  `textureBindingViewDimension`.
* WebGL offers the common extensions: instanced drawing, vertex array
  objects, 32-bit indices, float and half-float textures with linear
  filtering, depth textures, min/max blending, standard derivatives and
  float render targets in WebGL 1, and float and half-float render
  targets, float blending, parallel shader compilation and context-loss
  simulation in both versions. Only the debug-renderer and anisotropic-
  filtering extensions existed, so WebGL 1 pages that need instancing or
  float textures fell back or failed, and three.js could not render to
  the HDR targets its post-processing uses.
* The Linux and macOS clang builds compile again: WebGPU limit fields,
  half-float canvas readback and dynamic bind-group offsets were read
  through misaligned pointer casts that `-Wcast-align` rejects; they
  are now copied with `memcpy`.
* Pages that upload WebGPU textures without giving `rowsPerImage`, which
  three.js does for every texture, no longer abort the renderer: the
  missing value reached wgpu-native as an invalid zero and it stopped
  the process. `writeTexture()` writes to the mip level, layer and
  position the page names instead of always the top-left of the first
  layer, origins given as `{x, y, z}` are read correctly, and command
  encoders gain `copyBufferToTexture()`, `copyTextureToBuffer()` and
  `clearBuffer()`. Render passes support `drawIndirect()` and
  `drawIndexedIndirect()`, compute passes
  `dispatchWorkgroupsIndirect()`, and `queue.onSubmittedWorkDone()`
  exists.
* WebGPU bind groups support cube-map, array and 3D texture bindings,
  multisampled and integer textures, storage textures and dynamic buffer
  offsets; every texture binding used to be declared as a plain 2D
  texture, so cube-mapped reflections and skyboxes failed to bind, and
  dynamic offsets passed to `setBindGroup()` were dropped. A buffer
  binding with an offset but no size now covers the rest of the buffer
  instead of overrunning it, and a texture or buffer can be bound
  directly.
* The WebGPU interface objects (`GPUDevice`, `GPUTexture`,
  `GPURenderPassEncoder`, `GPURenderBundleEncoder` and the rest) exist
  as globals, as in other browsers. three.js tests command encoders
  against them, so its WebGPU renderer stopped on a `ReferenceError` at
  its first frame; the instancing and custom-lighting examples now
  render. WebGPU methods live on the interfaces' prototypes, so creating
  a render pass or a buffer no longer builds a fresh set of functions
  each time.
* WebGPU knows every texture format of the specification, including the
  signed-normalised, integer, packed, depth/stencil and BC, ETC2 and
  ASTC compressed formats; unknown names used to turn silently into
  `bgra8unorm`. Textures report their real `format`, `dimension`,
  `mipLevelCount`, `sampleCount`, `usage` and `depthOrArrayLayers`,
  `viewFormats` are honoured, and a canvas can be configured as
  `rgba16float` (HDR) as well as `bgra8unorm` or `rgba8unorm`.
* WebGPU render pipelines honour their full state: stencil tests and
  operations, depth bias (used against shadow acne), front-face winding,
  strip index formats, unclipped depth, multisample masks and alpha-to-
  coverage, and overridable shader constants. Vertex buffers can use
  every WebGPU vertex format; previously anything but a few float and
  uint32 formats was read as three floats, which scrambled packed
  normals, colours and skinning weights. A shader stage without an
  `entryPoint` uses its module's only entry point, and
  `createRenderPipelineAsync()` and `createComputePipelineAsync()`
  exist.
* WebGPU render passes honour `setViewport()`, `setScissorRect()`,
  `setBlendConstant()` and `setStencilReference()`, which were ignored,
  so split views, UI overlays and the stencil effects of three.js draw
  where the page asks. A render pass can write to several colour targets
  at once, as deferred shading and three.js multiple-render-target
  effects need, and its depth/stencil attachment honours the stencil
  load, store and clear values and the read-only flags. Occlusion
  queries can be started and ended inside a pass.
* A WebGPU canvas keeps showing what was drawn into it after the page
  lets go of its context object; it used to go blank at the next garbage
  collection, and calling `getContext('webgpu')` again created a second,
  empty context. WebGL contexts are kept alive by their canvas the same
  way. `device.limits` reports the device's real limits instead of
  zeros, `adapter.features` and `device.features` list what the GPU
  supports, and `requestDevice()` honours `requiredFeatures` and
  `requiredLimits`, so three.js can use compressed textures and float32
  filtering. Invalid usage flags on a buffer or texture are reported as
  a validation error instead of aborting the page's renderer, and a
  failed `mapAsync()` rejects its promise.
* WebGPU is now part of every Linux, macOS and Windows build, as WebGL
  already was. When wgpu-native is not installed, the build downloads
  the pinned release for the platform and links it into the browser, so
  no extra library has to ship beside it. FreeBSD, NetBSD, musl-based
  Linux, Android and iOS, which wgpu-native publishes no release for,
  still build without it. WebGPU stays off until the browser is started
  with `--enable-webgpu`.
* Text falls back through the page's own `font-family` list. Only one
  family of the list was handed to the text engine, so a character that
  family lacks came from the system's fallback font and never from the
  next family the page names: in `font-family: Icons, Georgia, serif`
  the letters were in neither Georgia nor a serif. Every available
  family of the list and its generic family are now passed on in the
  page's order. With that the list is also followed past a missing
  Arial, Helvetica, Segoe UI, Roboto or SF Pro name, which used to end
  it with a stand-in, the default sans-serif font or for SF Pro the
  system font: `'Segoe UI', SegoeUI, Arial` never reached the page's
  own `SegoeUI` web font, and `'Roboto Mono', monospace` was drawn in a
  proportional font. System emoji fonts named in the list are left out,
  emoji being drawn in the emoji font wherever they stand, so text in
  `font-family: 'Apple Color Emoji'` is no longer a row of boxes; a
  page's own font under an emoji font's name, such as Google Fonts'
  Noto Emoji, keeps its place in the list. A bare `fangsong` is a
  family name like any other, as in Chrome.
* JPEG XL images are displayed, as Chrome now does. `.jxl` files decode
  through libjxl, an optional dependency like libavif, in `<img>`, CSS
  backgrounds and `<picture>` sources typed `image/jxl`; transparency is
  kept, lossy images are decoded to sRGB, and animated JPEG XL
  plays like an animated GIF or WebP. Image requests advertise
  `image/jxl` in their `Accept` header so servers can pick it. A build
  without libjxl, or with `-Djxl=disabled`, carries no JPEG XL code.
* A page that declares several `@font-face` rules for one family is
  rendered with the same faces on every load. A variable font file named
  by one rule per weight had every named instance in it registered as
  that rule's weight, and faces of one family that matched a request
  equally well were ordered by which download finished first: text on
  YouTube and Trendyol came out regular in one load and light or bold in
  the next, and a `unicode-range` subset file could lose to a fallback
  font. A variable file with a weight, width or optical size axis is
  now registered as its variable face alone, and of the faces that
  match equally well the one declared last is used, as in other
  browsers.
* The home page set in Settings is saved and used. A text field never
  fired `change` when it lost focus or on Enter, so Settings, which saves
  on `change`, dropped a typed home page; text inputs and textareas now
  commit a user edit with `change` before `blur`, as other browsers do.
  Settings has a Save button for the General fields again, each control
  saves only its own value so a second Settings tab no longer writes back
  stale ones, and the Home button opens the configured page instead of
  always `about:start`.
* A frame's `history.pushState` and `replaceState` act on the History
  object they are called on. YouTube binds the methods of a hidden
  `about:blank` frame to the top window's history, so every navigation
  to another video changed that frame's URL and left the page's URL,
  `history.state` and back button on the first video.
* `ResizeObserver.disconnect()` and `unobserve()` work. Both were bound
  to a no-op on the prototype, so a disconnected observer kept firing;
  YouTube's like counter threw a `TypeError` from such a callback on
  every navigation.
* The address bar, tab title and back button follow same-document
  navigations. The renderer reported a page's URL and title only when it
  opened, so after YouTube moved to another video with
  `history.pushState` the shell kept showing the first video; the
  regular tick now carries both, and a pushed entry becomes a back step.
* Inline elements paint their own box: background over the padding,
  `border-radius`, and borders, sliced at line breaks so only the first
  and last fragment get the left and right edges. Before, an inline
  element's background covered only its glyphs and its borders were not
  drawn or given space at all. YouTube's description link chips and
  Stack Overflow's inline `code` now get their rounded, padded
  background.
* A translucent background on an inline element is painted with its
  alpha. Pango's background attribute is opaque, so YouTube's
  `rgba(0,0,0,.05)` link chip in a video description became a solid
  black bar that hid its text.
* `white-space` on an inline element applies to its own text. The
  whole inline run used the mode of its first node, so a `pre-wrap` span
  inside a normal block lost its line breaks, and a `normal` span inside
  a `pre-wrap` block kept its runs of spaces. YouTube's
  `yt-formatted-string[split-lines]` relies on this for the blank line
  between paragraphs of its notices and descriptions.
* `::before` and `::after` inside inline content get their own box
  model. An inline pseudo-element now keeps its horizontal margin and
  padding, so YouTube's `89K • Streamed 1mo ago` and El País's section
  separators are spaced as in Chrome instead of running together. An
  `inline-block` pseudo-element inside an inline element is laid out as
  a box instead of flattened into text, which brings back the Guardian's
  pulsing live dot; and a pseudo-element's padding and margin are no
  longer counted twice when its width is measured, which made Amazon's
  video controls and YouTube's ad badge 8-10px too wide.
* An inline-block in a centered or right-aligned `white-space: nowrap`
  line reports its real position to scripts and hit testing. Paint
  already aligned the line, but layout placed the inline-blocks as if it
  were left-aligned, so BBC's centered section menu reported x=0 instead
  of 259 and YouTube's search tab icon sat 8px left of its button center.
* Text and inline-blocks that share wrapped lines are painted on the
  line boxes that layout computed. Paint drew each line at Pango's own
  line height and each inline-block at Pango's position, so with a CSS
  line-height the second and later lines drifted away from the layout:
  a link after a wrapped paragraph on YouTube floated above the
  baseline and text overlapped the inline-blocks around it. Text on a
  single line is drawn as before, and the horizontal position still
  comes from Pango so `text-align` keeps working.
* An image or SVG with a percentage width no longer widens the boxes
  around it when they are sized to their min-content. Such a replaced
  element is compressible: its min-content contribution is zero, as in
  Chrome. Before, a YouTube channel name next to a 100%-wide verified
  badge kept the full name width, so the badge drew on top of the text.
* Flex layout stops laying the same item out again when nothing it
  depends on changed. A row flex item was laid out twice, once to
  measure its height and once at its final place, and a stretched item
  a third time with its new definite height; nested flex containers
  multiplied that, so a 1,500-box YouTube page took 37,000 box layouts
  per relayout. The second pass now moves the already laid out subtree
  when the width is the same and nothing inside read the item's
  definite height, and an inline-block is laid out a second time only
  when the first pass changed its size. A forced relayout of a saved
  YouTube watch page drops from 74 ms to 8 ms; the layout of 6,587
  flexbox, grid, alignment, sizing and inline WPT files is unchanged.
* Fewer styles are recomputed after DOM changes. Setting an attribute to
  the value it already has no longer restyles anything (YouTube rewrites
  the same custom properties into `ytd-watch-flexy`'s `style` attribute
  dozens of times, each restyling ~2,500 elements); removing a child
  restyles only the `:has()` anchors around it unless a structural
  selector applies; and a parent with more than 64 children is no longer
  restyled whole on every insertion, because the structural-selector
  check now scans all children with hash lookups instead of giving up.
* YouTube restyles incrementally again, about five times faster per
  forced layout. One `:has()` selector whose compound had no class, id,
  type or attribute to key on (`:not(:has(...))`, or a bare `:has(> x)`
  after a descendant combinator) turned incremental restyle off for the
  whole page, so every `offsetWidth` read recomputed all ~4000 styles.
  Such an anchor now keys on the compound that holds the `:not()`, or on
  the nearest keyed compound to its left. An anchor also has to match
  every key of its compound, not just the first: `#content.x:has(...)`
  used to match YouTube's top-level `#content` and restyle the whole app
  after any change.
* `-webkit-line-clamp` clamps. `display: -webkit-box` with
  `-webkit-box-orient: vertical` and a line clamp now lays out as a
  block container whose text stops at the clamp line with an ellipsis,
  as in Chrome, instead of a flex row that showed every line. YouTube's
  "N chapters" rows spilled their whole chapter list over the results
  below. `-webkit-box-orient: vertical` without a clamp stacks the
  children, and `getComputedStyle` reports `-webkit-box` as written.
* An empty block that starts a new formatting context (`display: flex`,
  `grid`, `flow-root`, `overflow: hidden`) stops margins from collapsing
  through it, so a later child's top margin no longer escapes to the
  parent's top edge.
* Underlines, overlines and line-throughs skip inline-blocks and images
  inside a link: an avatar inside a link no longer gets a stray line
  under it.
* SVG presentation attributes (`fill`, `stroke`, `stroke-width`,
  `stop-color`, `visibility` and the other paint attributes) take part
  in the cascade as presentational hints. A value inherited from an
  ancestor no longer beats the element's own attribute: YouTube's
  icon wrapper sets `fill: currentcolor`, which painted the red play
  button of the YouTube logo black. `getComputedStyle` reports the
  attribute value as well.
* CSS masks take several layers. The `mask` / `-webkit-mask` shorthand,
  `mask-clip` and `mask-composite` (with the legacy `-webkit-` keywords)
  are parsed per layer, and gradient layers are composited the way
  Chrome does: each layer inside its own clip box, with `add`,
  `subtract`, `intersect` or `exclude`. The common "border only"
  idiom, `linear-gradient(#fff 0 0) content-box exclude,
  linear-gradient(#fff 0 0)`, now leaves a thin rim. YouTube's buttons
  use it for their rim light, which was painted as a gradient band
  across the whole button.
* `querySelector` and `querySelectorAll` find every element whose
  selector ends in an id when the id is used more than once.
  `document.querySelectorAll('#owner #avatar')` returned nothing on
  YouTube, which repeats ids such as `avatar`, `content` and `text` in
  every component, because only the first element with that id was
  tested.
* A timer or animation frame callback belongs to the window whose
  `setTimeout` or `requestAnimationFrame` was called. When an iframe's
  script called a function of its parent that scheduled one, the
  callback was tied to the iframe and silently dropped once the iframe
  was removed, which could leave the parent's scheduler waiting forever.
* Inserting a `DocumentFragment` (`appendChild`, `insertBefore`),
  `replaceChildren()` and setting `textContent` now invalidate the
  styles that depend on sibling position. Before, an item that stopped
  being `:last-child` because a fragment was appended after it kept its
  `:last-child` style.
* The toolbar, tab-strip and address-bar icons render on systems without
  librsvg's GdkPixbuf loader (Debian/Ubuntu `librsvg2-common`), which a
  KDE or minimal desktop often lacks. GTK hands SVG icons, symbolic ones
  included, to GdkPixbuf, so without that loader every icon was GTK's
  image-missing placeholder, with no warning. The shell now draws its
  bundled icons through the in-engine SVG renderer (`src/gtk/icons.c`)
  at the screen's own scale, recolouring the symbolic ones to the theme's
  foreground as GTK did, and sets the window icon only when GTK can load
  it. Ported from Northstar (northstar-browser#16).
* The geometry APIs report an inline element's own box.
  `getBoundingClientRect()`, `getClientRects()`, `offsetHeight` and
  the rest returned the rectangle of the line a `<span>`, `<a>` or
  `<code>` sat on, so with `line-height: 1.6` a 16px link measured
  22px high and started at the line's top. They now return the content
  area of the element's font (ascent plus descent around the baseline,
  rounded as other browsers round them) plus its padding and border,
  the same box the background is painted in, and a line that mixes
  text directions is measured over all of it. That painted box also
  follows a raised or lowered `<sup>` / `<sub>`, is left open on the
  side its `direction` says when it wraps, and is no longer painted
  for a `visibility: hidden` inline. A border width without a border
  style no longer takes up room on the line.
* A printable key fires one `keydown`, not two. Embedders send a
  keydown and then a keypress (`kind` 3) for each typed character, and
  the keypress path dispatched `keydown` again before `keypress`, so
  every letter typed into a page reached its `keydown` listeners twice
  in the GTK and Java shells. Kind 3 now fires only `keypress`, as
  `libnordstjernen.h` documents, and is skipped when the keydown before
  it was cancelled.
* Clicking an `<a download>` link saves the file instead of navigating
  to it. Only a scripted `element.click()` honoured the attribute; a
  real mouse click followed the link like any other.
* Java browser: the page's own shortcuts no longer swallow the
  browser's. Alt+Left, Alt+Right and Alt+Home did nothing while the page
  had the focus, because the page view consumed every arrow and Home
  key before the window's key bindings ran.
* Java browser: Ctrl+X / Ctrl+V (and Shift+Insert) cut and paste in text
  fields, and the right-click menu of a field offers Cut, Copy and
  Paste. Text a page copies (`navigator.clipboard.writeText`, a copy
  button) reaches the system clipboard.
* Java browser: downloads work. The renderer reports a download as the
  URL and the page's suggested file name separated by a tab, which the
  Java shell handed whole to `URI.create`, so every page-initiated
  download was silently dropped. The save dialog now opens in the
  Downloads folder with the suggested name, and `data:` and local
  `file:` downloads are saved too.
* Java browser: Space pages down (Shift+Space up) when no text field has
  the focus, and the arrow, Page and Home/End keys move the caret in a
  focused field instead of also scrolling the page. Pages receive
  `keyup` for every key, `code` `"Space"` for the space bar, and AltGr
  characters such as `@` on European layouts on Windows.
* Java browser: zoom reflows the page like the GTK shell — the CSS
  viewport is the window width divided by the zoom, the zoom steps
  through the same 25–500 % ladder, and resizing the window, opening the
  find bar or showing the horizontal scrollbar relays the page out.
  Pages render sharp on HiDPI screens: frames are rasterised at the
  screen's scale factor and `window.devicePixelRatio` reports it.
* Java browser: page audio plays through `nordstjernen-audio`, which the
  shell starts beside the renderer and silences on navigation, as the
  GTK shell does. A page can take the window full screen (Esc leaves),
  a middle click opens a link in a new window, touchpad scrolling and
  Shift+wheel sideways scrolling work, animations refresh at about
  30 frames a second, and a link clicked while a page is loading is
  followed once the load finishes instead of being ignored.
* Java browser: an address typed without a scheme is normalised like
  the GTK shell's — an existing local path opens as a `file:` URL,
  `localhost` and `host:port` are hosts, and the command-line start URL
  is normalised too.
* Java library: `RemotePage.render` and `renderRgba` return the frame
  again when called twice for the same region; the second call used to
  come back fully transparent because the renderer answers an identical
  request with `X-Unchanged` and no pixels. Renderer headers are decoded
  as UTF-8. `RemoteBrowser` adds `paste`, `cutSelection`,
  `clipboardText`, `openContextMenu`, `setDevicePixelRatio` and
  `audioCommands`, and frames report `downloadName`, `clipboard`,
  `audio` and `requestedScrollX`; the new `AudioHelper` plays a
  session's audio for any embedder.
* Android: pages no longer stay blank and white after a load until they
  are reloaded. The progress bar resizes the page view as a load starts
  and ends, and every resize gives the view a fresh, white bitmap. When
  a render of the previous page was still running as the new page
  arrived, the new page's render request was dropped, so the white
  bitmap stayed on screen; animated pages such as `about:nordstjernen`
  hit this most. The request is now kept, and a frame the renderer
  reports as unchanged is copied into a bitmap that has not yet received
  it, so a view resized away and back is repainted too.
* Icon fonts keep the ligatures that start with "s". ns-pango hands
  HarfBuzz made-up glyph ids for characters a font lacks, and HarfBuzz
  14.4 stores per-lookup coverage answers under the low 14 bits of the
  id. In a font without a space glyph, the made-up id for a space was
  remembered as "not covered" under glyph 32, the letter "s" in Material
  Icons, so once any text with a space had been shaped in that font,
  `search`, `settings`, `star` and `share` were drawn as plain words
  while other icons kept working (Britannica, Google). ns-pango now
  gives HarfBuzz per-call stand-in ids below that range and writes the
  made-up glyphs back afterwards. `subprojects/ns-pango.wrap` moves to
  the ns-pango commit that carries the fix; it also brings the fork's
  shape and item cache fixes and the merge of upstream Pango 1.58.2.
* A `::before` or `::after` with text content that becomes a flex or
  grid item paints its background once, on the item box, sized by its
  `width` and `height`. The generated text painted the background again
  over its own line box, so Tripletex's hidden `width: 0; height: 2px`
  focus underlines showed as blue bars under every input.
* `@font-face` rules honour their `font-weight` and `font-style`
  descriptors. A downloaded face was registered under its CSS family
  with whatever weight and slant the font file reports for itself, so
  when one family listed several files the choice between them followed
  the files' metadata rather than the stylesheet, and a face declared
  `font-weight: 700` whose file calls itself regular was emboldened
  again on top. The declared weight and style now describe the face.
  A face without the descriptor keeps what the file reports, and so
  does the weight of a face declared with a weight range or backed by a
  variable font. FINN's category labels were drawn in a bolder face
  than the regular one they ask for, Spotify's headings in the wrong
  face of their family, and the New York Times' summaries came out
  about ten pixels wider per line than in their declared face.
* Absolutely and fixed positioned elements assigned to a shadow root's
  `<slot>` find their containing block in the flattened tree, through the
  slot and its shadow ancestors, instead of among their light-DOM
  ancestors. A slotted `position: absolute; inset: 0` scroll container
  was placed against the viewport and painted under the component's
  opaque layout, so Tripletex showed its menus but no page content.
* Elements inside an inline `<svg>` report their geometry to scripts.
  `getBoundingClientRect()` returned an empty rectangle at 0,0 and
  `getClientRects()` an empty list for every `<g>`, `<path>`, `<rect>`,
  `<use>` and `<text>`, `getBBox()` ignored the transforms of child
  elements and knew nothing about `<use>`, `<text>`, `<image>` or a
  nested `<svg>`, and `getCTM()` / `getScreenCTM()` carried only the root
  `viewBox` scale, without `preserveAspectRatio`, element transforms,
  nested viewports, the root's padding and border, CSS transforms or page
  scroll. All of them are now measured by the SVG renderer itself, with
  the transforms, `viewBox` fitting, `<use>` and `<switch>` handling it
  paints with, so a script that positions a tooltip, measures a chart
  label or checks whether an icon is on screen gets the box that is
  drawn. A path arc that ends where it starts is skipped instead of
  drawing a stray curve, and CSS rotations by a multiple of 90 degrees
  use exact sines and cosines.
* SVG `systemLanguage` is honoured. An element whose `systemLanguage`
  list names none of the user's languages is no longer drawn, and
  `<switch>` picks the first child whose language matches instead of
  always the first one, so a translated diagram shows the reader's
  language rather than whichever translation comes first in the file.
* Clicking into the address bar selects the whole address, as in other
  browsers; a second click places the cursor, and dragging still selects
  part of the text.
* The title bar shows the version after the browser name
  ("Nordstjernen Browser 1.0.29"; just "Nordstjernen" when space is short).
* 32-bit builds: the selector-cache hash folded a pointer-sized value
  with `>> 32`, which is undefined on i386 and put every key in one
  bucket, so each lookup scanned the whole cache. The fold is now done
  in 64 bits. minimp3's short-block scalefactor loop states its array
  bound, so a 32-bit LTO link no longer fails `--werror` on
  `-Wstringop-overflow`. Ported from northstar-browser.
* No more 64-bit format and pointer-type warnings on macOS in the
  IndexedDB, Temporal and WebAssembly code. Ported from
  northstar-browser.
* `<picture>` is an ordinary inline element again and the `<img>` inside it
  is the image, as in other browsers. The image used to be laid out with
  the `<picture>` element's style at its natural size, so the `<img>`'s own
  `width`/`height` attributes and CSS (`width: 100%`, `max-width`,
  `position: absolute`, ...) were ignored: hero images covered headlines,
  card images grew to twice their height and product shots were squeezed
  instead of cropped. A `<picture>` without an `<img>` now renders nothing.
  `<source>` selection is unchanged.
* An inline `<svg>` that has only a `viewBox` (no `width`/`height`) no
  longer stretches the box around it. Floats, inline-blocks, buttons,
  absolutely positioned boxes, `auto` grid tracks and table cells measured
  it at the full width of the page, so icon buttons became hundreds of
  pixels wide and drew their icons as huge black shapes. Such an SVG now
  adds no width to its container's min- and max-content size, as in other
  browsers, takes its width from a definite `height` and its aspect ratio,
  and still fills the line of a flex container or an ordinary block.
* An image, video or SVG with a percentage width (`width: 100%` and the
  like) inside a box that sizes itself to its content -- a float, an
  inline-block, an absolutely positioned box, an auto table column, a
  flex or grid item -- adds its natural width to that box's max-content
  size and nothing to its min-content size, as CSS Sizing specifies (an
  SVG without a natural width counts as 300px here, a definite height
  and aspect ratio take precedence). It used to add the percentage of the
  page width, so such a box grew to the full width and the image was
  drawn at that width.
* A `::before` or `::after` with `display: block` (or `flex`, `grid`)
  and text content is a block box of its own. It used to be laid out as
  inline text at the start of the element's first line or the end of its
  last one, so it did not start a new line, and its width was added to
  the text's in shrink-to-fit sizing instead of the wider of the two being
  taken. Tabs that reserve their bold width with a hidden block `::before`
  grew by about half, and headings and counters drawn with block pseudo
  content ran into the text beside them.
* Table cells (`<td>`, `<th>`) get their `::before` and `::after`
  generated content. It was never built for cells, so sort arrows,
  `content: attr(data-label)` labels in responsive tables and icons drawn
  in a cell's pseudo-element were missing.
* CSS grid placement by name follows the spec. `grid-column: content`
  finds the lines named `content-start` and `content-end` (the implicit
  area that line names create), the end edge of `grid-row: top / nav` or
  `grid-area: a / 1 / b / -1` resolves to `nav-end`/`b-end` instead of
  the area's start line, and an item with only an end line
  (`grid-row-end: main`) is placed against it. Such items used to fall
  back to auto-placement, which squeezed the Guardian's front-page
  containers into 60px columns.
* Grid rows are sized from single-row items first and then from spanning
  items, smallest span first, and a spanning item's extra height goes to
  rows that have not reached their content height limit before it is
  shared out evenly. An empty row next to a spanning header no longer
  takes half of the header's height.
* `flex` with only a basis (`flex: 100%`, `flex: 30px`,
  `flex: calc(...)`) sets `flex-grow: 1` and `flex-shrink: 1`, as the
  shorthand defines. The grow factor stayed 0, so such items did not grow
  into the free space of their flex line.
* A single-value `overflow` (`overflow: hidden`) now sets `overflow-x`
  and `overflow-y`, so it takes part in the cascade like any shorthand.
  Any `overflow-x`/`overflow-y` from another rule used to win over it,
  even when the `overflow` rule was `!important` or more specific, so
  `overflow: hidden !important` utility classes did not clip.
  `element.style.overflowX` and a rule's `style.overflowX` read back the
  value as well.
* A custom property whose value is `var(--b, initial)` (or `inherit`,
  `unset`) sees `--b` when `--b` is a plain value declared later in the
  same rule or in the inline style. The fallback keyword used to win and
  the property was treated as unset, so Netflix's card grid lost its
  `flex-basis` and stacked the cards.
* Form controls get the default look other browsers give them:
  `<button>`, `<input>` and `<select>` use a 13.33px system font and
  `<textarea>` a 13.33px monospace font instead of the page's font, buttons have `1px 6px` padding and a
  2px border, and `<select>` and `<textarea>` a 1px border with Chrome's
  padding. Unstyled buttons and search boxes were drawn in the page's
  16px serif with wider padding, and fixed-size icon buttons left too
  little room for their icon.
* A `<video>` whose metadata has not loaded uses the 300x150 default
  object size when only its width or only its height is set, as in other
  browsers. `<video style="width: 100%">` without a loaded source used to
  be as tall as it was wide, a square blank box that pushed the page down.
* A wrapping flex container (`flex-wrap: wrap`) with a percentage height
  resolves it against its containing block's height. It used to take the
  percentage of its own width, so a one-line header with `height: 100%`
  became as tall as it was wide and `align-items: center` pushed its
  items hundreds of pixels down.
* An absolutely positioned box placed at its static position inside a
  flex or grid container, or in a right-to-left block, now moves with its
  content. Only the box itself was moved, so its text and children stayed
  where block layout had first put them: search placeholders and icons
  were drawn below their input.
* Table cells with `display: none` no longer take a column. They were
  still built into their row, so a hidden responsive header cell
  (`colspan="2"`, `width: 100%`) widened GitHub's file table past its
  container and pushed the commit message and date columns out.

1.0.29:
======
* The about:nordstjernen splash shows version 1.0.29.
* Clipboard in text fields: paste with Ctrl+V, Shift+Insert or Cmd+V on
  macOS, and from a new right-click menu on inputs, textareas and
  contenteditable elements (Cut, Copy, Paste, Select All). Ctrl+X cuts and
  Ctrl+C, Ctrl+Insert and Select All act on the focused field's text
  instead of the page. Pasting fires a cancelable `paste` ClipboardEvent
  whose `clipboardData` holds the text, honours `readonly` and `maxlength`,
  and turns line breaks into spaces in single-line inputs. Password fields
  cannot be copied or cut. `beforeinput`/`input` are now InputEvents with
  `inputType` and `data` for typing, deleting, pasting and cutting, and
  `DataTransfer` treats the `text` and `url` formats as `text/plain` and
  `text/uri-list`.
* Fixes for the HiDPI, layout and smooth-scrolling changes:
  - Keyboard scrolling (arrows, Page Up/Down, Space, Home/End) moves the
    page again instead of the scroll container under the mouse pointer.
  - On scroll-snapping pages one wheel notch or key press moves one snap
    point; the eased scroll used to skip about a dozen sections.
  - Zooming out below about 35% no longer lays the page out narrower and
    shorter than the window.
  - A page restored with Back after the window moved to a screen with a
    different pixel density picks up the new density, `resolution` media
    queries re-apply when the density changes, and out-of-range device
    pixel ratios are clamped instead of ignored.
  - Absolutely positioned children of 3D-transformed elements are drawn
    inside the transformed element again.
  - `vw`/`vh` inline styles read back from `element.style` follow
    viewport resizes; `getComputedStyle()` on a `display: none`
    `::before`/`::after` returns its real values.
  - A crafted WOFF2 file can no longer decode to a font of over 64 MB.
  - A float in the first paragraph of a block whose top margin collapses
    through (common in blog posts: `<p><img style="float:left"></p>`) no
    longer drops below the text beside it, and blocks below every float
    no longer rebuild the float bands (pages with many floats laid out up
    to seven times slower).
  - `<hr>` beside a float stops at the float's edge again (UA
    `overflow: hidden`, as in other browsers); column-flow grids measure
    their min-content with their own font; very large `line-height`
    values no longer overflow to a zero-height paragraph.
  - Each tab in single-process mode no longer commits its whole
    framebuffer up front, and the shell rejects frames larger than it
    asked for.
* Pages are drawn at the screen's real pixel density. On a Retina or other
  HiDPI display the renderer used to paint at one device pixel per CSS pixel
  and the window stretched the frame, so text and images were blurry. The
  frame is now rendered at the window's scale factor (2x on a Retina Mac,
  fractional scales on Linux) and shown pixel for pixel, re-rendering when
  the window moves to a screen with another scale. `devicePixelRatio`,
  `resolution` and `-webkit-device-pixel-ratio` media queries and `srcset`
  report the real ratio, so pages pick their sharp images. The renderer
  framebuffer limit grows from 2560x1600 to 6144x3456 device pixels, so wide
  windows are no longer cut off at 2560 CSS pixels.
* Text flows around floats like in other browsers: a paragraph next to a
  floated figure keeps its full width and only its lines beside the float
  are shortened, so text returns to the full width below the float. The
  whole paragraph used to be narrowed for its entire height, which made
  Wikipedia articles about 50% taller than in Chrome. Floats also no longer
  get their top margin twice, and a float before a block whose top margin
  collapses through the parent moves down with that margin.
* Absolutely positioned and `display: none` table rows are taken out of the
  table. Wikipedia hides collapsed table rows that way, and they used to
  stay in the grid at full height.
* Definite `min-width` and `max-width` limit a box's min- and max-content
  width, and the flex items of a single-line row contribute their min-content
  width as the flex spec describes (clamped by their flex base size when
  they cannot grow or shrink). A centered flex column holding a heading with
  `max-width` no longer stretches to the full width.
* Lit and other web components that define their reactive properties in an
  `observedAttributes` getter work: `customElements.define` now reads
  `observedAttributes` once at definition time, as the spec says, instead of
  after the first element was constructed. Lit components on MDN never
  re-rendered, so every dropdown menu in MDN's header stayed open.
* A `display: contents` shadow host lays out its shadow tree, so content
  assigned to a hidden `<slot>` is not drawn.
* `visibility: hidden` hides `::before`/`::after` text, and in flex and grid
  containers those pseudo-elements become their own items. Stack Overflow's
  menu labels ("About", "Active") were drawn twice.
* Percentage widths of inline-blocks resolve against the line's containing
  block during layout. The real layout sized them correctly and then laid
  them out again against their own content width, so a `width: 50%`
  inline-block came out a few pixels wide (apple.com's region picker showed
  "..").
* `display: none` on `::before` and `::after` removes the pseudo-element,
  as when another rule sets its `content`.
* `::placeholder` honours `opacity` and `visibility`, and keeps the default
  grey (#757575) when a rule changes other properties. GitHub's sign-up
  field drew its hidden placeholder in black on top of its label.
* Reading an element's inline style from script is much faster:
  `style[i]`, `style.length`, `getPropertyValue()` and
  `getPropertyPriority()` no longer re-serialize and re-parse the whole
  `style` attribute on every call. bbc.com/news loads in about 6 s of CPU
  instead of 33 s; in the desktop browser it used to pass the renderer's
  30-second reply timeout, restart, and never finish loading.
* Scrolling is smooth. Touchpad deltas are applied 1:1 (they were multiplied
  by 60, so a small swipe jumped hundreds of pixels), the scroll continues
  with momentum after the fingers lift, mouse-wheel notches and the arrow,
  Page Up/Down, space, Home and End keys glide to their target, and each
  scroll rides on the next frame request instead of a separate round trip.
  The browser no longer re-runs a hover hit test after every frame while
  the pointer is still (GTK repeats the last motion event), and holds hover
  updates until scrolling stops; hovering tests only the exact point under
  the pointer. Scrolling Wikipedia went from about 9 to about 55 frames per
  second on a 2x display. Wheel and touchpad scrolling also lets the
  renderer delay page timers and relayouts while the gesture lasts, as it
  already did when the scroll position came from the browser window.
* A single-line row flex container is as wide as the sum of its items'
  min-content widths, not its widest item, when it is squeezed. GitHub's
  header menu shrank below its content and the buttons ran into each other.
  Text under `white-space: nowrap` or `pre` is never measured as if it could
  wrap, including text with letter or word spacing.
* A `grid-auto-flow: column` grid is as wide as the sum of its columns, so
  button rows like apple.com's "Learn more / Buy" are no longer cut off.
* Paint order follows stacking contexts: an element with a positive
  `z-index` inside a positioned `z-index: auto` box is painted above later
  positioned siblings, and flex and grid items with a `z-index` are lifted
  even without `position`. apple.com's hero headline and buttons were
  hidden under the hero image.
* Text uses the same fonts and widths as Chrome on macOS. `sans-serif` is
  Helvetica (it was Verdana, about 14% wider, so text wrapped earlier and
  overflowed its boxes), `serif` is Times, `monospace` Menlo, `cursive`
  Apple Chancery, and `system-ui`, `-apple-system` and `BlinkMacSystemFont`
  the San Francisco system font. Glyph advances are no longer rounded to
  whole pixels, and font sizes reach HarfBuzz as CSS pixels, so San
  Francisco gets the same optical size and tracking as in Chrome; measured
  string widths now agree with Chrome to a tenth of a pixel. `line-height:
  normal` comes from the font's own ascent, descent and line gap.
* WOFF2 web fonts load on systems whose FreeType was built without Brotli,
  such as Homebrew's on macOS. Those fonts used to fail silently and pages
  fell back to a default face (GitHub's Mona Sans showed as Verdana). The
  new in-tree decoder `src/woff2.c` unpacks the Brotli stream and rebuilds
  the transformed `glyf`, `loca` and `hmtx` tables over libbrotlidec.
* Every named instance and the variable pattern of a web font are now
  registered under its CSS family, so a variable font such as Mona Sans
  renders at the requested weight instead of its first instance.
* Zoom reflows the page like other browsers: at 150% the page is laid out
  for a viewport 1.5 times narrower instead of being magnified and cut off
  at the right edge.
* Security audit of the engine, shell and helpers. Fixed memory-safety
  bugs that web content could reach:
  - Out-of-bounds reads and writes: WebGL texture uploads sized for the
    wrong pixel format or ignoring the unpack state; a grid
    `repeat(auto-fill, ...)` track count past the stored tracks; CSS
    per-rule match arrays one slot short for `::file-selector-button`;
    `container-name` lists of 16+ names; word selection over text with
    lone surrogates; Ogg Vorbis links and libav frames whose channel
    layout changes mid-stream in the audio helper; shared-memory video
    geometry re-read after it was validated in the shell.
  - Use-after-free when page script runs in the middle of an operation:
    custom element upgrades and `whenDefined`, `replaceChildren` and
    template `innerHTML`, radio groups, inline event handlers,
    `addEventListener` with an AbortSignal getter, EventSource dispatch,
    structured clone and worker messages over resized or detached
    buffers, `getAnimations()`/`getKeyframes()`, performance entries and
    observers, IndexedDB handles, canvas state and paths, WebAssembly
    memory buffers passed to `transfer()`, WebCrypto ECDH peer keys,
    scrollbar and headless hit-testing, and queued scripts or document
    index entries for removed nodes.
  - Stack exhaustion from deep nesting: `:nth-child(of ...)`, selector
    chains, `@supports`, `@container`, `image-set()`, time values,
    `random-item()`, nested grids and inline runs. CSS nesting can no
    longer expand a small stylesheet into gigabytes of selectors.
  - Integer overflow and runaway sizes: textarea rows, fitted input
    columns, multicol splits, animation delays, SVG arcs and sizes, PDF
    page rasters, print spans, Web Audio and microphone buffers.
* More fixes from the security audit:
  - `fetch()` and message delivery take the requesting document from the
    engine, not from `this.location`, which a page could forge to reach
    about: pages, local files or another site's cookies.
  - Engine-private `data-nd-*` attributes can no longer be read or written
    through `toggleAttribute`, `setAttributeNode`, `Attr.value` or the
    namespaced attribute methods; a page could otherwise make
    `<input type=file>` read any local file the renderer can open.
  - The unused `__ndDocEnter`/`__ndDocExit` globals are gone.
  - WebGL validates draws against the GL driver's own buffer and
    vertex-array state, keeps the calling context current while page
    `valueOf` code runs, and reads the canvas back without the page's pack
    settings; WebGPU keeps borrowed handles and mapped ranges alive while
    in use.
  - Use-after-free fixes for live collections, IntersectionObserver and
    ResizeObserver callbacks, Attr wrappers, form validation, pointer lock,
    `scrollIntoView()`, `document.scripts`, `document.body`, table row and
    cell insertion, `hashchange`, frame creation, and DOM insertion methods
    whose custom element callbacks move nodes; freed nodes no longer stay
    the `:active`/`:hover`/`:focus` element.
  - Media elements only send engine-issued tokens to the audio helper, and
    long chains of safe-browsing "continue" prefixes no longer recurse.
  - Titles and URLs with invalid UTF-8 can no longer inject markup into
    about:history. An iframe's internal realm document and window are no
    longer reachable as properties of the iframe element, so a
    cross-origin frame's DOM stays out of the embedding page's reach.
  - Extension content-script natives are removed from the page even when
    the content script fails; `DecompressionStream` output per chunk is
    capped at 256 MiB; assigning to read-only accessors such as
    `element.dataset` no longer crashes; error reports copy the file name
    before running the thrown value's `toString()`; orphaned nodes are not
    swept while page script is still on the stack, and freed nodes are
    dropped from video-frame callbacks and the pending fullscreen target.
* The shell and helpers trust the sandboxed renderer less: media helper
  commands with line breaks or over-long lines are refused, saved pages
  are not copied through symlinks, cursor names are limited to CSS
  keywords, renderers no longer inherit other tabs' descriptors, and
  `file:` downloads from web pages are refused.
* Sandbox: Landlock now also restricts creating symlinks, fifos, sockets
  and device nodes, and restricts truncation; it picks the rights the
  running kernel supports instead of failing on older kernels. Seccomp
  refuses the TIOCSTI/TIOCLINUX terminal-injection ioctls.
* Web security: subresource redirects keep their top-level site (cookies,
  `Sec-Fetch-Site`, `Origin`); `document.cookie` can no longer replace
  HttpOnly cookies; about:blank/about:srcdoc no longer count as browser
  pages; HSTS applies to mixed-case URLs; the nghttp2 backend refuses
  request methods containing line breaks and caps response headers at
  1 MB; frame documents read cookies for their own URL; IndexedDB is
  refused for opaque origins (file:, data:, about:) instead of sharing
  one database between them; extension content scripts keep their
  privileged natives away from the page.
* Pages load again in the desktop browser on macOS versions whose
  `shm_open()` rejects `O_CLOEXEC`. Creating a tab's shared framebuffer
  failed there, so no renderer process was started and every page stayed
  blank with "Done" in the status bar. The framebuffer is now opened
  without the flag and marked close-on-exec afterwards in that case.
* Event listeners are indexed by their target. Dispatching an event to a
  node, dispatching to the window, and checking for a duplicate in
  addEventListener each walked every listener of the page; they now look
  only at the target's own listeners.
* IntersectionObserver entries are only built for targets whose
  intersection changed. Every relayout built a full entry with three
  DOMRects for every observed target and dropped most of them.
* The animation check after each style pass skips elements that cannot
  have anything to do: those without animation state and without an
  animation or transition property, and those whose style was reused and
  have nothing running. It no longer computes their depth and sorts them.
* Style elements inside shadow roots and frames are no longer rewritten on
  every relayout. Their CSS is flattened and scoped to the host, which is a
  pure function of the text and the host, so the result is now kept by
  that text. Pages with many such style elements relayout faster
  (Speedometer's Svelte-Complex-DOM 204 ms to 115 ms).
* Relayouts measure text once per distinct run instead of on every pass.
  The size, line count and baseline of a measured text layout are kept,
  keyed by its text, font, attribute list and layout settings, so a forced
  layout after a small change no longer shapes and breaks every paragraph
  of the page again. Speedometer's NewsSite layout time halves.
* Pages that repeat the same `<style>` many times, such as inline SVG
  icons that each carry their own style element, restyle much faster.
  Consecutive style elements are merged into one sheet, and every copy of
  a rule was matched against every element it could apply to; an earlier
  copy can never win over a later identical one, so only the last copy of
  each repeated style text is kept. A Speedometer Complex-DOM page has 416
  copies of one icon style, 98% of its selector matching.
* Shadow DOM pages restyle faster. Style sheets in shadow roots are scoped
  by an attribute on the host, so their rules read `[data-nd-host="7"] .x`;
  the ancestor filter that skips rules whose ancestors are not on the
  element's path ignored attributes, and every host's copy of a rule was
  matched in full against every element. Exact attribute values in ancestor
  position now go into the filter too. Speedometer's TodoMVC-WebComponents
  style pass takes 57 ms instead of 221 ms.
* Placing absolutely positioned boxes nested in other positioned boxes is
  faster: finding each box's static position no longer walks up the tree
  for every box it visits, but compares document order ranks and a set of
  the box's ancestors made once.
* Large blurred box shadows paint up to 250 times faster. A blurred rounded
  rectangle is the same along its straight middle, so the blur now runs on a
  copy with the middle cut short and the uniform row and column are
  repeated back to full size. TodoMVC's 550 by 6000 pixel list shadow took
  300 ms per paint and now takes about 1 ms. The pixels are unchanged.
* Blurred box shadows are drawn once and reused. Every frame blurred every
  visible shadow again, three passes over a fresh surface each; the blurred
  surfaces are now kept, keyed by size, corner radii, blur and colour, up to
  32 MB.
* Loading a frame no longer measures the whole JavaScript heap. The frame's
  `performance` object needs to know whether the parent has
  `performance.memory`, and reading it to find out walked every object of
  the runtime.
* Event dispatch no longer walks every listener of the page after each
  target it visits to clear out removed listeners when none was removed.
* Dispatching an event no longer searches the document. Looking up the
  window's `on<type>` handler fell through to the window's named
  properties whenever the page had not set one, which walked the whole
  document for a frame and then an element with that name, for every
  event.
* Relayouts skip the transition and animation check for elements whose
  style was reused unchanged and that have nothing running. An element with
  `transition: all` compared every transitionable property on every
  relayout.
* Restyles after a DOM change are much faster on pages whose elements
  declare `transition`. Every element that had ever been checked for a
  transition was restyled on every relayout, with its whole subtree, even
  when nothing was animating; now only elements whose style an animation
  or transition actually changed are. Speedometer's Complex-DOM suites run
  two to five times faster (Svelte-Complex-DOM 748 ms to 244 ms).
* The title bar always shows the browser name. "Nordstjernen Browser" sits
  centred in the window, moves to the right edge once the tabs reach the
  middle, and shortens to "Nordstjernen" when the full name no longer fits.
* The Nordstjernen "N" logo is back at the right end of the toolbar; clicking
  it opens nordstjernen.org.

* `flex: unset`, `flex: inherit` and the other CSS-wide keywords reset all
  three flex longhands. `flex-basis` kept its old value, so GitHub's
  security section drew its screenshot as an 18px wide sliver.
* Auto margins work in wrapping flex containers (`flex-wrap: wrap`): free
  space on each line goes to the items' `auto` margins, and `margin-top` or
  `margin-bottom: auto` aligns an item in its line. MDN's Baseline box
  showed the browser icons next to the label instead of at the right edge.
* Links, labels and summaries with `display: inline-block` are laid out as
  inline blocks, with their vertical padding, instead of as plain text.
  MDN's table of contents links were 19px tall instead of 32px. Inline
  blocks and images with a horizontal margin are no longer moved right by
  that margin a second time.
* Margins collapse as in other browsers. When a first child's top margin
  collapses through its parent, the parent's box now starts below that
  margin, so its background and border no longer cover it. A top margin
  also collapses with the margin above the parent, and flex and grid items
  keep their children's margins inside. The root element keeps its
  children's margins too, while `<body>` lets its last child's bottom
  margin collapse through it. MDN's page header was 16px shorter than in
  Chrome.
* Grid sizing: `minmax()` tracks keep their minimum when a grid with
  flexible columns is too narrow (MDN's three columns overflowed the
  window), a stretched grid item stops at its `max-height`, and `0fr` or
  `0.5fr` rows in a grid without a fixed height get that fraction of their
  content. The `grid-template-rows: 0fr` accordions on github.com showed
  every closed panel open. Flex and grid items with `overflow: hidden` now
  clip their content even when they are zero pixels tall.
* Each line box takes the `line-height` of every inline element on it, as
  CSS describes. A `<span>` with a larger font or line height inside a
  paragraph made its lines taller when drawn but not in the layout, so the
  text overlapped the next block; GitHub's feature descriptions were 9px
  short. A line made only of smaller text keeps the paragraph's line height.

* A box shadow without blur is drawn only outside its box. A shadow
  shifted sideways also showed a strip inside the box on the opposite
  edge, so MDN's table of contents links had a grey line on both sides.

* Solid borders whose sides have different colours meet in a diagonal
  mitre, as in other browsers, so CSS triangles drawn with transparent side
  borders show as triangles. Stack Overflow's "More" dropdown arrow was a
  black bar.

* An absolutely positioned `::before` or `::after` with `content: ""`
  no longer draws its borders and background a second time around an empty
  line of text next to the element.

* A box with `aspect-ratio` and an explicit `min-height` (or one that
  scrolls vertically) takes the height from its ratio even when its content
  is taller, as the spec's automatic minimum size describes. Content still
  grows such a box when `min-height` is `auto`.

* The experimental V8 JavaScript backend is removed. The `js_engine` and
  `v8_root` build options, `src/js_v8.cc`, docs/V8.md and the V8 CI job are
  gone; every build binds the DOM to QuickJS, with `-Dquickjs` still picking
  the in-tree quickjs-ng fork or Bellard's original QuickJS.

* `-Dquickjs=quickjs` builds again. The binding had started calling four
  quickjs-ng functions that the adapter for Bellard's original QuickJS did
  not provide (`JS_ToNumber`, `JS_GetClassCount`, `JS_NewForwarder`,
  `JS_CloneCFunction`), so `src/js.c` no longer compiled there. The adapter
  now supplies them; a frame's copies of native functions forward to the
  page's functions on that engine, as docs/quickjs.md describes.

* The window hands each page frame to GTK as a texture instead of painting
  it through cairo on every redraw. GTK had to rasterize the whole frame
  into a new image and upload it each time the window redrew, also when
  only a wheel event or a video frame changed; it now uploads a frame once.
  Scrolling a results page in a 1200x760 window on a 2x screen draws 51-54
  frames a second instead of 44-46, and the window's memory stays around
  100-130 MB instead of 120-250 MB.
* Scrolling no longer waits for the renderer. The renderer paints the page
  as tiles and keeps each `position: fixed` and `position: sticky` part in
  its own layer; the window keeps the tiles as textures and moves them
  itself, so a long task in the page's scripts no longer stops the page from
  moving, and only tiles that come into view are uploaded. After a change on
  the page only the visible tiles are painted again, and tiles whose pixels
  did not change stay on the GPU. A playing YouTube video is drawn under the
  tiles. Scrolling at 1500 px/s in a 1280x800 window on a 2x, 120 Hz screen:
  Wikipedia 56 -> 102 frames a second, GitHub 51 -> 102, BBC News 42 -> 100,
  a YouTube video page 38 -> 95, and the longest pause on the YouTube video
  page went from 509 ms to 25 ms. Pages that cannot be split this way (for
  example `background-attachment: fixed`) still get full frames;
  `NS_TILES=0` turns tiles off.
* A grid item can span more than 24 rows. `grid-row: span N` and the other
  span forms were cut to 24, the limit on explicit tracks in a template,
  although implicit rows go up to 4096. Google's image results lay their
  tiles out on 5px rows with spans of 25 to 100, so every tile was 24 rows
  tall, the images were cropped and the captions were drawn over them.
  Column spans keep the 24 track limit.
* Web fonts used only by `::before` and `::after` content are downloaded.
  The window decided which `@font-face` files to fetch from the text nodes
  in the document, so an icon font that a page only uses through generated
  content (Britannica writes every icon as `content: attr(data-icon)`) was
  never loaded, and the icon names were drawn as plain words.

* Signing in to Google works: Gmail opens after the email and password
  steps, where the sign-in page used to turn the browser away with "This
  browser or app may not be secure" after the email. Methods and
  attributes of Node, Element, Document and the other node interfaces now
  check that `this` is a real node, as WebIDL requires: called on any other
  object they throw a TypeError ("Illegal invocation"), the promise-returning
  ones reject, and the `[LegacyLenientThis]` handlers `onmouseenter`,
  `onmouseleave` and `onreadystatechange` return undefined. Before, they
  quietly returned null or 0, or ran on the wrong object, and Google's
  sign-in checks for this. `HTMLImageElement.decode()` called on something
  that is not a node now rejects at once instead of never settling.
* `meson setup` checks every hard system-library version floor -- libcurl
  8.5; with ns-pango, GLib 2.80, Cairo 1.18, HarfBuzz 8.3, FriBidi 1.0.6
  and fontconfig 2.15; with `-Dgtk=enabled`, GTK 4.14 -- before it
  configures ns-pango, and names each library that is missing or too old
  in one error. On a release older than Ubuntu 24.04 the build used to
  stop inside the ns-pango subproject on whichever floor it met first, as
  a bare GLib version mismatch. The error also points at
  `-Dns-pango=disabled`, which shapes through the system Pango instead.
* The release ships one .deb per supported distro release -- Ubuntu
  24.04 LTS, Ubuntu 26.04 LTS and Debian 13 -- named for the release it is
  for (`nordstjernen_<version>_ubuntu24.04_amd64.deb`,
  `..._ubuntu26.04_...`, `..._debian13_...`). The single .deb was built on
  Debian 13, so it depended on that release's FFmpeg 7.1 packages
  (libavcodec61, libavformat61, libavutil59, libswscale8, libswresample5)
  and on libwebp 1.5, and dpkg refused to install it on Ubuntu 24.04
  (FFmpeg 6.1, libwebp 1.3.2) and Ubuntu 26.04 (FFmpeg 8.0). FFmpeg puts
  its ABI in its package names, so no single package can depend on every
  release's FFmpeg. Each .deb is now built in a container of its own release
  against that release's libraries, then installed there and started
  headless before it is attached to the GitHub release.
  scripts/pack-deb.sh takes the release tag for the file name from
  /etc/os-release (`DEB_DISTRO_TAG` overrides it). (Reported by Danik-a-a.)

1.0.28:
======
* Scrolling no longer waits behind the page's own work. While a page is
  animating, the window asked the renderer for a page tick every frame and
  put those requests ahead of everything else, so the frames that carry the
  wheel movement waited behind them; while the user scrolls it now asks for
  frames, which run the page's work at a lower rate themselves.
* `Map` and `Set` with number keys are fast again. Small integers hashed to a
  handful of buckets, so a `Set` of a few thousand numbers searched long
  chains on every `add`/`has`/`set` (100,000 operations took about 180 ms,
  now about 5 ms).
* Pages with container queries restyle incrementally again: after a small
  DOM change only the affected elements get new styles, instead of all of
  them. Incremental restyle was turned off whenever container sizes were in
  use, which on Google's results page was every relayout; a style write
  followed by a layout read on a 4,800 element container query page takes
  35 ms instead of 45 ms.
* Scrolling stays smooth on pages that react to it. While the user scrolls,
  frames are painted from the current layout and the page's timers, scroll
  events and relayouts run at most every 250 ms (longer when they are slow);
  before, every frame waited for them, and a page that restyles its header
  on scroll, as Google's results page does, dropped to a few frames per
  second. The page sees the final scroll position as soon as the user stops.
* A grid that gets its height from `top` and `bottom` (absolutely positioned,
  no `height`) sizes its `fr` rows from that height; they were 0 px tall, so
  the items collapsed or took their content height (the picture mosaic at
  the top of a Google results page showed one picture and an empty tile).
* Percentages inside `round()`, `mod()`, `rem()` and `abs()` resolve against
  the same basis as a plain percentage. They were resolved against the
  viewport width when the style sheet was parsed, so
  `width: round(nearest, 100%, 1px)` made a box as wide as the window
  whatever its container (Google's results page laid out its header, its
  top cards and its news list too wide, over the right-hand column).
* A column flex container with `min-height` and no `height` is as tall as
  its content again; the minimum was used as the height, so the items were
  shrunk to fit it and the rest of the page was laid out over them
  (DuckDuckGo's comparison page drew its table over the introduction).
  `overflow: clip` on a flex item no longer lets it shrink below its
  content, since it does not make a scroll container.
* Relayouts of pages with container queries are about 17% faster when the
  containers keep their size (measured on a 6,000 element page relaid out
  after each style write): the selector match cache that serves the second
  container pass is no longer filled and thrown away on relayouts that need
  no second pass. It is used again as soon as a relayout needs one.
* The about:start splash shows version 1.0.28.
* Nordstjernen is now dual-licensed: it may be used, modified and
  redistributed under either the Nordstjernen Source License v1.0 or the GNU
  General Public License version 3 or (at the recipient's option) any later
  version (`LicenseRef-NSL-1.0 OR GPL-3.0-or-later`). The GPL text ships as
  `COPYING` in every package and shows at `about:gpl`; the About page and the
  Debian,
  RPM, openSUSE, Alpine, Maven and AppStream metadata declare both licenses.
* Style resolution is faster on pages with attribute selectors: whether an
  attribute's value compares case-insensitively is decided once per selector
  instead of for every element (Speedometer's Complex-DOM suites run about 18%
  faster).
* Scripts with many functions parse faster (about 9% on a 1.2 MB script of
  40,000 small functions): a nested function now shares its script's file
  name instead of looking the URL up again in the atom table.
* Reading any property of a `<form>` no longer rebuilds the list of its
  controls: forms resolve control names before their own members, so every
  `form.action` or `form.q` walked the document for the form's controls. The
  list is now cached until the DOM changes, about three times faster on a
  form with a hundred controls. Changing a control's `name` or `form`
  attribute now also updates `form.elements` and `getElementsByName()`
  collections a page already holds, which kept the old members before.
* Pages with container queries relayout in one pass instead of two when no
  container changed size: the previous relayout's container sizes are used
  for the first cascade and checked against the new layout, and only a
  change in a container's size or position among its siblings runs the second
  pass. Google's results page ran the second pass on 44 of its 45 relayouts.
* Constructed style sheets adopted by a shadow root style it again. The
  engine scopes them like a `<style>` at the end of that shadow root, so
  `:host` works, `innerHTML` set later no longer drops them,
  `adoptedStyleSheets.push()` takes effect, and a frame's adopted sheets no
  longer pile up in the top document (they made every later relayout slower;
  Speedometer's later suites ran several times slower than alone).
* `instanceof` on an interface that extends `Node`, `Element`, `HTMLElement`
  or `Document` is the ordinary prototype chain test: every HTML element was an
  instance of `SVGElement`, `HTMLUnknownElement` and `HTMLMediaElement`, so Vue
  rendered whole apps as SVG elements without `click()` (Speedometer's
  TodoMVC-Vue, TodoMVC-Vue-Complex-DOM and NewsSite-Nuxt failed). MathML
  elements are `MathMLElement`s, an HTML element named with upper-case letters
  through `createElementNS()` is an `HTMLUnknownElement`, and the events the
  engine fires (submit, wheel, keyboard, touch, drag, animation, transition,
  WebSocket close, EventSource, media query change, offline audio completion)
  have their interfaces. Frame realms get their own `customElements`,
  `WindowProperties` and stream objects, and named access on a kept frame
  window follows its navigations.
* A box that sizes itself to its content (flex, inline-block, float) counts a
  child's `calc()` width. Wikipedia's "Checked" review indicator came out
  20 px too narrow, its icon shrank to half width and the text ran into the
  lock icon.
* A grid item that spans a `1fr` row in a grid without a set height grows
  only that row, not the `min-content` rows it also spans. Wikipedia's
  Appearance sidebar spans the title, tab bar and article rows, and pushed
  large blank gaps above and below the Article/Talk tabs.
* A page with many shadow roots no longer crashes the renderer. The cache of
  parsed `<style>` sheets dropped its oldest entries when it grew past 64,
  even sheets the relayout in progress was still using (Speedometer's
  TodoMVC-WebComponents crashed as soon as it added its items).
* A style sheet styles only its own document. The page's rules no longer
  apply to the elements of its frames, and a frame's `<link>` style sheets no
  longer apply to the page.
* `loadedmetadata`, `loadeddata` and `canplay` fire once per video resource
  instead of after every layout, and `progress` fires only when the buffered
  range grows. YouTube got them several times a second and ran its player
  handlers for each one.
* A `<video>` that switches to a new source stops getting events from the old
  one. After a YouTube ad the old stream kept sending its buffered range,
  position and `ended` to the element, so the player saw the video stall,
  stopped at 0:00 and reloaded it at 144p.
* A page that registers one custom property with `@property` no longer
  makes every element that sets its own custom properties copy all the
  inherited ones. YouTube registers one and carries 1361 variables, so each
  forced layout on the watch page took 444 ms; it now takes 56 ms.
* On macOS the sandboxed renderer can create its profile directories when
  their parents do not exist yet, so history and IndexedDB work on a fresh
  profile (before, every `indexedDB.open()` failed with `UnknownError`).
* The fetcher looks for a bundled CA certificate file next to the browser
  through the executable path the app resolved at startup, instead of
  resolving that path a second time with its own copy of the code.
* The built-in pages share one modern style with light and dark colours.
  about:start is a new-tab page with the Nordstjernen mark, a large search
  field that also opens typed addresses, and shortcuts to History,
  Settings, About and nordstjernen.org; the release splash moves to the
  top of about:nordstjernen. about:settings has a section list, a short
  description under each option and switches that save as soon as they
  change. about:history groups visits by day and has a filter box, error
  pages use drawn icons instead of emoji, and the license pages match.
* The browser window has a new, flatter look that follows the light or
  dark desktop theme. The old toolbar stayed light with black text on a
  dark desktop, and the active tab drew white text on a near-white tab.
  Tabs now sit in the title bar at full width, and the selected tab joins
  the toolbar below it. The toolbar has round icon buttons drawn for
  Nordstjernen (Back, Forward, Reload, Home, Bookmarks, Downloads, Menu),
  Reload turns into Stop while a page loads, and each tab shows a spinner
  while its page loads, so the address bar no longer jumps when loading
  starts. The address bar is a rounded field with a lock or warning icon,
  the Nordstjernen mark on built-in pages and a star that bookmarks the
  page. Private tabs and the toolbar above them are tinted purple, the zoom
  level is a pill next to the address, and the find bar, status bubble and
  window buttons are rounded to match. Print stays in the menu and on
  Ctrl+P, and tabs shrink when many are open.
* Switching tabs updates the tab strip, the address bar, the window title
  and the Back and Forward buttons for the tab you switch to. The shell
  read the current tab before GTK had changed it, so after a click on a
  tab, Ctrl+Tab or Ctrl+Page Down these kept showing the tab you left.
* Content too wide for a centred line starts at the line's start and
  overflows the end edge, as CSS Text says, instead of being centred with its
  start cut off. reCAPTCHA's image challenge showed every tile one column off
  and the last column empty; it can now be solved.
* The IndexedDB interfaces (`IDBFactory`, `IDBDatabase`, `IDBObjectStore`,
  `IDBIndex`, `IDBCursor`, `IDBKeyRange`, `IDBRequest`, `IDBTransaction`,
  `IDBVersionChangeEvent` and the rest) have the shape of other browsers: their
  attributes are getters on the prototypes, their state is no longer kept in
  `_records`, `_store` and `_meta` members, `IDBRequest`, `IDBDatabase` and
  `IDBTransaction` inherit `EventTarget`, `IDBVersionChangeEvent` is an
  `Event`, and `objectStoreNames` is a `DOMStringList`. A transaction no
  longer stays open forever when a request is issued from a promise callback
  after the last one finished (idb-style wrappers never saw `complete`), a
  request's `result` throws until it is done, errors bubble from the request to
  the transaction and the database and abort the transaction unless cancelled,
  `cursor.continue(key)` lands on the key, `cursor.update()` works on stores
  with a key path, and `getAll()` takes an options dictionary.
* `MediaSource`, `SourceBuffer` and `SourceBufferList` keep their state out of
  sight, inherit `EventTarget`'s listener methods instead of carrying copies,
  and lose `appendBufferAsync()`, `removeAsync()`, `item()`, `audioTracks`,
  `videoTracks` and `textTracks`, which other browsers do not have; `handle`
  exists only in dedicated workers. Missing or empty arguments throw `TypeError`
  and an invalid `SourceBuffer.mode` is ignored, as in other browsers.
* `Cache` and `CacheStorage` are real interfaces (also in workers) whose
  operations reject for a wrong receiver or missing arguments. `ignoreSearch`
  now applies to `delete()` and `matchAll()`, `keys()` takes a request,
  `caches.match()` takes `cacheName`, and a cached response keeps its own `url`.
* `Observable` follows the current specification, and `Subscriber` exists (also
  in workers): `subscribe()` returns nothing, the subscribe callback gets a
  `Subscriber` with `active`, `signal` and `addTeardown()`, concurrent
  subscriptions to one Observable share one producer, and the operators are
  enumerable methods of the standard length.
* A document, a text node, a comment or a document fragment no longer
  has the members of an element. The element member table was installed
  on the `Node`, `Document`, `HTMLDocument` and `DocumentFragment`
  prototypes as well, so `document.click`, `document.style`,
  `document.tagName` and hundreds more existed and `Node.prototype` had
  519 members (47 in other browsers). Each of these prototypes now holds
  the members its interface defines; `CharacterData`, `Text`,
  `DocumentType` and `ShadowRoot` get the ones (`appendData()`,
  `splitText()`, `remove()`, `innerHTML` and so on) they only reached
  through `Node.prototype`, and `EventTarget.prototype`'s listener
  methods work on any node. Elements get the same treatment: what an
  HTML element interface defines (`value`, `href`, `checked`, `rows`,
  `play()`...) is on that interface's prototype and what every HTML
  element has (`style`, `click()`, `focus()`, `title`...) on
  `HTMLElement.prototype`, so a `<div>` no longer has `href` or
  `checked` and an SVG element no longer has `click()`;
  `Element.prototype` holds Element's own members.
* A redirect turns a request into a `GET` only where the Fetch standard
  says so (a `POST` on 301 and 302, anything but `GET` and `HEAD` on 303),
  and the redirected `GET` drops the request's `Content-Type` and the
  other headers that described its body. A `HEAD` request stayed a
  `HEAD` only on 307 and 308, and a `PUT` became a `GET` on 301 and 302;
  a `POST` turned `GET` still sent its `Content-Type`.
* A `POST` without a body (`XMLHttpRequest.send()` or `send(null)`, an
  empty `fetch()`) goes out with `Content-Length: 0`. libcurl read such a
  body from the renderer's standard input and sent it chunked; a server
  that does not take chunked requests then read the end of that body as
  the start of the next request on the connection and answered it with
  an error, so the next request on a kept-alive connection failed.
* `XMLHttpRequest` sends the `Content-Type` its body calls for: a string
  as `text/plain;charset=UTF-8`, a `Blob` as its own type and an
  `ArrayBuffer` or untyped `Blob` without one, instead of
  `application/x-www-form-urlencoded` for all of them. An author
  `Content-Type` on a string body has its charset changed to UTF-8, and
  a header set to an empty value is sent with an empty value instead of
  being dropped. Requests from scripts no longer get libcurl's form type
  when they have none.
* `SharedArrayBuffer` is no longer on the global object of pages,
  frames and workers. The HTML standard exposes it only to documents and
  workers that are cross-origin isolated, which no document here is
  (`crossOriginIsolated` is always false), and other browsers hide it the
  same way.
* `prepend()` with the parent's first child no longer hangs the page. The
  child was inserted before itself and became its own next sibling, so
  the next walk over the document never ended; example.com's new script
  does this, and loading it hung the renderer.
* Events the browser fires (messages from frames, workers, ports and
  broadcast channels, `XMLHttpRequest` progress, `load` and the others)
  carry a `timeStamp`, as in other browsers; it was `undefined`. Events
  at a port, worker, request or channel are at their target while their
  listeners run (`eventPhase` is 2 and `composedPath()` returns the
  target) and have no phase or current target afterwards. Messages and
  errors from a worker are trusted (`isTrusted` was false), and
  `XMLHttpRequest` progress events are `ProgressEvent`s.
* A frame without a source or with `srcdoc` shows `about:blank` or
  `about:srcdoc` as its `location.href`, `document.URL` and
  `document.location`, as in other browsers, and keeps the base URL and
  origin of the document that holds it. These frames showed the holding
  page's URL. A frame's initial `about:blank` document now reads as
  complete and same-origin with its creator from the start.
* A same-origin frame no longer sees the page's own global variables and
  functions as its own. A frame's window started with a copy of every
  property of the page's window, so a script in the frame found the
  page's `var` declarations, functions and `window.x` values (reCAPTCHA's
  frame found the page's `grecaptcha` configuration); it now gets only
  the names the platform defines.
* An asynchronous `XMLHttpRequest` a frame sends to its own origin works
  when that origin differs from the page's. The response was checked
  against whichever document the event loop was in when it arrived,
  usually the page, so it failed with status 0; synchronous requests and
  `fetch()` were not affected.
* The canvas, WebGL and geometry objects have the shape their interfaces
  define. A 2D context carried its attributes, its methods and the
  engine's bookkeeping as own properties, a gradient its stops and
  `addColorStop`, an `ImageData` or a `TextMetrics` its values, an
  `OffscreenCanvas` was an `HTMLCanvasElement`, a WebGL context carried
  about 400 members and writable constants, a `DOMRect` and a `DOMMatrix`
  their numbers, and `OffscreenCanvasRenderingContext2D`, `CanvasGradient`,
  `CanvasPattern`, `ImageBitmap`, `DOMQuad` and the WebGL object
  interfaces did not exist. `CanvasRenderingContext2D`,
  `OffscreenCanvasRenderingContext2D`, `CanvasGradient`, `CanvasPattern`,
  `ImageData`, `ImageBitmap`, `TextMetrics`, `Path2D`, `OffscreenCanvas`,
  `WebGLRenderingContext`, `WebGL2RenderingContext`, `WebGLBuffer` and the
  other WebGL objects, `DOMRect`, `DOMPoint`, `DOMQuad` and `DOMMatrix`
  (and their read-only bases) are classes with their attributes and
  methods on the prototypes, the constants on the interface and its
  prototype as read-only, WebIDL's argument counts and lengths, and a
  wrong receiver or too few arguments throwing `TypeError`. The objects
  are made in the realm of their canvas, so a canvas in an iframe gives
  the iframe's objects.
* `canvas.getContext('2d')` returns the same context each time, `null`
  once the canvas has another kind of context, and a resized canvas has a
  fresh context state. The context's attributes validate as the HTML
  standard says: an invalid value is ignored, `fillStyle`, `strokeStyle`
  and `shadowColor` read back as `#rrggbb` or `rgba(r, g, b, a)`, `font`
  reads back in pixels without a line height, and `color(srgb ...)` and
  `color(display-p3 ...)` colors parse. `new ImageData()` works, `new
  OffscreenCanvas()` is an `EventTarget` that resizes and converts to a
  `Blob`, `CanvasPattern.setTransform()` exists, and `createImageData()`
  and `getImageData()` throw `IndexSizeError` for an empty region.
* `OffscreenCanvas` and its context, `ImageData`, `ImageBitmap`, `Path2D`,
  `createImageBitmap()`, `DOMRect`, `DOMPoint`, `DOMQuad` and `DOMMatrix`
  exist in workers, where an `OffscreenCanvas` draws. `structuredClone()`,
  `postMessage()` and worker messages carry `ImageData`, `ImageBitmap` and
  the geometry objects; the other canvas objects throw `DataCloneError`.
* WebGL: `getAttachedShaders()`, `getIndexedParameter()`,
  `getSamplerParameter()`, `getTransformFeedbackVarying()`,
  `getUniformIndices()`, `invalidateSubFramebuffer()` and
  `drawingBufferStorage()` exist, `stencilFuncSeparate()`,
  `stencilOpSeparate()`, `stencilMaskSeparate()` and `sampleCoverage()`
  call GL instead of doing nothing, `copyTexImage2D()` and
  `copyTexSubImage2D()` work in WebGL 1, `drawingBufferWidth` follows a
  resize, the stencil masks read back unsigned and `getContextAttributes()`
  lists its members in dictionary order.
* CI no longer runs CodeQL: the `codeql` workflow, its configuration and
  the README badge are gone. The Semgrep badge is gone too.

1.0.27:
======
* The about:start splash shows version 1.0.27.
* The default search engine is DuckDuckGo (`https://duckduckgo.com/`)
  instead of DuckDuckGo Lite, for the address bar, the about:start search
  box and the Android app. A saved configuration still holding the old
  Lite default moves to the new one; other choices are kept.
* A CSS value with thousands of nested parentheses or functions, in an
  `@property` `initial-value` or in `CSS.registerProperty()`, no longer
  crashes the renderer. Values nested deeper than 128 levels are
  invalid.
* An SVG with thousands of nested `<g>` elements, as an `<img>` or
  inline, no longer crashes the renderer. Rendering stops descending
  after 256 nested elements.
* A page nested tens of thousands of elements deep no longer crashes the
  renderer. Font-usage collection, video discovery, container-unit
  detection, animated-value propagation and iframe cleanup walked the
  tree recursively and ran out of stack around 50,000 levels (a plain
  run of `<div>` or `<li>` was enough); they now walk it in a loop.
* The desktop browser now records visited pages in History. The shell
  recorded each page title into a history database it never opened, so
  in the default multi-process mode `about:history` stayed empty. A
  `--private` launch still records nothing.
* A link opened in a new tab from a private tab (middle-click,
  Ctrl+click or "Open in new tab") opens in a private tab. It opened in
  a normal tab, so its cookies, cache and storage were written to the
  profile on disk and its address was saved in the session file.
* The shell checks every `open` and `reload` command a renderer sends to
  the video helper, as it already did for the audio helper: only a
  stream file under `~/.cache/nordstjernen/msvideo/` is passed on. A
  renderer could make the unsandboxed video helper open any local file
  or URL. Both checks now split a command on spaces and tabs the way the
  helpers do, so a tab after `open` no longer skips the audio check, and
  both helpers open media with FFmpeg's `file` protocol only.
* Each published release gets a Debian/Ubuntu `.deb`, a portable Linux
  x86_64 zip and a `SHA256SUMS` file attached by a new `release`
  workflow. The packages are built and checked with the same script as
  the nightly builds, which installs the `.deb` and runs it headless
  before anything is uploaded.
* A connection that fails to one port of a host no longer makes
  requests to the host's other ports fail for two minutes: recent
  connection failures are remembered per origin (scheme, host and port)
  instead of per host.
* `performance.getEntriesByType('resource')` lists the document's
  scripts, stylesheets, frames and images along with `fetch()` and
  `XMLHttpRequest` requests, as `PerformanceResourceTiming` entries with
  the timings the network layer measured (DNS lookup, connection, TLS,
  request and response), the connection's protocol in `nextHopProtocol`,
  the real `responseStatus`, transfer and body sizes, and
  `renderBlockingStatus`. A cross-origin resource without a
  `Timing-Allow-Origin` header naming the page keeps only its start and
  end time, and a no-CORS cross-origin response shows status 0, as in
  other browsers. Only `fetch()`, `XMLHttpRequest` and script-made images
  were listed before, with every phase at the start or the end of the
  request, `nextHopProtocol` always `"h2"` and `responseStatus` always
  200. A second image with the same URL comes from memory and gets no
  entry, nor does a frame whose response is a download, and
  `PerformanceObserver.supportedEntryTypes` is in alphabetical order.
* Each document has its own performance timeline. A frame's
  `performance` listed the page's entries and marks and its
  `performance.mark()` added to the page's; now marks, measures and
  resource entries stay with the document that made them, a
  PerformanceObserver sees the entries of the document its callback
  comes from, and `clearResourceTimings()` clears resource entries
  instead of doing nothing.
* Scripts inserted with `append()`, `prepend()`, `before()`, `after()`,
  `replaceWith()` or `replaceChildren()` run, as they do with
  `appendChild()`. They never ran, so `document.head.append(script)`
  loaded nothing and Svelte 5 components lost the scripts in their
  `<svelte:head>` (Stack Overflow's Google sign-in among them).
  Stylesheet links and `srcdoc` frames these methods insert load now
  too. Custom
  elements these methods insert or move get `connectedCallback` and
  `disconnectedCallback` before the method returns, moving a node out of
  another parent produces its MutationObserver removal record and
  updates NodeIterators, and for every insertion method scripts run
  before custom element callbacks, as in other browsers.
* Images in the page's own markup fire `load` and `error` events, and
  the window's `load` event waits for them, as in other browsers. A
  broken or missing image could finish without an `error` event and
  still read as incomplete when the page's `load` event fired.
* Dedicated workers follow the HTML standard more closely. Messages to
  and from workers use structured cloning, so `Error` objects (with
  `cause`), `DOMException`, `Blob` and `File`, non-index array
  properties, shared references and cycles arrive intact, functions and
  symbols throw `DataCloneError`, and a transferred `MessagePort` can be
  sent back to where its other end lives. `new Worker()` accepts `data:`
  URLs (which run with an opaque origin), resolves URLs against the
  document's base URL, throws `SyntaxError` only for a URL that does not
  parse and otherwise reports failures through an `error` event.
  Uncaught worker errors reach `self.onerror`, then the `Worker`'s
  `onerror`, then the page's `window.onerror`, unless one of them
  cancels it; a worker keeps running after one, and `terminate()` drops
  messages the worker had already sent.
* `document.write` runs the scripts it writes; they used to be skipped.
  A write whose markup closes everything it opens is inserted right away,
  so the written elements exist when `write()` returns and a written
  inline script runs inside the call, as in other browsers. A written
  external script runs before the rest of the page is parsed.
* Scripts run against the part of the page parsed so far, as in other
  browsers: a script in `<head>` sees `document.body` as `null`, a
  script is the last `<script>` in the document while it runs, elements
  further down cannot be found yet, and the rest of the page is inserted
  after the script, with the MutationObserver records parser insertions
  produce. The whole page used to be visible to every script.
* Shadow roots behave as document fragments: nodes directly inside one
  have a `null` `parentElement`, the root's `nodeName` is
  `#document-fragment`, it is not `instanceof Element`, and `closest()`,
  `contains()` and `compareDocumentPosition()` stop at the shadow
  boundary. Document-wide queries such as `getElementById` and
  `querySelector` no longer return elements inside shadow trees, which
  they did once a script had looked those elements up inside the shadow
  root.
* A frame starts with its own `onload`, `onclick` and other window event
  handlers set to `null`. It used to start with copies of its parent's,
  so the page's `window.onload` could run again for the frame.
* Nested frames know their place: a frame inside a frame has the middle
  frame as its `parent`, so `parent.frames`, `parent.document` and
  `parent.name` refer to it, while `top` stays the page. `parent.location`
  and `top.location` read in a frame give that window's URL instead of
  the frame's own, setting `parent.location.hash` fires `hashchange` at
  the page, and a frame's `document.referrer` is the URL of the document
  holding it, cut down by the referrer policy as in other browsers.
* Focus follows the HTML focus update steps. `blur` and `focusout` run
  while no element has focus, `relatedTarget` names the other element,
  and the windows get `blur` and `focus` when focus moves between a page
  and its frames, by script or by a click. `document.activeElement` is
  the iframe, not the element inside it, when a frame has focus, so a
  page can no longer see which element of a cross-origin frame is
  focused. `document.hasFocus()` is true only for the focused document
  and the documents containing it, and `focus()` does nothing on an
  element that cannot take focus or is not in the document. Clicking
  plain content inside a frame focuses the frame's document, as in other
  browsers, instead of the iframe element around it.
* Events the browser fires carry the flags other browsers give them:
  `focus`, `blur`, `readystatechange`, element `scroll` and media events
  no longer bubble, and `DOMContentLoaded`, `input`, `change`, `scroll`
  and `focusin`/`focusout` can no longer be cancelled. `invalid` can be.
* Window events look as they do in other browsers: `load` reaches only
  window listeners, with the document as its target; `hashchange` and
  `popstate` are fired at the window and do not bubble; `pageshow` and
  `pagehide` have the document as their target; `window.onload`,
  `window.onmessage` and the other window handlers see `eventPhase` and
  `currentTarget`; and `<body onload>`-style handlers run with the window
  as `this`. Scrolling the page fires one `scroll` event instead of two,
  and `resize` is fired at the window only.
* Transferring a `MessagePort` moves it: the receiver gets a new port
  object, references to the port inside the message point to that new
  port, and the sender's port stops sending and receiving. Messages
  already queued for the port go with it. Transferring a port twice,
  transferring the port that sends the message, or listing a port twice
  throws `DataCloneError`, and a message that fails to clone leaves its
  ports in place. `window.postMessage` transfers its ports even when the
  target origin does not match, as in other browsers.
* A frame's document no longer shows through its iframe element:
  `textContent`, `innerHTML`, `outerHTML`, `getHTML()`, `XMLSerializer`,
  `TreeWalker`, `hasChildNodes()` and `cloneNode(true)` treat the iframe as
  having only its fallback content, as `childNodes` already did. They used
  to include the frame's text and markup, even for a cross-origin frame.
* Deep recursion in a worker throws `RangeError` instead of crashing the
  browser: the worker's JavaScript stack limit now fits its thread's stack.
* Message ports work across frames: a frame that receives a transferred
  `MessagePort` gets its message events, `data` and `ports` in its own
  realm, so arrays, dates and objects pass `instanceof` checks there,
  `addEventListener` on a port accepts `{handleEvent}` objects, and the
  handlers run against the frame's document. Exceptions thrown by port
  handlers and by window `on<event>` handlers now reach `onerror` and
  `error` listeners instead of being dropped.
* Events inside a frame reach that frame's window: window listeners and
  `window.onclick`-style handlers see clicks, pointer and key events,
  `DOMContentLoaded` and bubbling custom events, and a frame's `load`
  event fires once with the document as its target.
* When the browser dispatches an event, such as a user click or a port
  message, microtasks queued by one listener run before the next
  listener, as in other browsers.
* The window keeps being ticked while a `requestIdleCallback`, a posted
  message or a script-started image load is waiting, so they no longer
  stall until an unrelated timer fires.
* `window.postMessage` throws `SyntaxError` for an unparseable target
  origin and defaults to `"/"` when none is given; messages to and from a
  frame sandboxed without `allow-same-origin` use the opaque origin
  `"null"`; message events target the receiving window and carry a
  frozen `ports` array from the receiving realm.
* Aliased built-ins keep their spec names (`String.prototype.trimLeft`
  is the function named `trimStart`), and `Function.prototype.name` is
  empty again.
* `postMessage` and `MessagePort` messages are delivered as tasks, after
  the sender's microtasks, as in other browsers. They ran as microtasks,
  so a message arrived before promise callbacks queued ahead of it, and a
  handler that posted back to itself kept `setTimeout` callbacks from
  ever running.
* A frame's `WindowProxy` stays the same object across its first
  navigation. A `contentWindow` read while an iframe still showed its
  initial `about:blank` now reaches the document that loads into it:
  same-origin frames reuse the initial window, as the HTML spec
  requires, and messages posted through the early reference to a
  cross-origin frame are delivered. The link between a frame's outer
  window and its realm global moved out of JavaScript-visible properties,
  which had let a cross-origin frame reach its embedder's window.
* Structured data sent between windows, frames and `MessageChannel`
  ports arrives as objects of the receiving realm: `e.data instanceof
  Uint8Array`, `Map`, `Date`, `Array` and `Object` hold in the receiver,
  as in other browsers. Messages crossing a frame boundary carried the
  sender's objects, and on ports `Map`, `Set`, `Date`, `RegExp`,
  `DataView` and boxed primitives degraded to plain objects.
* Frames have their own document lifecycle and geometry. Each frame's
  `document.readyState` runs `loading` -> `interactive` -> `complete`
  with `readystatechange` at each step, instead of reporting the top
  page's state (usually already `complete`). `innerWidth`/`innerHeight`
  in a frame are the frame's size, not the top window's, element rects
  are measured from the frame's content box rather than its border box,
  and `document.elementFromPoint` in a frame hit-tests that frame.
* A message from a cross-origin frame has the frame's WindowProxy as
  `event.source`, the same object as `iframe.contentWindow` and
  `frames[i]`, so pages can tell which frame spoke. It was a different
  wrapper, so `e.source === iframe.contentWindow` was false.
* A `Response` or `Request` built from a string keeps that string as its
  body. `new Response('{"a":1}').json()` rejected with a `SyntaxError`
  because `text()` saw an empty body, and `fetch(new Request(url,
  {method: 'POST', body: 'x'}))` sent nothing.
* Rounded solid borders whose sides differ in color, or have some sides
  transparent, follow the corner radius. Each side was stroked as a
  straight line, so a `border-radius` ring with two transparent sides,
  such as the reCAPTCHA checkbox spinner, drew as a right angle.
* Events carry the interface their type implies: messages from windows,
  `MessageChannel` ports and workers are `MessageEvent`s, and `error`,
  `hashchange`, `popstate`, `storage`, `pageshow`/`pagehide` and promise
  rejection events get their own interfaces. Port messages were plain
  objects. `isTrusted` lives on each event, not on `Event.prototype`, so
  objects deriving from `Event.prototype` can define their own, as in
  other browsers. Assigning to a getter-only property now names it in
  the `TypeError`.
* Page scripts enumerating the global object (`Object.keys(window)`,
  `Object.getOwnPropertyNames`, `for...in`) no longer see the engine's
  own `__nd`/`__ns`/`__js` helper properties, which no other browser
  exposes. The engine's own code still reaches them by name.
* Objects the engine hands to pages inherit from their WebIDL interface
  and report its name: `new FileReader() instanceof FileReader`,
  `Object.prototype.toString.call(localStorage)` is `[object Storage]`,
  and canvas contexts, `TextMetrics`, `ImageData`, `MediaQueryList`,
  `FontFaceSet`, `location`, `screen` and the `navigator` sub-objects
  follow suit. `Location`, `Screen`, `BarProp`, `CustomElementRegistry`
  and the other interfaces these belong to now exist as globals.
* Functions the engine implements in JavaScript print as native code,
  `function animate() { [native code] }`, like every other built-in.
  434 of them printed their JavaScript source and many carried internal
  names (`elementAnimate`, `value`), which no browser does. Page scripts,
  inline handlers and `new Function` keep their source.
* Inside a frame, `window`, `location` and `history` report themselves
  as `[object Window]`, `[object Location]` and `[object History]`, and
  `history instanceof History` holds, as on the top-level page. The
  frame's global still carried QuickJS's `global` tag.
* A worker's `navigator` matches the page's: `appVersion` follows the
  user agent instead of a fixed Linux string, and `platform`, `product`
  and `deviceMemory` are present. `performance.getEntries*` return empty
  lists in workers instead of throwing. A frame's document has `domain`,
  `timeline`, `pictureInPictureEnabled`, `adoptedStyleSheets`,
  `xmlEncoding` and `xmlStandalone` like the top-level document.
* Dedicated workers have `Intl` and `crossOriginIsolated`. A worker that
  read its time zone or formatted a number threw `ReferenceError`, and
  since worker errors do not reach the page, the page waited forever.
* Numbers the engine formats keep a `.` decimal point under locales that
  use a comma, such as Turkish or Norwegian. `--single-process` runs the
  engine inside the GTK shell, whose startup applies the OS locale, so
  `Accept-Language` went out as `tr-TR,tr;q=0,9`, an invalid header.
* `window.postMessage(message, [port])` with an array as the second
  argument follows the `(message, options)` overload, as WebIDL overload
  resolution requires: the array is read as an options dictionary, so
  nothing is transferred. It was taken as a transfer list, so a library
  that posts `postMessage(token, [channel.port2])` from code shared with
  workers handed its own port to every `message` listener on the page,
  and a listener waiting for a port from another frame accepted that one
  instead. Any second argument that is neither an object nor
  `undefined`/`null` is the target origin, and `postMessage()` with no
  arguments throws `TypeError`.
* The Android app builds with the current toolchain: Android Gradle
  plugin 9.4 with its built-in Kotlin, Gradle 9.8, compileSdk 37
  (Android 17) with build-tools 37, CMake 4.1.2 and the current AndroidX
  libraries. targetSdk stays 36. Native libraries are still stored
  compressed and extracted at install, now requested from the build script
  (packaging.jniLibs.useLegacyPackaging) because AGP 9 rejects
  android:extractNativeLibs in the manifest.
* Nordstjernen can run on Fabrice Bellard's original QuickJS as well as the
  in-tree quickjs-ng fork, chosen in meson with `-Dquickjs=quickjs` or
  `-Dquickjs=quickjs-ng` (the default). The original engine is fetched at
  configure time through `subprojects/quickjs.wrap`, pinned to its
  2026-06-04 release, and is never vendored. The same binding runs on both:
  `src/ns_quickjs.h` maps the quickjs-ng API onto the original's, and the
  original keeps its own bytecode cache. On the original engine, named
  access into another frame's window, a view taken on an imported
  WebAssembly memory before instantiation, and the fork's language
  compatibility changes (`RegExp.$1`, `Function.prototype.caller`) behave
  as stock QuickJS does; `docs/quickjs.md` lists the differences.
* A module script that imports other modules runs again after the page
  has created an event, posted a message, opened IndexedDB or fetched
  something. Native code that built those objects assigned `isTrusted`
  over the read-only accessor on `Event.prototype`, and a fetched
  response's `body` over the read-only `Response.prototype.body`, ignoring
  the failure; quickjs-ng kept the error pending and reported it from the
  next module evaluation as `TypeError: no setter for property`, so the
  whole module was lost. Each page load also left two such errors behind
  from start-up: `Event.prototype` was being made its own prototype, and
  `new URL()` built its `searchParams` before `URLSearchParams` existed.
  Trusted events keep `isTrusted` true through the accessor.
* `new Response(body)` and `new Request(url, { body })` keep a string body
  again. The constructors assigned it over the read-only
  `Response.prototype.body` accessor, so the text never reached the body,
  and `text()`, `json()` and `clone()` returned an empty body,
  `new Response(null).body` was a stream instead of `null`, and reading
  the stream failed. The raw body is now defined on the object and
  replaced by the buffered body once it is read, as the GPL edition does.
* On the original QuickJS, `Array.prototype.sort` calls the comparator for
  identical values, as the fork and every other engine do, through one
  patch applied to the fetched source
  (`subprojects/packagefiles/quickjs-sort-calls-comparator.patch`).
  Without it jQuery 4's `uniqueSort` kept duplicates, so `$(a).add(a)` and
  `.closest()` returned the same element twice. The adapter also pads
  short argument lists to `JS_NewTypedArray`, whose original constructor
  reads three arguments whatever `argc` says, gives `JS_EvalThis2` the
  quickjs-ng default file name when none is passed, and refuses a
  resizable external `ArrayBuffer` instead of quietly making it fixed.
* `window[i]` and `frames[i]` return the WindowProxy of the i-th child
  frame, and `window[name]` returns the frame whose `name` attribute
  matches, as the HTML named-access rules specify. Indexed access always
  gave `undefined`, and from inside a frame `parent.frames` counted the
  frames of the calling document instead of the parent's, so sibling
  frames could not reach each other to `postMessage`. Named and indexed
  access resolves in the document of the window being read, so a frame's
  `window.foo` no longer finds an element in its parent. A cross-origin
  parent now exposes its child frames by index and name too, each behind
  the same cross-origin WindowProxy, while every other property still
  throws `SecurityError`.
* Inline event-handler content attributes inside a frame, such as
  `<body onload="...">`, are compiled and run in that frame's realm. They
  ran in the top-level page's realm, so they could not see functions
  declared by the frame's own scripts and failed with `ReferenceError`,
  and a frame without scripts ran its handlers with the parent's document.
  An `<iframe onload="...">` attribute runs in the page that holds the
  iframe, not inside the frame.
* A frame's `window.name` starts as its `<iframe name>`, and a same-origin
  frame's `window.frameElement` is its `<iframe>`. Both were empty, so a
  frame could not find itself in `parent.frames`.
* `postMessage` into a same-origin frame reports the caller as
  `event.source`. It reported the receiving frame itself, so replying to
  `event.source` sent the answer back into the frame instead of to its
  parent. A message from another origin hands the frame a cross-origin
  WindowProxy as `event.source` rather than the sender's own window.
* The in-tree HTML parser, lexbor, is refreshed to upstream master
  (327a8b6). It fixes memory corruption when a pooled allocation shrinks,
  an out-of-bounds write in `CharacterData.replaceData()`, and
  uninitialized memory in IDNA host processing. URLs parse more exactly:
  `.` and `..` path segments no longer swallow the `?` or `#` after them
  or miscount the path (`/a/b/../../../c/é/../../x` is `/x`), `//./c`
  keeps its empty segment, a username may contain `@`, and spaces at the
  end of an opaque path such as `sc:a ?q` are encoded. A `<form>` inside
  a template's `innerHTML` is parsed as in the rest of the template.
* `align-content` works on block containers, as CSS Box Alignment
  specifies: `center`, `end` and their `safe`/`unsafe` forms move the
  content of a block with a fixed height. It only worked in flex and grid
  containers, so a vertically centered block kept its text at the top.
* A block-level `<textarea>` with a CSS height is sized by that height.
  Its text run was sized as the whole control, padding included, so a
  single-row field overflowed its own content box: it drew a scrollbar
  and reported a `scrollHeight` 20px too tall, which auto-growing chat
  inputs copied into their height.
* A focused, empty `<textarea>` keeps showing its placeholder, with the
  caret in front of it, as `<input>` already did. Placeholder text is no
  longer spell-checked.
* Web fonts whose files carry a family name other than the `@font-face`
  family are registered under the CSS family with each file's own
  weight. A family split over Regular, Medium and Bold files used to be
  aliased to whichever file loaded first, so normal text could render
  bold.
* Nordstjernen builds against GLib 2.90. Its `g_new()`/`g_renew()`
  macros now declare a local named `_n`, which the CSS selector matcher
  shadowed, so the Windows (MSYS2) build failed under `--werror`.
* The macOS CI smoke test of the bundled app accepts the redesigned
  example.com page, which no longer carries the "Example Domain"
  heading the test looked for, so every macOS run failed after a
  successful build.
* YouTube and other MSE players play video again.
  `SourceBuffer.appendBuffer()` still called a helper that the Blob
  rewrite removed, so every append threw `ReferenceError` and no media
  byte reached the decoder: YouTube showed "An error occurred". It now
  copies the `ArrayBuffer` or view it is given and throws `TypeError`
  for anything else, before the state checks, as WebIDL requires.
* `animation` and `transition` set to a CSS-wide keyword (`inherit`,
  `initial`, `unset`, `revert`, `revert-layer`) give that keyword to
  every longhand. The shorthand expansion read the keyword as an
  animation list, which crashed the renderer on pages such as YouTube
  and made `animation: inherit` compute to `none`.

1.0.26:
======
* Grids with more than 24 rows lay out fully. Auto-placement stopped at
  row 24 and piled every later item onto it.
* Grids that use `grid-template-areas` are laid out by the full grid
  algorithm. They went through a reduced code path that gave `fr` rows no
  share of a fixed container height (a header / `1fr` / footer page left
  the footer under the header), sized the columns the areas added as `1fr`
  instead of by `grid-auto-columns`, and ignored `align-content`,
  `align-items` and `grid-auto-rows`. Area names also work as line names,
  so `grid-row: main` places an item on a named area.
* `fr` rows grow to fill a container's `min-height`, so the `min-height:
  100vh; grid-template-rows: auto 1fr auto` page layout keeps its footer
  at the bottom.
* A grid row with a fixed size keeps it: content taller than a `20px` row
  overflows instead of pushing later rows down. Items given both a row and
  a column claim their cell before auto-placed items flow in, instead of
  landing on top of one.
* Grid track sizing follows the specification more closely: an item
  spanning several `auto` or content-sized columns widens them instead of
  overflowing; `fit-content()` tracks shrink to their content;
  `minmax(<length>, 1fr)` columns no longer push the grid wider than its
  container; `repeat(auto-fill / auto-fit)` counts repetitions in the
  space the other tracks leave and now works for rows too, empty
  `auto-fit` rows collapse, and line names inside the repetition repeat
  with it; columns created past the explicit grid are sized from
  `grid-auto-columns`.
* `grid`, `grid-template`, `grid-row`, `grid-column`, `grid-area` and
  their longhands follow their grammar: invalid values such as
  `grid-template: 10px` or `grid-row-start: 0` are dropped instead of
  partly applied, omitted parts are reset, rows written without a size in
  the template form are `auto`, line and area names keep their case, and
  `element.style` and `getComputedStyle()` read the shorthands back in
  shortest form. Computed track lists keep `repeat()`, `minmax()`,
  `fit-content()` and subgrid line names.
* `self-start` and `self-end` on a grid item use the item's own writing
  mode.
* `sibling-index()` and `sibling-count()` in a container size query
  resolve against the container element.
* Popovers follow the HTML standard. `showPopover()` left the element
  hidden until something else restyled the page, opening one auto popover
  never closed another, nothing threw for elements without a `popover`
  attribute or for disconnected ones, `toggle` fired synchronously as a
  plain event, and a `popovertarget` submit button inside a form toggled
  the popover instead of submitting. Popovers now use the standard auto
  and hint stacks, fire a cancelable `beforetoggle` and a queued
  `ToggleEvent` carrying `source`, close when the user clicks outside or
  presses Escape, hide when removed or retyped, and get the standard
  centred fixed box. The `popover`, `popoverTargetAction`,
  `popoverTargetElement`, `command` and `commandForElement` properties
  reflect their attributes, and `commandfor`/`command` buttons work: show,
  hide and toggle-popover, show-modal, close, request-close, and custom
  `--` commands through `CommandEvent`.
* `<dialog>` follows the HTML show and close steps: `show()`,
  `showModal()` and closing fire `beforetoggle` and a queued `toggle`, and
  the `close` event is queued; `close()` on a closed dialog no longer
  changes `returnValue`; `show()` on an open modal dialog throws, as does
  `showModal()` on a dialog outside the active document; modal dialogs
  stack in the order they were opened, so the last one opened is drawn on
  top and stays interactive, and `:modal` matches every open modal dialog;
  focus returns to the element focused before the dialog opened;
  `requestClose()`, Escape and `closedby="any"` light dismiss go through a
  close watcher that honours `closedby`, and cancel can no longer re-enter
  itself.
* Light dismiss of popovers and dialogs runs before the pointer event
  reaches the page, so `beforetoggle` arrives before the page's
  `pointerup` listeners, as the standard orders it.
* A click on a popover opened inside a modal dialog lands on the popover,
  not the dialog behind it.
* Hiding a popover no longer moves focus back onto an element the page has
  since moved into that popover.
* `:focus-visible` is no longer the same as `:focus`: a button or link
  focused by a mouse click no longer shows the focus styling pages reserve
  for keyboard users, and `:focus:not(:focus-visible)` now matches.
  Keyboard focus, text fields and script focus remain visible, and a
  focused control that turns into a text field starts matching.
* Pages' `autofocus` attribute works: the first visible, focusable element
  with `autofocus` gets focus before the page finishes loading, unless the
  page already focused something or the URL fragment points at an element.
* The `--wpt` runner sends test_driver clicks, action sequences and keys
  as trusted input, so light dismiss, Escape, Tab, keyboard activation and
  click-to-focus are exercised the way real input exercises them, and
  WebDriver key codes are no longer typed into fields.
* The audio helper only opens what a page may play. It opened any
  `file://` path the renderer named, and treated any other string that was
  not an http, https or data URL as a local path, so the page's JavaScript
  check was the only thing keeping a web page from playing (and so
  probing) local files. The shell now forwards `open` and `reload`
  commands only for http, https and data URLs, the browser's own MSE audio
  stream files, and local files when the tab itself shows a `file:` page;
  the helper no longer reads bare paths at all.
* The Windows process mitigations are the ones intended. The policies were
  passed as bare numbers and two were wrong: 0 is DEP, not ASLR, and 7 is
  Control Flow Guard, not the dynamic-code policy -- DEP is always on for a
  64-bit process and CFG cannot be turned on after start, so both calls did
  nothing. ASLR now forces image relocation; dynamic-code prohibition stays
  off, since the renderer's GPU driver and a V8 build need executable
  memory; SECURITY.md is corrected to match.
* A frame whose URL redirects to another site takes the origin of the page
  it actually loads. The frame kept the URL it was requested with, so a
  same-origin address that redirected elsewhere gave the other site's
  document the embedding page's origin: it could read the parent's DOM and
  cookies, and the parent could read it. The frame now uses the response's
  final URL, and a redirect into a source the page's CSP `frame-src` or
  `object-src` does not allow is blocked.
* A page can no longer read the pixels of another site's images. Drawing
  a cross-origin image into a canvas -- directly, through a pattern,
  another canvas, an ImageBitmap, an OffscreenCanvas or a video poster --
  left its pixels readable through `getImageData`, `toDataURL`, `toBlob`
  and `convertToBlob`, and WebGL's `texImage2D` plus `readPixels` read them
  back too. The canvas is now marked tainted and those calls throw
  `SecurityError`; WebGL and WebGPU refuse cross-origin sources outright;
  a redirect to another origin counts as cross-origin. Same-origin, `data:`
  and `blob:` images are unaffected, and so are images loaded with
  `crossorigin` from servers that allow it through CORS, so WebGL texture
  and map-tile pages keep working.
* `OfflineAudioContext` and `createBuffer` reject impossible sizes with
  `NotSupportedError`. A page could ask for an 8 GB render buffer, or raise
  `length` after construction, and crash the browser, and bad channel
  counts, lengths or sample rates were quietly replaced with 1. Very deep
  audio graphs no longer abort when scratch memory runs out.
* AES-CTR honours the counter `length`: the whole 128-bit block was used as
  the counter, so once the counter wrapped the ciphertext no longer
  decrypted in other browsers. Invalid lengths and messages that would
  reuse a counter block are rejected.
* `crypto.getRandomValues` throws the errors the specification names:
  `TypeMismatchError` for a DataView and `QuotaExceededError` for more than
  65536 bytes.
* Changing an element's `class` or `id` to a name no style sheet mentions
  no longer restyles everything inside it: toggling an unused theme class
  on `<body>` re-ran the whole cascade, about 200 ms on a page of 12,000
  elements and 3,000 rules. Names used anywhere in a selector, including
  inside `:is()`, `:not()`, `:has()`, `:nth-child(... of S)` and `@scope`,
  and any `[class]`/`[id]` attribute selector, still restyle as before.
* Class changes and element insertion no longer walk sibling lists to keep
  the document's class and tag indexes in order; large index entries are
  re-sorted only when read, so 3,000 class changes on a 12,000-element page
  take 5 ms instead of 480.
* Removing or inserting children no longer counts the node's position
  among its siblings when no live `Range` exists: emptying a 20,000-item
  list from the end took 19 seconds and now takes 58 ms.
* A CSS `font-family` list is resolved to a font once rather than on every
  text run of every layout and paint, until the system font set changes or
  a web font loads.
* An image with a CSS `filter` is filtered once and the result kept with
  the decoded image, not on every paint.
* Ordered lists are numbered once per layout or paint pass instead of once
  per item.
* Page zoom scales each element's font size once; computed values shared
  between sibling styles were multiplied once per sharer.
* An absolutely positioned grid child placed with `grid-column`/`grid-row`
  stays on its tracks when the grid moves after layout, under a
  relatively positioned ancestor or in a reversed wrapping flex container.
* A cross-origin frame no longer reaches the embedding page through what
  the two share. A frame's global object received a copy of every global
  on the parent's window, including the page's own variables, so
  `window.someState` or any stored reference to `document` handed the
  frame the parent's DOM and cookies; `new Text()`, `new Comment()` and
  `new Range()` produced nodes belonging to the parent's document;
  `customElements.define()` in the frame upgraded the parent's elements
  with the frame's class; and the `cookieStore` shim returned the parent's
  cookies. A cross-origin frame now gets only the browser's own globals,
  captured before the page's scripts run, and none of the parent-bound ones
  (`cookieStore`, `caches`, `getSelection`, `opener`, `frameElement`,
  `name`, `origin`, `navigation`); nodes and ranges a frame creates belong
  to the frame's document; each document has its own custom element
  registry, as HTML specifies; and `self.origin` reports the frame's
  origin.
* URL setters follow the URL Standard where the parser library does not:
  `url.host = "example.com:99999"` changes the host and keeps the port,
  clearing the host of a non-special URL with credentials is refused, and
  `new URL("??a=b").searchParams` keeps the second `?`. Links whose `href`
  does not parse report `":"` as their protocol, and an `href` containing
  a NUL is no longer cut short there.
* `innerText` follows the rendered-text rules: a shadow host's text no
  longer includes its shadow tree, an inline `<svg>`'s `<text>` counts, a
  `visibility: hidden` paragraph or `<br>` adds no line breaks, and setting
  `innerText` or `outerText` to a string with a NUL keeps what follows it.
* Reflected attributes that ignored writes or read the wrong type are
  fixed: `meta.content`, `textarea.rows` and `frameset.rows` can be set
  from script; ARIA properties read `null` when absent and the missing
  ones exist; `font.size` is a string; `progress.max` ignores non-positive
  values; progress and meter use the HTML number rules; and `label` and
  `defaultValue` writes reach mutation observers.
* `innerHTML`, `outerHTML`, `insertAdjacentHTML` and
  `Range.createContextualFragment` parse markup in the context element's
  namespace, so gradients, filters and shapes that D3, icon libraries and
  chart code insert into an `<svg>` that way are drawn.
* A `<script>` inserted empty runs once it gets text or a `src`, and
  `src=""` fires `error` instead of running the inline text.
* Page images start downloading when the document is laid out rather than
  at first paint; each `<img>` fires `load` when its image arrives,
  `complete` and `naturalWidth` report it, and the window `load` event
  waits for the page's non-lazy images.
* Setting `document.title` on a page without a `<title>` creates one that
  later reads find and mutation observers see.
* `reportError(value)` reports the value like an uncaught exception, as a
  cancelable `ErrorEvent` at the window, and uncaught-exception events are
  `ErrorEvent`s.
* Elements whose interface is plain `HTMLElement` (`article`, `section`,
  `b`, `nav`, `summary`, ...) are no longer `HTMLUnknownElement`, and
  `listing`/`xmp` are `HTMLPreElement`s.
* Enter activates a focused link, button or `<summary>`, and Space on
  release a focused button, checkbox, radio button or summary, with a
  trusted `click` as a mouse press would; and Tab continues from where you
  last clicked instead of from the top of the page.
* Form validation: every radio button in a required group reports
  `valueMissing` while none is checked; submit buttons are validation
  candidates, so a custom validity message on one blocks submission; and a
  `readonly` input of any type is left out of validation.
* `relList.supports()` answers per element and `<form>` has a `relList`;
  `String(link)` gives an `<a>`'s or `<area>`'s URL; and natively
  implemented interfaces have their `Symbol.toStringTag`.
* Client-side image maps work: clicking, hovering or `elementFromPoint()`
  over an `<img usemap>` lands on the `<area>` under the pointer, and
  clicking an area follows its link.
* `Blob` and `File` follow the File API: any iterable of parts, `endings:
  "native"`, printable-only `type`, prototype getters, clamping `slice()`
  and `blob.bytes()`.
* `structuredClone()` and `postMessage()` follow HTML's serialization
  rules more closely: transferring an `ArrayBuffer` detaches it, a detached
  or duplicate transfer throws `DataCloneError`, resizable buffers and
  shared views survive, and a page that replaces `window.structuredClone`
  no longer changes what `postMessage()` sends.
* Styling a large page is much faster. Every rule used to be tried on each
  element whose tag, class or id matched its last compound, and each try
  walked all the way up the ancestors. The cascade now keeps a small
  filter of the tags, ids and classes above the element being styled and
  drops a rule at once when an ancestor it needs cannot be there: a page of
  16,000 elements and 3,000 rules loads in under a second instead of three.
* A selector whose left end cannot match no longer makes matching
  exponential in the depth of the page. `.nomatch div div div span` on a
  deep tree took seconds, in a style sheet or in `querySelectorAll`;
  matching now stops as soon as the rest of the selector cannot match.
* `:nth-child`, `:nth-last-child`, `:nth-of-type` and `:nth-last-of-type`
  number each list once per style pass instead of counting siblings for
  every element, so a long list is no longer quadratic to style: 40,000
  rows with `li:nth-child(even)` load in about two seconds instead of 25.
* A child no longer inherits from the wrong parent through style sharing.
  Sharing identified parents by a counter restarted on every style pass,
  so after a partial restyle, or under parents styled with `attr()`, two
  different parents could look the same.
* Relayouts no longer re-resolve, re-scan and re-key every style sheet,
  and `@import`ed sheets are parsed once rather than on every relayout,
  which also lets a page with an import restyle incrementally. Identical
  inline CSS on two pages in different directories resolves its `url()`
  values against each page's own address.
* `@container` conditions are parsed once per rule instead of for every
  element they are tested against.
* Changing the desktop colour scheme or the reduced-motion preference takes
  effect on reload; cached style sheets parsed for the old preference were
  being reused.
* `<img style="width:100%">` no longer crashes the renderer. Measuring a
  percentage-width image asked for its own natural width, which asked for
  the percentage basis again, until the stack ran out; replaced elements
  resolve the percentage against their container again.
* `srcset` is parsed as the HTML standard describes: a `data:` URL or any
  URL containing a comma is no longer cut short, malformed descriptors are
  rejected, and a `1x` candidate beats `src`. An image picked from a `2x`
  or `w` candidate is laid out at its density-corrected size and reports it
  through `naturalWidth`/`naturalHeight`, and a `<picture>` only considers
  the `<source>` elements before its `<img>`.
* Legacy presentational attributes follow the rendering section. Dimension
  attributes use HTML's parsing rules (`width="20.25e2"` is 20.25px, not
  2025px); `<font size="+1">` is relative to size 3 and nested sizes no
  longer compound; hspace/vspace take percentages and apply to embed,
  object and image buttons. Newly mapped: body topmargin/leftmargin/
  marginwidth/marginheight (including the containing frame's),
  `background`, `nobr`, `br clear`, `caption align` and legend `align`;
  `<marquee>` shows its text. Table `border`, `frame`, `rules` and
  `bordercolor` give the spec's outset/inset borders, and a width/height
  pair on img, video and image buttons sets `aspect-ratio`, taken from the
  selected `<source>` inside a `<picture>`.
* The `dir` attribute sets CSS `direction`, so right-to-left pages lay out
  flex rows and list markers from the right, not only their text.
* The default stylesheet follows the HTML rendering section instead of
  house style: lists get disc/circle/square by nesting depth with no gaps
  around nested lists; text and links use the system colours;
  `code`/`kbd`/`samp` are plain monospace; `pre` has 1em margins;
  `<dialog open>` is centred instead of pushing the page down; form
  controls no longer inherit uppercase, indent or letter-spacing. Also the
  hidden-element and form-in-table rules, `appearance: auto` on controls,
  the default iframe border with `frameborder`, and fieldset's groove
  border.
* A `<legend>` sits in its fieldset's top border, with the border broken
  around it, positioned by its margins and `justify-self`.
* The `<details>` disclosure triangle is a list marker, so `list-style:
  none` or `display: block` on the summary removes it, and content inside
  `<details>` is no longer indented 16px per level.
* Frames and objects are no longer forced with user-agent `!important`, so
  a page can hide a loaded helper iframe, and an `<object>` without a
  document shows its fallback content.
* No-doctype pages get HTML's quirks-mode rules: tables stop inheriting
  body fonts and alignment, forms keep a bottom margin, and a nowrap cell
  with a pixel width wraps.
* An image loaded into an iframe shows as an image rather than its bytes
  parsed as HTML.
* Each `<style>` element is its own style sheet. Adjacent inline sheets
  were joined before parsing, so one that ended inside an unclosed block
  or comment swallowed every sheet after it. `<style type="text/foo">`, a
  `<link>` whose `type` is not CSS and `<link disabled>` no longer apply,
  and `styleEl.disabled` / `sheet.disabled` switch a sheet off.
* A declaration whose `var()` cannot be substituted computes as `unset`
  instead of letting an earlier declaration win, as CSS Variables
  requires; a substituted value carrying `!important` counts as failed.
* Declarations written after a nested rule keep their place in the
  cascade instead of losing to the nested rule.
* A stray `;` or `}` between rules invalidates the rule after it, and a
  selector list with a trailing comma, an empty item or junk is dropped
  whole, as CSS Syntax says and other browsers do. A type selector after
  an id, class or attribute is rejected too.
* Comments inside a media query are whitespace, so
  `@media (min-width: 100px) /* desktop */` applies again.
* An inline `!important` beats a layered `!important` rule.
* `initial` gives inherited properties their real initial value instead
  of behaving like `inherit`; `color: currentColor` takes the parent's
  colour; and `bolder`/`lighter` resolve against the parent's weight with
  the CSS Fonts 4 table.
* `text-shadow`, `orphans`, `widows` and `dominant-baseline` are
  inherited, so a shadow on a container reaches its paragraphs.
* A percentage `line-height` is inherited as a length rather than
  re-applied to each child's font, and `rem` in the root's `font-size`
  refers to the initial size rather than to itself.
* Every layer of a multi-image background resolves its `url()` against
  the stylesheet, not the document.
* A `min()`, `max()` or `clamp()` inside `calc()` resolves against the real
  percentage basis instead of the window width, and NaN or infinite
  `calc()` results are clamped instead of reaching layout.
* `display: contents` computes to `none` on replaced elements, form
  controls and an outermost `<svg>`, so their fallback no longer leaks.
* `translate()` and the `translate` property keep percentages and font
  units, so the `translateX(calc(-50% + 10px))` centring idiom works, and
  `getComputedStyle().transform` no longer folds in `translate`, `rotate`
  and `scale`.
* `selectorText` and a style rule's `cssText` drop comments and write the
  attribute case flag as ` i]`.
* A flex item with a height of its own keeps it. Stretching ignored
  whether an item's height was `auto` and ignored `min-height`/`max-height`,
  so a 20px item in a 100px row came out 100px tall; only auto-height items
  stretch now, within their min and max, and the relayout that stretching
  triggers keeps the flexed width.
* A `position: fixed` box inside a transformed element is positioned and
  painted relative to that element and scrolls with it, as css-transforms
  requires, instead of being pinned to the window.
* Border-box sizing is honoured in more places: floats are placed by their
  border box, so a Bootstrap-3-style row of padded `width: 50%` columns no
  longer wraps; shrink-to-fit widths stop counting padding twice; a
  percentage height inside a border-box parent resolves against its content
  box; and tables honour `box-sizing` and are border-box by default, so a
  bordered `width: 100%` table no longer overhangs its container.
* A float is never narrower than its longest word, and a `width: 0` child no
  longer widens a shrink-to-fit parent.
* Absolutely positioned boxes shrink-wrap including their margins, padding
  and border, honour `min-height`/`max-height`, find their static position
  inside their parent rather than after it, and treat `margin: auto` as 0
  unless both `left` and `right` are set.
* A last child's bottom margin stays inside `overflow: hidden` blocks,
  floats, inline-blocks and parents with a fixed or minimum height instead
  of escaping through them, and the document contains the body's bottom
  margin.
* Flexbox: images and other replaced items stretch across column
  containers, items move with their line when `align-content` grows it, and
  row and column follow a vertical `writing-mode`.
* `min-content`, `max-content`, `fit-content` and `stretch` work in
  `min-width`, `max-width`, `min-height` and `max-height`, and as flex item
  sizes.
* Text around an HTML comment or a hidden element keeps its spaces ("Text
  after", not "Textafter").
* `<center>` and `align="center|right"` move block-level children too,
  including a table nested in an aligned cell.
* `document.fonts.load()` and `FontFace.load()` wait until the web font has
  loaded, and `document.fonts.ready` resolves with the FontFaceSet, so
  scripts that measure text after them see the web font's metrics.
* A text input's line height is never smaller than `normal`, so a reset
  such as `input { line-height: 1px }` no longer clips the field.
* Viewport units in an iframe resolve against the frame, even when a
  stylesheet sizes the frame, and `calc()` expressions with vw/vh follow the
  current viewport rather than the one in effect when the stylesheet was
  parsed.
* An embedding page can no longer read a cross-origin frame's document.
  `iframe.contentDocument` returned the framed page's DOM and
  `contentWindow` its real window whatever their origin, so any page
  could frame another site and read or script it. For a cross-origin
  frame, or one sandboxed without `allow-same-origin`, `contentDocument`
  is now `null` and `contentWindow` (and `window.frames[n]`) a restricted
  window that allows only `postMessage`, the `location` setter, `closed`,
  `length`, `window`/`self`/`frames`/`parent`/`top`/`opener` and
  `close`/`focus`/`blur`. Same-origin, `about:blank` and `srcdoc` frames
  are unaffected.
* `window.scrollTo()`, `scroll()` and `scrollBy()`, and setting
  `scrollTop`/`scrollLeft` on the root element, move the page. They only
  changed the position script read back; the view stayed where it was. They
  now lay out if needed, clamp to the scrollable range and scroll the
  viewport in both axes. `scrollBy()` called bare, as it almost always is,
  read its position from `undefined` and scrolled to NaN, and `scrollY`
  briefly snapped back to the old position before the view caught up.
* `postMessage`'s `targetOrigin` is checked against the origin of the
  window the message goes to. The check read that window's `location`,
  which answers with the URL of whichever frame's script is running, so
  when a frame posted to its parent, the parent seemed to have the
  frame's own origin: a message addressed to the parent's real origin was
  dropped, and one addressed to any other origin -- the case
  `targetOrigin` exists to stop -- was delivered. The top-level window's
  origin now comes from its document, `"/"` means the sender's own origin
  instead of matching everything, and an origin is compared as an origin,
  not as a string prefix.
* A cross-origin iframe can no longer script the page that embeds it.
  A frame's `parent` and `top` were the embedding page's real window, and
  the frame's global object inherited from it, so a framed site could read
  and rewrite the embedder's DOM, `document.cookie` and `localStorage`
  (only its own storage threw SecurityError). A cross-origin frame now
  gets a restricted window proxy for `parent` and `top`, exposing only
  `postMessage`, the `location` setter, `closed`, `length`,
  `window`/`self`/`frames`/`parent`/`top` and `close`/`focus`/`blur`, and
  throwing SecurityError for anything else. Its global inherits
  `Window.prototype` rather than the parent's global. Messages it
  exchanges with the parent carry the right `source` in both directions.
  A frame sandboxed without `allow-same-origin` is treated the same way,
  whatever its URL.
* A framed document's `document.cookie` reads and writes the cookies of
  the frame's own URL. It used to return the embedding page's cookie
  string, and assigning to it replaced that string. Documents that have no
  browsing context -- from `DOMParser`, `cloneNode` or
  `createHTMLDocument` -- are cookie-averse and return the empty string,
  as HTML specifies. The `Document.prototype` accessor also no longer
  hands the top-level page's cookies to another document it is called on.
* An iframe whose load handler navigates it again no longer hangs the
  page. Loading a queued frame fires its load event synchronously, so a
  handler that set `src` once more re-queued the same frame and the loader
  never returned: timers, painting and the rest of the page starved while
  the renderer spun at full CPU. Each pass now loads a frame at most once;
  a frame queued again waits for the next tick.
* A multi-column block splits a list, not just a run of siblings. The
  column code distributed a container's own children and gave up below
  two of them, so `column-width` on a wrapper whose sole child is an
  `<ol>` -- which is how a Wikipedia reference list is built -- laid the
  whole list out in one column. A lone in-flow block child is looked
  through now and its children distributed instead.
* A multi-column block establishes a block formatting context, as the
  spec says, so it sits beside a float rather than under it.
* A block that establishes a formatting context is placed clear of every
  float it spans, not just those beside its top edge. It was narrowed
  against the float band at its first line and kept that width all the
  way down, so a wider float lower down overlapped it.
* A table column is never narrower than its cells' contents need. The
  auto layout measured a cell's minimum through measure_min_width, which
  returns a specified width when it has one, so a cell's own width
  doubled as its minimum: `width: 1%` on a heading cell -- the idiom
  Wikipedia's navboxes use to shrink a column to its label -- left the
  column one per cent of the table wide and its text ran across the cell
  beside it. The floor is the cell's min-content width now, and when the
  minimums together exceed the width the table asked for, the table grows
  to their sum instead of scaling every column down below what it can
  hold, as CSS 2.1 requires.
* `min-width` and `max-width` on a table cell take part in the column
  measures, clamped max-then-min the way the rest of the box model is.
* A table cell inherits `text-align` from the table or the row. The
  default stylesheet pinned `td, th` to `text-align: left`, which no
  browser's does, so `text-align: center` on a `<table>` or a `<tr>`
  reached the caption and nothing else. `vertical-align` likewise moves
  from a pinned `middle` on the cell to the spec's arrangement -- the row
  groups carry it and the cells inherit -- so a row can set it, and
  `align`/`valign` now map on the row, row-group and column elements too.
* An inline-block whose width is a percentage no longer drags the
  intrinsic width of whatever contains it up to the width of the page:
  a percentage is indefinite while intrinsic sizes are measured, so the
  atomic is measured against its own content.
* `content: '[' / ''` renders just the bracket. The alternative text a
  `content` value carries after a slash, for a screen reader to read in
  place of the glyphs, was drawn as part of the text, so MediaWiki's
  section-edit links came out as `[/ edit ]/`.
* A list item styled `display: inline-block` or `display: block` draws no
  bullet; only a `list-item` display generates a marker.
* `<th>` paints no background of its own and `<caption>` is not bold,
  `<figcaption>` is not italic, and `<figure>` and `<dl>` carry the
  margins the HTML rendering rules specify. None of these are in a
  browser's default sheet, and each showed through wherever a page paints
  its own tables or figures.
* The Android app is about 8 MB smaller. The build staged every shared
  library in the dependency sysroot into the APK, including ones this
  build does not link at all -- llama/ggml, gobject-introspection and the
  unused harfbuzz and pcre2 variants -- so 19 of the 51 libraries per ABI
  were dead weight. Only the engine's `DT_NEEDED` closure is packaged now.

1.0.25:
======
* Restyling a large document is roughly 40% faster. The style-sharing
  cache builds a lookup key for every element on every cascade pass by
  serialising that element's matched declarations; on Speedometer 3.1's
  6,650-node complex-DOM pages that key cost more than the cascade it
  was meant to avoid — 76ms of a 190ms pass. The key is now written
  once into a correctly sized buffer instead of through ~2 million
  incremental byte-array appends, the container-query part of the key
  no longer scans every matched custom property when no container is on
  the stack, and the key hash reads eight bytes at a time. Selector
  gathering also jumps straight to the pseudo-element bucket a matched
  selector belongs to rather than testing all ten. The same pass now
  takes 120ms. Across the 22 loadable Speedometer 3.1 TodoMVC workloads
  the aggregate score improves 31.7% (1.328 to 1.749); Vue-Complex-DOM
  drops from 4810ms to 881ms and jQuery-Complex-DOM from 4948ms to
  3140ms. Layout output is byte-identical.
* The toolbar takes the classic look of the Northstar web browser: a
  raised, softly shaded bar with labelled colour buttons for Back,
  Forward, Reload, Stop, Home, Print and Downloads, bevelled hover and
  pressed states, etched separators, an inset address field that shows
  a page icon when there is no certificate state to report, labelled
  Bookmarks and Menu buttons, and the Nordstjernen logo on a dark tile
  at the far right. Stop stays in place and greys out when nothing is
  loading instead of disappearing, Print and Downloads have their own
  toolbar buttons, the Go button is gone (Enter and the address bar do
  the same thing), and the bookmark button uses the bookmark icon, which
  fills in with a gold star when the current page is saved. The README
  screenshot shows the new toolbar.
* Fullscreen mode is announced: when a page calls requestFullscreen the
  shell overlays a notice at the top of the page naming the site's host
  and saying it is now full screen and that Esc exits, in the style of
  Chrome and Firefox, so a page can't hide the address bar and paint a
  spoofed one without the user being told. The notice stays for five
  seconds and can't be covered by page content. Element fullscreen now
  also ends by itself when the fullscreen tab navigates to a new
  document, when another tab is switched to, or when the tab is closed —
  the header and toolbar used to stay hidden across all three. And per
  the Fullscreen API, requestFullscreen() now needs transient user
  activation: called from a timer or on load without a recent click,
  tap or key press it rejects with a TypeError, fires fullscreenerror
  and logs to the console instead of taking over the screen.
  (Reported by Muhammad Wishal.)
* The nightly .deb installs again on a Debian that is a patch release
  behind the build container. dpkg-shlibdeps copied Debian's FFmpeg
  shlibs floor, which is the exact upstream version of the build host's
  FFmpeg, so the package demanded e.g. libavcodec61 (>= 7:7.1.5) and
  dpkg refused it on a system with 7:7.1.1 although the SONAME, and so
  the ABI, is the same. scripts/pack-deb.sh now relaxes the libav* and
  libsw* floors to the FFmpeg major.minor release (>= 7:7.1) while the
  SONAME-numbered package names keep guarding the ABI, logs the final
  Depends line, and no longer Recommends an external media player the
  shell doesn't launch. The container build then installs the .deb, .rpm
  or .apk it produced and runs the installed browser headlessly, so a
  package whose metadata, maintainer scripts, dependencies or installed
  paths are broken fails the nightly stage instead of reaching the
  download links. (Reported by guest271314.)
* The nightly download links no longer 404 when one platform's build
  fails: scripts/nightly.sh publishes a stage's directory only once it
  holds artifacts and otherwise keeps the previous night's files, a
  container that fails after pack-linux.sh still ships its portable zip,
  the Linux zip link falls back from the Ubuntu build to the Debian or
  openSUSE one, dangling stable links are removed instead of left to
  404, and MANIFEST.txt records why a stage failed and that its files
  are stale.
* The README's download table lists the Windows Store and Google Play
  listings and the source release tags; the nightly build links are
  gone from it.
* The .deb no longer bundles the dynamic loader: pack-deb.sh's core
  runtime deny list matched ld-linux only when a dot followed the name.
* The lexbor encoding module is trimmed to UTF-8. Its 43 legacy codecs
  (Big5, GB18030, EUC-JP, Shift_JIS, the ISO-8859 and Windows code
  pages, UTF-16 and the rest) had no caller: page bodies are decoded
  through uchardet and g_convert, and lexbor's own URL parser only ever
  needs UTF-8, yet its codec lookup table kept every conversion table
  alive through the linker. Dropping them removes 11 MB of generated
  source, about 1 MB from each binary that links lexbor, and the
  per-encoding branches from the URL parser's query serializer.
* The C DOMMatrix in js_canvas.c is gone. The startup polyfill defines
  the 4x4 DOMMatrix, DOMMatrixReadOnly, DOMPoint and WebKitCSSMatrix
  and assigned them over the native constructors on every page, so the
  C class was dead at runtime while canvas getTransform() and the SVG
  getCTM()/getScreenCTM() helpers still minted the C flavour, which
  failed instanceof DOMMatrix. They now construct through the page's
  DOMMatrix constructor, and about 250 lines of duplicate matrix code
  and their declarations are removed.
* The startup polyfill no longer stubs the Credential Management API:
  navigator.credentials resolved every call to null and Credential,
  PasswordCredential, FederatedCredential, PublicKeyCredential and
  IdentityCredential were empty data holders, so sites that feature
  detect WebAuthn or password autofill took a code path that could only
  fail. With the properties absent they fall back to plain forms.
* Repository weight: the 1.0.22, 1.0.23 and 1.0.24 splash frames
  (1.4 MB of PNGs nothing referenced) are deleted, and the QuickJS
  tree drops the harness sources the build never compiled (api-test.c,
  lre-test.c, fuzz.c, ctest.c, cxxtest.cc and the WASI reactor) with
  their CMake and Makefile targets.
* CI moves to current toolchains: CodeQL Action v4 (v3 is retired in
  December 2026), setup-java v6, actions/cache restore and save v6 on
  Windows, FreeBSD 15.1 and NetBSD 11.0 VMs (14.2 and 10.0 are past
  end-of-life), NDK r30 (30.0.16248370) for the Android and CodeQL
  builds, and the V8 15.2 monolith for the js_engine=v8 job. The V8
  backend follows the V8 15 embedder API, which requires a type tag on
  every v8::External and aligned internal-field pointer, and the build
  now passes the pointer-compression defines the just-js monolith is
  built with; docs/V8.md notes that the CREL relocations in those
  releases need lld 19 or newer. NDK r30's bionic refuses
  malloc_usable_size under _FORTIFY_SOURCE=3, so the QuickJS allocator
  reports the usable size as unknown on Android instead of calling it.
* about:start wears the Northstar web browser's splash, carried over
  from that project's scripts/gen-splash.py and retitled "Nordstjernen
  web browser" with this release's version read from meson.build: a
  flat, sunny xkcd-style comic of Noah's ark in Comic Neue lettering,
  with the animal pairs walking two by two to the gangplank behind Noah
  and his clipboard ("Two of each. Yes, even browsers."), a wooden ark
  with a flag, portholes, a chimney and a penguin on deck, blue wavy
  water with a spouting whale, a red-and-white lighthouse with its
  keeper waving from the gallery, gulls, a dove, drifting clouds and a
  yellow sun. Every frame is drawn at three times supersampling and the
  32 frames (walking gaits, a bobbing ark, a fluttering flag, chimney
  smoke, spinning sun rays, flapping gulls) are quantized to one shared
  256-colour palette and squeezed by gifsicle into the embedded GIF.
  scripts/gen-splash.sh regenerates it and writes the first frame to
  data/about-splash-<version>.png; the earlier build-splash-art.py and
  build-splash-gif.py generators are gone.
* Container queries evaluate the full condition grammar: not/and/or
  with nesting, size features in plain, boolean and range form
  (double-sided ranges and math functions included), aspect-ratio and
  orientation, unknown features that make the enclosing condition
  false, comma-separated condition lists, name-only rules and vertical
  writing-mode containers. Invalid preludes are dropped from the engine
  and the CSSOM, conditionText serializes canonically, container-name
  and the container shorthand validate their values, and
  CSSContainerRule exposes containerName, containerQuery and
  conditions.
* image-set() is validated against the CSS Images 4 grammar (url and
  string images, gradients, resolutions in x, dppx, dpi and dpcm, a
  type() hint, no duplicate or missing options) and serializes
  canonically; unicode-range validates and canonicalizes its ranges
  (U+26, u+0-7F, U+4?? read back U+26, U+0-7F, U+400-4FF).
* An absolutely positioned box whose static position comes from an rtl
  ancestor sits at that ancestor's content-right edge, and the used
  values of auto insets are reported physically. font-family keeps
  random-item() and -webkit-generic() items, validates random-item()'s
  arguments and rejects a generic family inside a multi-word name; the
  background-position and object-position shorthands accept CSS-wide
  keywords; specified grid track lists serialize their line names.
* Transitions and animations run for every property. The animation
  engine keeps one channel per (element, property): a transition starts
  whenever a property named by transition-property (or "all") computes
  to a different interpolable value, lengths, percentages, calc()
  mixes, numbers, colours, shadow lists and op-compatible transform
  lists interpolate, visibility flips discretely, and keyframe
  animations sample every declared property between the surrounding
  keyframes, so margins, sizes and colours animate, not just opacity
  and transform. Interpolated values are written into the computed
  style after each cascade, so layout, paint and getComputedStyle see
  the in-flight value; a frame whose animated properties affect layout
  marks the page for relayout.
* The Web Animations surface: document.getAnimations() and
  element.getAnimations() return CSSTransition and CSSAnimation
  objects (stable identity per element, property and run) with
  currentTime and startTime that seek the engine, playState, pending,
  the ready and finished promises, play/pause/finish/cancel, the finish
  and cancel events, transitionProperty/animationName and an
  AnimationEffect whose target, getTiming() and getComputedTiming()
  describe the run. Element.animate() builds a keyframe animation from
  a keyframe list or a property-indexed object with duration, delay,
  iterations, direction, fill and easing.
* Several CSS animations run on one element, one per animation-name
  entry, each with its own duration, delay, timing function, iteration
  count, direction, fill mode and play state from the animation
  longhands; animation-timing-function, animation-iteration-count,
  animation-direction, animation-fill-mode, animation-name,
  transition-property and transition-timing-function are real
  longhands that parse, cascade and serialize on their own, and
  AnimationEvent and TransitionEvent are constructible.
* The animation and transition shorthands parse per comma-separated
  item against their full grammar (a time is a duration before it is
  a delay, an easing keyword, steps() or cubic-bezier(), a
  transition-behavior keyword, "auto" and "none" where allowed),
  expand into their longhands with the omitted ones reset, and
  style.animation, style.transition and getComputedStyle rebuild the
  shorthand from the longhands in canonical order. animation-timeline,
  animation-range-start, animation-range-end, animation-composition and
  transition-behavior are properties.
* Animation and transition events fire by phase: animationstart at
  the end of the delay, animationiteration on every iteration boundary,
  animationend once, transitionrun when the transition is created,
  transitionstart after its delay, transitionend when it completes,
  and the cancel events when a run is interrupted; elapsedTime and
  pseudoElement are filled in. A transition can start from an unset
  value (the property's initial value) and a keyframe that leaves a
  property out fills it from the base style.
* attr() is substituted at cascade time, so content: attr(data-x)
  and attr() in other properties follow attribute changes; an attr()
  URL is tainted. Boxes honour width: stretch and height: stretch,
  grid item margins are resolved against the grid area, a canvas
  takes its width and height attributes as dimension hints, and the
  specified style serializes shorthands from their longhands.
* aspect-ratio keeps its numerator and denominator (16 / 9 reads back
  16 / 9), an absolutely positioned box with both insets set is
  aligned inside them by justify-self and align-self rather than
  stretched unconditionally, and a shrink-to-fit abspos box measures
  against the inset width.
* A declaration whose value is exactly one {}-block is a declaration
  rather than a nested style rule when the CSSOM splits a style block,
  and a custom property keeps a {}-block anywhere in its value.
* A table's max-content and min-content widths are measured column by
  column, as css-tables-3 requires: each column takes the widest cell
  it holds, the columns are summed once with the border spacing, and
  captions widen the result. They used to be the sum of every row, so
  a table in a flex or grid item, a floated infobox and a table nested
  in a cell reported several times their real width.
* position: fixed elements stay anchored to the viewport while the
  page scrolls: paint offsets a fixed box by the viewport origin,
  hit-testing applies the same offset so clicks land on the fixed
  element, getBoundingClientRect reports its viewport position, and
  mouse events carry viewport-relative clientX/clientY with document
  coordinates in pageX/pageY. position: sticky boxes are hit-tested
  where they paint through one shared ns_box_sticky_offset that
  resolves percentage and calc() insets against the scrollport and
  measures a sticky box inside an overflow container against that
  container's padding box.
* box-shadow and text-shadow serialize their specified value in
  canonical order (colour, offsets, blur, spread, inset) with 0
  written as 0px, and reject the forms the grammar excludes: a lone
  length, a fifth length, two colours, inset twice, a negative blur,
  a percentage, or a colour splitting the lengths. A shadow without a
  colour takes currentcolor from the computed color, and rgb(0, 255,
  0) with spaces inside the parentheses no longer splits into tokens.
  A colour keyword keeps its lowercase spelling in the specified style
  and the deprecated CSS2 system colours map to their CSS Color 4
  replacements.
* The background shorthand is parsed layer by layer against the
  css-backgrounds grammar: every comma-separated layer sets all eight
  longhands (image, position, size after the slash, repeat,
  attachment, origin and clip, with the colour on the final layer) and
  a longhand the layer leaves out resets to its initial value, so
  background: red no longer keeps an earlier background-image.
  background-attachment is a property, background-clip,
  background-origin and background-attachment take comma-separated
  lists, paint resolves origin and clip per layer, clips the colour by
  the last layer's clip and positions a background-attachment: fixed
  layer against the viewport, background-position keeps the keywords
  it was written with, background-position-x/-y accept x-start,
  y-end and an edge with an offset, and getComputedStyle composes
  border-radius and background-position from their longhands.
* -webkit-border-radius and the -webkit-border-*-radius corners are
  aliases of the unprefixed properties, and the border-radius
  shorthand is validated before any corner is written: a fifth value,
  a negative radius or a second slash rejects the declaration, and the
  specified value collapses each half as a quad.
* transform is validated function by function against css-transforms:
  each function checks its argument count and types, so translate(1px,
  2px, 3px), scale(6, 7, 8) and skewX(0, 0) are rejected, and the
  specified value serializes canonically (percentages in scale()
  become numbers, rotate(0) reads rotate(0deg), 0 lengths read 0px,
  function names are lowercased). The scale, rotate and translate
  properties get the same treatment, transform-origin and
  perspective-origin follow the position grammar, perspective: 1000
  without a unit is rejected, and transform-box is a property.
* The border, border-top/right/bottom/left, border-block-*,
  border-inline-*, outline and column-rule shorthands are validated
  against their grammar before any longhand is written: a second
  width, style or colour, a negative or percentage width, a unitless
  number other than zero or an unknown keyword rejects the whole
  declaration instead of leaving a partial expansion behind, and the
  outline and column-rule shorthands reset the longhands they leave
  out. outline-style accepts auto.
* Percentage border radii resolve against the box (border-radius: 50%
  on a 200x100 box is an ellipse, not a 50px circle), a corner takes a
  horizontal and vertical radius pair (border-top-left-radius: 10px 5px)
  and the border-radius shorthand honours the vertical radii after the
  slash; paint draws elliptical corners and scales overlapping radii
  down together as css-backgrounds requires, and em/rem pairs resolve
  against the font size in the computed style.
* A transition from transform: none no longer resets the target
  transform to identity: the interpolation built its identity endpoint
  on an alias of the target value and zeroed it in place. An animated
  value propagates to descendants only for inherited properties, so a
  child sharing the parent's opacity, width or transform value is no
  longer animated along with it. A transition on a property paint does
  not read live (box-shadow, filter, border colours, visibility) now
  restyles the page each frame instead of showing the stale value.
* A frame's parent.postMessage() reaches the listeners the top document
  added with addEventListener: the message used to be dispatched
  against the frame's own document, so only window.onmessage saw it.
* Setting style.borderTopLeftRadius (or another corner) on an element
  whose inline style carries a later border-radius appends the corner
  after the shorthand, so the new value wins as the CSSOM requires.
* When several options of a single-choice <select> carry the selected
  attribute, the last one wins, as the HTML selectedness setting
  algorithm requires. HTMLOptionsCollection exposes selectedIndex, and
  every event the engine dispatches carries a composed flag.
* quickjs-ng is at v0.16.2: bytecode constant pools are 8-byte
  aligned, proxy traps consult IsExtensible() on the target so a
  nested proxy's trap is observable, ownKeys must return an object,
  the array iteration builtins poll for interrupts so a long loop
  stays interruptible, TypedArray.prototype.with converts through
  ToBigInt on the 64-bit arrays, and the regexp parser rejects the
  identity escapes that are invalid in unicode mode outside a class.
  WAMR is at 2.4.5: the constant-expression loader rejects an invalid
  reference type in ref.null and the fast-interpreter constant table
  can no longer desynchronise its two passes.

1.0.24:
======
* about:start wears a new splash: the same night scene, painted in an
  impressionist hand with a gilded frame, rendered by
  scripts/build-splash-art.py from the version in meson.build so a
  release regenerates the artwork with two scripts and no image editor.
* Flex layout resolves flexible lengths the way css-flexbox-1 §9.7
  describes: one implementation shared by row, wrapping-row and column
  containers distributes free space with the item freezing loop, so flex
  factors below one scale the free space, flex-shrink is weighted by the
  flex base size, and min/max violations are frozen and re-distributed.
  The automatic minimum size of a flex item is min(content size,
  specified size) rather than the specified size, so width: 200px in a
  100px container shrinks as browsers do; min-content and max-content
  minimums are honoured, and a percentage size on a replaced element or
  text control counts as zero for the specified size suggestion.
* Column flex containers wrap: flex-wrap: wrap and wrap-reverse break
  items into lines against the definite main size, align-content places
  the lines (start/end resolve in the inline axis; space-around and
  space-evenly fall back to start when the lines overflow), a wrapping
  container with a single line is still multi-line, rtl mirrors the
  cross axis, column-reverse packs from the main end, auto margins in
  the main axis absorb free space, and an indefinite-height column sizes
  itself from its items' content contributions so flex: 1 items no
  longer collapse.
* Negative free space overflows in the right direction: space-between,
  space-around and space-evenly fall back to start, flex-end and center
  overflow the start edge, and a scroll container packs overflowing
  content toward its start so it stays reachable; row wrap-reverse
  mirrors lines against the container's definite height.
* scrollWidth and scrollHeight include the scroll container's end
  padding and, in rtl, overflow to the left. offsetTop and offsetLeft
  round negative values to nearest and flush pending layout before
  locating the offset parent.
* The static position of an absolutely positioned flex child honours
  start, end, left and right on justify-content and align-self as
  writing-mode-relative keywords, self-start/self-end use the child's
  own direction, and last baseline aligns to the cross end.
* align-items, align-self, align-content, justify-content, justify-items
  and justify-self parse first baseline, last baseline and the safe and
  unsafe prefixes.
* WPT css/css-flexbox: 1465 -> 1997 of 3670 subtests on a 2026-09
  checkout of the horizontal-writing-mode suites.
* An absolutely positioned box whose containing block is a grid
  container takes that block from its grid-column and grid-row lines,
  as css-grid-1 §9 requires: a line inside the explicit grid resolves to
  the edge of the adjacent track, a line outside it, an unknown name or
  a span resolves to the padding edge after the start/end swap, and
  offsets, percentages and shrink-to-fit sizes resolve against that
  area. A shrink-to-fit abspos box no longer squeezes below its
  min-content width when the area is narrower than its content.
* align-content: stretch on a grid container distributes free block
  space only to rows whose max track size is auto; fixed-length rows
  kept their length.
* An absolutely positioned element with an inline-level display that
  follows inline content takes its static position from the line it
  would have occupied, after the preceding text, instead of the top of
  the block.
* document.fonts.ready waits for the web fonts the page needs: it
  flushes style so pending @font-face loads are requested, resolves
  once the loader is idle and marks the document for relayout;
  fonts.status reports loading meanwhile.
* Alignment properties keep their full specified keyword: safe and
  unsafe prefixes, legacy left/center/right, first baseline (computed
  as baseline) and last baseline parse and serialize, and the
  place-self, place-items and place-content shorthands split two-word
  values and serialize a repeated value once.
* offsetTop/offsetLeft flush pending layout before locating the offset
  parent, so a first read during parsing no longer returns viewport
  coordinates.
* Grid containers with direction: rtl lay their columns out from the
  right, and grid-placed absolutely positioned boxes mirror with them.
* repeat(auto-fit, ...) collapses the repeated tracks that no in-flow
  item occupies, so a card grid with fewer cards than columns stretches
  the remaining fr tracks as browsers do.
* A grid item's percentage height resolves against its grid area when
  the rows it spans have definite track sizes, and an absolutely
  positioned child of a grid container with auto offsets takes its
  static position from its grid area, aligned by justify-self and
  align-self.
* getComputedStyle on a grid container returns the used track sizes for
  grid-template-columns and grid-template-rows, with explicit line names
  in place, as CSSOM requires.
* Track lists resolve em against the element's own font size and calc()
  percentages against the track axis; an auto track grows to its
  max-content contribution before fr tracks share the remainder, fr rows
  fill a definite container height, over-constrained minmax() rows
  shrink toward their minimum, percentage rows in an indefinite-height
  grid re-resolve against the final height, and an auto column measures
  its content with real text metrics.
* The grid track-list parser rejects negative sizes, stray commas,
  consecutive or trailing-only line-name lists, reserved words as line
  names, a second auto-repeat and non-fixed tracks beside one, and
  supports line names inside repeat().
* JavaScript can be turned off: an "Enable JavaScript" toggle in
  Settings parses pages with scripting disabled (so noscript content
  renders) and skips script execution entirely — the user decides what
  runs, in the old Mozilla tradition.
* about:mozilla shows the maroon page every browser of this lineage
  owes its readers, and about:config opens the settings page.
* The status bar reads "Done" for a moment when a page finishes
  loading, as it always did.
* Fixed a use-after-free of the session URL: timer, event-dispatch and
  requestAnimationFrame callbacks saved the current URL pointer and restored
  it unconditionally after the callback, so a handler that navigated (a
  fragment click, location.hash =, history.pushState) left the engine
  reading and double-freeing a freed URL. The URL is now restored only when
  it was actually swapped for an iframe realm.
* Added view-source: — Ctrl+U / "Page Source" in the menu shows the current
  page's HTML with classic syntax highlighting. Only chrome-initiated
  navigations can use the scheme; web content is refused.
* input.showPicker() and select.showPicker() are implemented per spec:
  InvalidStateError on disabled or readonly controls, NotAllowedError
  without a user gesture, and a successful call consumes the activation.
  WPT show-picker suites pass 129/129.
* stepUp()/stepDown() follow the spec: they throw InvalidStateError on
  non-numeric input types and step="any", honor the per-type default
  step and scale, round to the step grid, clamp to min/max, and
  serialize date, month, week, time and datetime-local values back to
  their canonical strings. WPT input-stepup 53/53 and time 32/32.
* The color input sanitizes through the CSS color parser: keywords,
  rgb() and #rgb shorthand normalize to lowercase six-digit hex, and
  surrounding whitespace is stripped.
* Label association follows the spec: label.control resolves the for
  attribute against the label's own tree to the first element with that
  id (null when it is not labelable or the attribute is empty),
  label.form returns the associated control's form owner, and .labels
  is a live NodeList.
* The WPT harness's testdriver bridge grants real user activation for
  simulated gestures, and the __nsWpt* hooks are inert outside the
  harness.
* Form constraint validation follows the HTML spec much more closely:
  ValidityState flags are computed for disabled and readonly controls
  (bars from validation affect willValidate, not the flags), valueMissing
  is suppressed on disabled/readonly controls, the pattern attribute
  compiles as a JavaScript regular expression with the v flag (invalid
  patterns are ignored) and applies to each address of a multiple email
  input, tooLong/tooShort fire only after a user edit as the spec's dirty
  value flag requires, and willValidate is false inside a datalist.
  WPT form-validation: patternMismatch, tooLong, tooShort, typeMismatch
  and badInput suites now fully pass.
* Live HTMLCollection/NodeList property semantics follow WebIDL: silent
  sloppy-mode failures and strict TypeErrors for read-only indexed and
  named properties, spec-compliant descriptors, expando support, and
  Object.keys listing only indices; moveBefore() is ParentNode-only;
  replaceChildren() queues a single mutation record; Node.isConnected is
  true for any node whose root is a document.
* JavaScript can be turned off: an "Enable JavaScript" toggle in
  Settings (and the NS_NO_JAVASCRIPT=1 environment variable) parses
  pages with scripting disabled, so noscript content renders, and skips
  script execution entirely.
* about:mozilla shows the maroon page every browser of this lineage
  owes its readers, and about:config opens the settings page.
* The status bar reads "Done" for a moment when a page finishes
  loading, as it always did.
* Popup blocking, in the Firefox 1 tradition: window.open only navigates
  when called within five seconds of a real user gesture (click, key
  press, touch), consumes that activation, and logs blocked attempts to
  the console. navigator.userActivation now reports the live activation
  state instead of constants.
* Ctrl+Enter in the address bar completes a bare name to www.name.com,
  Shift+Enter to .net and Ctrl+Shift+Enter to .org, as in classic
  Firefox.
* Ctrl+Shift+R and Ctrl+F5 reload the page bypassing the HTTP cache.
* Ctrl+D bookmarks the current page, with a matching "Bookmark This Page"
  menu entry, in the classic browser tradition.
* about:book — every browser of the lineage carries its Book.
* Updated the ns-pango subproject pin to the latest upstream commit.
* Optimized event dispatch throughput by lazily evaluating composedPath()
  arrays on demand using active dispatch paths.
* Added node-level listener filtering with NS_NODE_HAS_LISTENERS flag, skipping
  listener array iterations on intermediate DOM tree nodes without listeners.
* Bound standard Event prototype methods (preventDefault, stopPropagation,
  stopImmediatePropagation, cancelBubble, composedPath) directly on Event.prototype.
* Accelerated document.createElement with lowercase ASCII fast paths and
  ASCII-first element name validation.
* Optimized inline style property conversions (camel_to_kebab) and empty initial
  inline style updates avoiding intermediate GString allocations.
* Optimized dataset property lookups by matching target attribute names directly
  without repeated per-attribute string allocations.
* Bypassed redundant storage allocations and event dispatches when setting
  identical localStorage/sessionStorage values.
* Added HTMLDialogElement.showModal() standards compliance validating open and
  connected document state.
* Aligned structuredClone with specification requirements (argument validation
  and transfer options support).
* Optimized CSS.escape for simple identifiers and bounded selector and form
  ancestor traversals.
* Optimized DOM textContent and innerText get/set paths with zero-allocation
  string returns for empty and single text child nodes.
* Optimized DOMTokenList (classList) add, remove, and toggle with single-token
  fast paths avoiding token array and parser allocations.
* Accelerated DOM hierarchy validation in pre-insert checks and ancestor-or-self
  queries with O(1) leaf child checks and bounded traversals.
* Optimized DOM attribute operations (getAttribute, setAttribute, hasAttribute,
  toggleAttribute, removeAttribute) with zero-allocation lowercase ASCII fast paths
  and single-pass string conversion.
* Fast-path selector matching in Element.matches() and Element.closest() for simple
  class (.cls), tag (tag), and id (#id) selectors, avoiding full CSS selector AST
  allocations on delegated event lookups.
* Subtree getElementById queries leverage document root ID indexes and bloom filters
  prior to falling back to full recursive DOM tree traversal.
* CSS line-height unit calculations now cover all root font relative units
  (rem, rlh, rex, rch, rcap, ric), element font relative units (lh, ex, ch,
  cap, ic), viewport percentage units (vh, vw, vmin, vmax, vi, vb, svh, svw,
  lvh, lvw, dvh, dvw), and container query units (cqw, cqh, cqi, cqb, cqmin,
  cqmax) across text layout and painting.
* DOMTokenList (classList) optimizes add and remove operations to skip
  redundant attribute re-serializations and DOM mutation dispatches when the
  underlying token set is unchanged.
* AbortSignal spec compliance: AbortSignal.abort() and AbortSignal.timeout()
  generate standard DOMException instances (AbortError, TimeoutError),
  AbortSignal.any() accepts any iterable signal collection, and prototype
  chains correctly inherit from EventTarget.
* Norwegian regional Accept-Language configuration adds complete fallbacks
  across Bokmål (nb), Nynorsk (nn), and generic Norwegian (no) locales.
* Parser and layout loop safety improvements: bounded counter formatting
  buffers, guaranteed forward pointer progress in pseudo content resolution,
  unified depth-bounded ns_node_root tree traversals, and cycle-protected
  document order comparisons.
* Selection highlights the text it is actually on. The highlight was
  painted as one flat pass over the finished page, in document
  coordinates, after everything else had been drawn — so inside a
  scrolling box it landed wherever the text would have been unscrolled,
  spilled past the box it belonged to, and ignored the transforms and
  clips the glyphs themselves were drawn under. It is now drawn where the
  text is drawn, from a range table computed once per frame and looked up
  per box, so it inherits that box's scroll offset, transform and clip by
  construction. It also goes down before the glyphs rather than over them,
  which is what lets `::selection` carry an opaque background without
  washing the letters out. Selection hit-testing gained the two things the
  click path already had: it adds a scrolling ancestor's scroll offset as
  it descends, so a click inside a scrolled `<div>` selects the line under
  the pointer instead of the line that would be there at scroll zero, and
  it stops at a box that clips its children, so a point below an
  `overflow: hidden` container no longer reaches the content clipped out
  of it.
* `user-select` and `::selection` are read from where the style is. Both
  were looked up on the inline box's own `style`, which inline boxes do
  not carry — the style lives on an ancestor — so the pointer was always
  null and both properties were silently ignored: `user-select: none` text
  copied anyway, and a page's `::selection` colours never appeared. Both
  now walk to the nearest ancestor that has a style, the same way the text
  painter finds its font.
* Copied text reads like the page. Every inline box ended with a newline,
  so a paragraph broken into runs by a `<b>` or an inline-block came out
  one word per line; a `<br>` — which layout carries as U+2028 — came out
  as nothing at all. A line break is now emitted where a block boundary
  is, U+2028 and U+2029 become newlines, and the zero-width characters
  `<wbr>` leaves behind are dropped. Text under `user-select: none` is no
  longer collected at the ends of a range, only in the middle of one.
* Double-click selects a word, triple-click selects the block. Both
  gestures previously did what a single click did. Word edges come from
  the shaper's own break attributes rather than an ASCII rule, so they
  hold for scripts that do not put spaces between words. Shift-click
  extends the existing selection from its anchor instead of dropping it,
  and a drag that selected text no longer activates the link it ended on —
  releasing the mouse after selecting a sentence containing a link used to
  navigate away.
* A page can see and set the selection it is showing. `getSelection()`
  reported an empty, collapsed selection no matter what was selected on
  screen, because nothing ever told the JavaScript engine what the
  selection was; `document.execCommand` answered false to everything and
  `navigator.clipboard.write` was absent. The page selection now flows
  into the engine on every change, so `toString()`, `type`,
  `isCollapsed`, `rangeCount` and the range's `getBoundingClientRect()`
  describe the real one; `execCommand` performs `copy`, `cut`,
  `selectAll` and `unselect`, with `queryCommandSupported` and
  `queryCommandEnabled` agreeing about them; and `clipboard.writeText`
  and `clipboard.write` reach the system clipboard by way of an
  `X-Clipboard` flag on the render response, which the shell answers with
  a `/clipboard` fetch from the renderer. Reading the clipboard stays
  refused — there is no permission prompt behind which to put it.
* The heading of `about:nordstjernen` names the browser's version and
  nothing else. It carried the JavaScript engine's version beside it, which
  told a reader looking for the browser's own version that there were two
  numbers to choose between; QuickJS is still listed under *Version &
  libraries* further down the same page, next to lexbor, Pango, SQLite and
  the rest, which is where a reader goes looking for it.
* The heading drops its tagline too. *The unique, legendary web browser*
  sat between the version and the sentence that says what the browser is,
  making a reader read past an advertisement to reach the description.
* A custom property can say what it holds. `@property` was parsed for its
  `inherits` and `initial-value` descriptors and nothing else: the `syntax`
  descriptor was read past, so the one thing the rule exists to declare —
  the grammar its value has to match — was never checked, and
  `CSS.registerProperty` was absent entirely, which is how most of the
  libraries that use registered properties reach for them. Both now go
  through one grammar: `src/css_prop_syntax.c` parses a `<syntax>` string
  into its alternatives and multipliers and matches a value against it,
  including the arithmetic, so `calc(7in - 12px)` is a `<length>` and
  `calc(5px + 10%)` is not, and so is the computational-independence rule
  that makes `10em` a legal length in a stylesheet but not as an initial
  value. A rule missing `syntax` or `inherits`, or carrying an
  `initial-value` its own syntax rejects, is now dropped rather than half
  honoured, and a declaration whose value does not match the registered
  syntax falls back to the initial or inherited value instead of being
  taken at face value. `CSS.registerProperty` throws the errors the API is
  specified to throw — `SyntaxError` for a name, syntax or initial value it
  cannot accept, `InvalidModificationError` for a second registration — and
  the CSSOM grew `CSSPropertyRule`, so `name`, `syntax`, `inherits` and
  `initialValue` read back off the rule.
* Viewport units inside a frame measure that frame. `vw`, `vh`, `vmin`,
  `vmax` and their small/large/dynamic spellings resolved against the
  top-level window wherever they appeared, so a 200x100 iframe laid its
  `100vw` box out at the width of the whole browser. The cascade now
  swaps in the frame's own viewport while it walks a nested document,
  the way the media-query evaluation already did, and only when the
  frame's size is actually known — from its `width`/`height` attributes,
  an inline size, or the last layout — so a frame whose size has not been
  measured yet keeps the behaviour it had rather than guessing at
  300x150. `data/fixtures` aside, a 200x100 `<iframe>` whose content asks
  for `100vw` by `50vh` now lays that box out at 200x50.
* A registered custom property computes its value instead of carrying the
  text it was written with. `<length>` arrives in pixels whatever unit it
  was authored in and whatever the element's font size is, a
  `<length-percentage>` that mixes the two serializes as the `calc()` the
  CSSOM specifies, `<angle>` lands in degrees, `<time>` in seconds,
  `<resolution>` in `dppx`, `<integer>` rounded, `<color>` as `rgb()` with
  `currentcolor` resolved against the element's own colour, `<string>`
  requoted, and a `<transform-function>` with its arguments computed the
  same way. The computation runs once the element's font metrics are
  known, so `--x: 14em` on a ten-pixel element is `140px` and an inherited
  value keeps the number its parent computed.
* A property registered through `CSS.registerProperty` now restyles the
  page. Incremental restyle skips a pass when no stylesheet has changed,
  and a registration changes no stylesheet, so a property registered from
  script had no effect until something else happened to dirty the tree.
  The registration is part of the signature that decides whether the pass
  can be skipped.
* `@counter-style` rules with a name no counter style may take — `none`, a
  CSS-wide keyword, or one of the six predefined styles the spec forbids
  overriding — are dropped instead of entering the stylesheet, and
  `CSSCounterStyleRule` reports its `name`.
* The CSS tokenizer decides what starts an identifier the way the Syntax
  specification does. A `-` was treated as the beginning of a name whatever
  followed it, so the subtraction in `calc(7in - 12px)` tokenized as an
  identifier rather than an operator; a hyphen now only starts a name when
  a name character, a second hyphen or an escape follows it, and the same
  rule governs the unit after a number and the name after `@`.
* The page itself snaps. `scroll-snap-type` worked on scroll containers
  only, which left out the arrangement almost every page that asks for
  snapping actually uses: full-height sections down the document, with the
  property on `html` or `body` and nothing overflowing in between. The
  document scroller is the one scroller that is not a box — the shell owns
  its offsets in a `GtkAdjustment`, and the renderer only learns them as the
  coordinates it is asked to paint from — so the snap positions its
  descendants offer were never consulted. The solver no longer derives the
  snapport from the scrolling box: it takes one, so the viewport can supply
  its own, and the renderer resolves the proposed offset against the root
  element's `scroll-snap-type` on the way into a frame and returns the
  snapped one to the shell on the render response, the channel
  `scrollIntoView` and fragment navigation already use to move a tab's
  scroll position. Every source of document scrolling therefore snaps — the
  wheel, the scrollbar, the keyboard — because each ends in a frame rendered
  at a new offset, and it works the same in process-per-tab and
  `--single-process` because both drive the same renderer service.
  `scroll-padding` on the root insets the viewport snapport as it insets a
  box's, `mandatory` and `proximity` keep their meanings, and the horizontal
  axis now rides back beside the vertical one. `html` is the root element
  and `body` is honoured as a source too, matching how the root's `overflow`
  is already read here. Verified on
  `data/render-tests/scroll-snap-viewport.html`, four sections of `100vh` in
  a 953-pixel viewport: proposed offsets of 100, 600 and 1400 resolve to
  953, 2000 to 1906 and anything past the end to 2859, while
  `scroll-snap.html`, whose snapping is all inside boxes, and the ordinary
  layout pages resolve to no snap and scroll exactly as before.
* `Ctrl+P` prints in the default process-per-tab mode. Printing paginates
  in the renderer and drew onto cairo recording surfaces, which cross no
  process boundary, so every window that was not started with
  `--single-process` — which is every window, by default — answered
  *Nothing to print*. The renderer now rasterises each finished sheet and
  writes it into the runtime directory the way the page export already
  hands a file across, and the shell loads the sheets back and feeds them
  to the same `GtkPrintOperation` as before, dividing out the scale they
  were rendered at. Pagination, `@media print`, `@page` and the `break-*`
  properties are the single code path they always were; single-process
  printing still hands the recording surfaces over untouched, so it keeps
  printing vectors.

1.0.23:
======
* The browser prints. `Ctrl+P`, or *Print…* in the menu, lays the page out
  for paper and hands the sheets to the operating system's own print
  dialog through `GtkPrintOperation` — CUPS on Linux, the Win32 printer
  dialog on Windows, the Cocoa panel on macOS — so no printing code is
  written per platform and no dependency is added. The sheets are cairo
  recording surfaces, which cross no process boundary, so the print
  action needs the in-process renderer: it works under
  `--single-process` (or `NS_SINGLE_PROCESS=1`) and reports *Nothing to
  print* otherwise. `--dump=print:FILE` renders the same pagination to a
  multi-page PDF from any mode and needs no printer.
* The engine gained the parts of CSS a printer needs. `@media print` now
  matches — the media type was hardcoded to `screen`, so a page's print
  stylesheet was simply ignored. `@page` sets the sheet size from a name
  (`A4`, `letter`, `legal`, `ledger`, the A/B series), from one or two
  lengths, or from `portrait`/`landscape`, along with its margins.
  `break-before`, `break-after` and `break-inside` — with the legacy
  `page-break-*` spellings mapping onto them and `always` becoming `page`
  — decide where a sheet may end. A sheet is cut at a forced break when
  one falls before the page is full; otherwise the cut is pulled up above
  any box it would split, which is every leaf box, every line of a
  paragraph, and anything asking for `break-inside: avoid`. Printing
  restores the on-screen layout afterwards.
  `data/render-tests/print-pagination.html` comes out as three A4 sheets
  with every card whole, the `@media print` paragraph swapped in for the
  screen one, and the forced break starting sheet three.
* `offsetLeft` and `offsetTop` are measured from the offsetParent again.
  Both returned a document coordinate, built from the margin box rather
  than the border box, so an element inside any positioned ancestor
  reported where it sat on the page instead of where it sat in its
  parent. CSSOM View asks for the distance from the offsetParent's
  padding edge, with a statically positioned `body` or root the exception
  every engine makes. A great many pages measure this way, and so does
  the `checkLayout` harness most of WPT's layout tests are written
  against — in the sibling GPL edition, where this was measured, fixing
  it plus the flex change below took `css/css-flexbox` from 653 to 1437
  of the same 3535 subtests.
* An absolutely positioned child of a flex container is placed where the
  flexbox specification says. It landed at the container's content-box
  origin whatever the container asked for; CSS Flexbox 4.1 gives it a
  static position from `justify-content` and its own `align-self`, as
  though it were the only flex item, and `flex-direction: *-reverse`,
  `flex-wrap: wrap-reverse` and `direction: rtl` each turn around the
  axis they govern. Vertical writing modes are not covered — flex layout
  itself is horizontal-only here.
* `flex-wrap: wrap-reverse` puts the first line last. Lines wrapped, but
  the cross axis was never turned around, so the first line stayed at the
  top and `align-content: flex-start` stayed at the top with it. The
  lines are now mirrored within the container after `align-content` has
  placed them, and each item within its line, which reverses
  `align-items: flex-start`/`flex-end` along with them.
* CSS Scroll Snap. `scroll-snap-type` on a scroll container, with
  `scroll-snap-align` on the things inside it, moves the container onto
  the nearest snap position once a scroll lands — from the wheel, and
  from `scrollTop`/`scrollLeft`. `scroll-padding` on the container and
  `scroll-margin` on an item inset the snapport and outset the snap area,
  both as shorthands and per side; `mandatory` always snaps, `proximity`
  only from within half a page. A wheel tick shorter than the gap between
  two snap positions still moves the reader forward rather than falling
  back to the one behind. `data/render-tests/scroll-snap.html` walks both
  axes. This is scroll containers only: the document scroller belongs to
  the window, not to a box, so `scroll-snap-type` on `html` or `body`
  does nothing yet.
* A regular expression whose `v`-flag class contains the empty string,
  `/[\q{}]/v`, no longer writes outside the string set it is building.
  The JavaScript engine is refreshed onto quickjs-ng 0.16.1, which
  carries that fix along with the iterator proposals — `Iterator.concat`,
  `Iterator.prototype.join`, `includes`, and the chunking proposal's
  `chunks` and `windows`, with `take` and `drop` now rejecting
  out-of-range limits — resizable externally managed ArrayBuffers, and a
  parser that no longer rescans the line to report an identifier's
  column.
* A processing instruction is parsed as one. `<?target data?>` produced a
  comment that the engine then took apart again by hand, guessing where
  the target ended; the HTML parser now implements the specification's
  own rules, so the node carries a real target and data, a target the
  specification disallows stays a comment, and serializing one writes
  `<?target data>` instead of dropping the target on the floor — which
  the XML parser's processing instructions had been doing all along.
  Two URL fixes come with the same refresh: a caret in a path is
  percent-encoded, and a URL that has userinfo but no host is rejected.
* Two paragraphs measuring the same words come out the same width. The
  text shaper cached a piece ending in a space together with the kerning
  the following word had induced on it, so "Type of" was laid out narrow
  after "Type A" had been drawn, and which paragraph came out wrong
  depended on what the process had already done. The shaper now asks
  HarfBuzz which pieces are safe to store.
* The bundled MP3 decoder and the WebGPU headers match their upstreams
  again — the headers move to wgpu-native v29.0.1.1, the release the
  build actually downloads.
* The third-party notices name every library in the binary. pl_mpeg and
  minimp3 are compiled into it and were listed nowhere; the wgpu-native
  headers were missing too; lexbor's `NOTICE` file, which Apache 2.0
  requires be propagated, was recorded as not existing.
* The build instructions install what the build requires. The
  Debian/Ubuntu, Fedora and openSUSE package lines omitted FFmpeg, which
  `meson setup` has refused to proceed without on Linux since inline WebM
  landed, so following them exactly produced a failing configure.
* The documentation stops pointing at files that are not there:
  `src/mobile.c`, `src/tab_worker.c`, `src/env.c`, `src/media.c` and
  `docs/ipc-http-experiment.md` were all cited by name. The mobile-site
  note described a per-host list the browser does not have — the choice
  is made once for the whole build — and the threading model still
  documented a per-tab worker thread that the move to process-per-tab
  removed.
* Every link to the project's own repository points at
  nordstjernen-web/nordstjernen-browser, its new home: the README badges
  and release-tag link, the AppStream metainfo, the Debian, RPM and Alpine
  packaging, the Java POM and Gradle metadata, and the build documentation.
  The Alpine recipe also follows the archive root GitHub names after the
  repository rather than the package, so the tarball it fetches for 1.0.22
  unpacks where the build looks for it, with a checksum to match.
* A distribution package built from a release tarball declares the FFmpeg
  libav\* libraries it needs. meson has required them on Linux and Windows
  since inline WebM landed, but the Debian control file, the OBS and Fedora
  spec files, the Alpine APKBUILD and the source-RPM recipe all still
  described them as optional, so each of those builds failed at configure
  time on a clean machine. The nightly container build no longer treats a
  missing FFmpeg as a reason to carry on either, and the Linux and Windows
  build guides list the packages.
* A package built where the build host has no network reaches the system
  Pango. ns-pango is cloned by meson at setup time, which an OBS worker, an
  sbuild chroot, a mock root and Alpine's build phase all forbid, so those
  recipes now pass -Dns-pango=disabled and build-depend on Pango itself.
* A source RPM carries a version rpm accepts and installs what it built.
  The generated spec spelled a development version with the hyphen rpm
  rejects, and hand-installed two of the four binaries with none of the
  runtime data, so the browser it packaged could not start a renderer or
  find its translations.
* The Debian tree has the changelog dpkg-buildpackage needs, and its rules
  file configures the build the way the packaging documentation says.
* The documentation index lists the architecture, iOS, extensions,
  vendored-engine and wpt-fast documents that were missing from it, and no
  longer points at two documents that are not there. The OBS packaging notes
  no longer describe a _service file the repository does not carry, and the
  HTML compatibility table describes what actually happens when an
  undecodable media element is clicked.
* The Android, iOS and Java hosts compile again. Each of them builds its
  bridge against the engine's headers with nothing on the include path but
  `src/`, and printing put `<glib.h>` and `print.h` into three of those
  headers — `libnordstjernen.h`, which is the one the engine installs,
  `renderer_serve.h` and `rproc_http.h`. `print.h` reaches on to cairo, the
  CSS engine and the layout tree, none of which those builds can see, so
  the Java bridge, the Android bridge and the Swift bridging header all
  stopped finding what they included while the engine itself, which has the
  full include path, kept building. Every print entry point moves to
  `print.h` beside the pagination it drives, and the three headers are
  self-contained again.
* The Android bridge attaches its in-process renderer again: the callback
  that hands one over began returning the connection the print path needs
  rather than a status code, and Android's still returned the status code,
  which its compiler rejects outright.
* The NetBSD build no longer rests on one package mirror. It took whatever
  `pkg_add` defaults to, which is `ftp.netbsd.org` over plain HTTP, and when
  that host refused connections every dependency failed to install and the
  job died before a compiler ran. It now names the CDN the NetBSD sets
  already come from first and that host second, and retries, so one mirror
  being down is no longer the end of the build.

1.0.22:
======
* A mouse or pointer event carries the window it was dispatched in.
  `UIEvent.view` is that window and every event the engine synthesised
  for a click, a drag or a hover reported null, which is a value no
  browser produces.
* A grid item placed by area name is aligned to its row. Items placed
  through `grid-template-areas` went down a layout path that never read
  `align-items` or `align-self`: each was put at the top of its row at its
  own height, where the default is to stretch. On lichess.org the lobby's
  start-button column stayed 179 pixels tall beside a 600-pixel
  neighbour, and the player counts pinned to its bottom edge came to rest
  on top of the buttons.
* A single flex line is as tall as the container says. A row flex
  container with a definite height has a line exactly that tall, and a
  stretched item gets that height whether its content fits or not. The
  line was sized to the taller of the container and its content instead,
  so one over-tall item dragged the whole line past the height the author
  asked for.
* A percentage inside `min()`, `max()` and `clamp()` is measured against
  the box rather than the viewport. These functions were folded to a
  single pixel value during parsing, when the only basis available was
  the viewport width, so the comparison ran against the wrong number: in
  a 400-pixel column `min(300px, 50%)` came out 300 instead of 200.
* A dialog opened from script renders, and a modal one is centred.
  `showModal()` and `show()` set the open attribute without telling the
  style engine, so the element kept the `display: none` it was matched
  with at parse time and had no box at all. The user-agent sheet now also
  carries the modal rule the HTML specification defines, and an
  out-of-flow box asking for an intrinsic height is no longer stretched
  between its top and bottom offsets.
* A grid track can be measured in any length unit. `grid-template-columns`
  understood px, %, fr, em and rem, and quietly dropped every track it
  could not read, which moved each remaining track one place to the left.
  lichess.org asks for five columns with a `1vw` margin at either end;
  three arrived, its `<main>` landed in a column with no room in it, and
  the whole site laid out zero pixels wide down a 26000-pixel page. Track
  lengths now go through the same reader as every other length, and a
  track list that still cannot be read is discarded whole rather than
  closed up, because a missing list leaves the columns to
  `grid-template-areas` while a shifted one leaves nothing standing.
* A positioned box answers the pointer in the layer it paints in. Hit
  testing ranked a box against its own siblings and nothing else, so a
  fixed, high `z-index` overlay never rose above content in another branch
  of the page even though the painter drew it on top: an overlay's button
  was visible and the content behind it took the click. Positioned boxes
  are now collected and tried in the order the painter flushes them, and a
  modal dialog in the top layer is tried before the document.
* An `<svg>` that paints nothing hands the pointer to what is under it.
  SVG hit-tests as `visiblePainted` -- a shape answers where it draws and
  nowhere else -- but the engine lays an `<svg>` out as one replaced box
  and let that box answer for its whole rectangle. chess.com stretches an
  `<svg>` of rank and file labels over the board, so a piece could be
  picked up and never put down: the labels swallowed the pointerup that
  ends the drag.
* An SVG `font-size` attribute survives the cascade. A presentation
  attribute is author style at the very bottom of the cascade, so a real
  declaration beats it but inheritance must not; the renderer read the
  attribute and then overwrote it from the computed style, which always
  has a font-size because font-size is inherited. Every `<text>` drew at
  the page's font size scaled by the viewBox -- chess.com's board
  coordinates came out four times their size and spilled across the board.
* A worker shares the storage of the page that started it. The worker
  runtime was built without a storage partition, and IndexedDB reads that
  partition to find the origin's databases, so every `indexedDB` call
  inside a worker threw "Storage is unavailable" and chess.com reported
  its opening database as unusable.
* `prefers-color-scheme` answers with the scheme the desktop is actually
  using. Nothing set it at all, so every window reported "light" however
  dark the desktop was: a site's dark stylesheet never applied. The shell
  judges the scheme by the luminance of the foreground colour the theme
  resolves for its window -- which holds for any theme, rather than only
  the ones that set GTK 3's `gtk-application-prefer-dark-theme` -- honours
  an explicit `color_scheme` setting over it, hands the answer to each
  renderer it starts, and re-evaluates when the theme changes underneath a
  running window. The internal pages -- start, about, settings, history and
  the error page -- gain the dark half they never had, and stop declaring
  themselves light-only.
* Text is laid out through three new ns-pango caches: the unicode break
  attributes, the items a paragraph was cut into, and shaping keyed on a
  word rather than on a run. Intrinsic sizing means the same paragraph is
  laid out for min-content, for max-content, for the real width and again
  to paint it, and the line breaker cuts a run wherever a line ends and
  shapes the piece again -- so a paragraph shared no cache entry even with
  itself. On this repository's own test page the shape cache now serves
  1512 lookups against 67 misses, the break cache 312 against 24 and the
  item cache 277 against 63. The layout dump is byte-identical to the one
  the previous pin produced.
* The status line behaves like a status line. It held a permanent row under
  the page saying "Done" for the life of every visit, and hovering a link
  wrote the URL there while moving off it wrote nothing -- so the last link
  the pointer touched stayed on screen indefinitely, naming a destination
  the cursor had left. It now floats over the bottom-left corner of the
  page, appears only while it has something to say, and clears when the
  pointer leaves a link or a load finishes. Notices the window raises
  itself -- a bookmark added, a session recovered -- fade after five
  seconds.
* The toolbar menu is grouped into tab, view, page and tool sections and
  gains Zoom In/Out/Reset, Full Screen, History and the two save entries;
  zoom, full screen and history had keyboard shortcuts but no visible
  affordance, and saving a page was reachable only by right-clicking it.
  Items whose action carries more than one accelerator name one explicitly,
  because GTK shows nothing when a shortcut is ambiguous. A popover menu
  also takes the height its items need: GtkPopoverMenu builds a section's
  separator after the popover has negotiated its size, so the last item was
  always clipped.
* The page zoom is shown beside the address bar while the page is scaled,
  and resets when clicked. Zooming said "Zoom 121%" in the status line for a
  moment and then left no trace, so a window could sit at any magnification
  with nothing on screen admitting it -- and 121% is where successive tenths
  land. The steps now follow the usual ladder: 90, 100, 110, 125, 150.
* The bookmark button says whether the page is bookmarked, carrying a hollow
  star that fills once the page is on the list, and the popover's action
  becomes "Remove this bookmark" where it would otherwise do nothing --
  adding a URL already on the list is a no-op, so pressing it a second time
  silently did nothing.
* Escape in the address bar reverts it to the URL the window is showing.
  It restored focus to the page but left whatever had been typed sitting in
  the bar, naming a page that was not on screen. Escape also closes the find
  bar, which its own tooltip already promised, and stops a load in progress.
  The connection indicator moves inside the entry, where the padlock
  belongs, and the title bar no longer reads "Nordstjernen 1.0.22 —
  Nordstjernen 1.0.22" on a page with no title of its own.
* A text field shows as much of its value as it has room for. An `<input>`
  was given a visible window of exactly as many characters as its `size`
  attribute names, and CSS that widened the control -- `flex-grow`,
  `width: 100%` -- only stretched the painted frame, so a field with room
  for sixty characters still scrolled its text away after twenty. The
  window now comes from the used content width: it grows when the box is
  wider than `size` asks for, and shrinks when a definite CSS width is
  narrower, where the value used to be painted straight through the
  control's own border.
* An inline-block, inline-flex or inline-grid sits on the line's baseline.
  Two things put it elsewhere: the shape rect handed to the shaper aligned
  the box's bottom margin edge to the baseline -- only the fallback CSS 2.1
  gives a box with no in-flow line boxes of its own -- and the placement
  pass then ignored the shaper's answer and pinned the box to the top of
  the line. A badge or button written inline with a sentence was drawn with
  its own text floating above the words either side of it, and the line box
  grew to cover the overshoot.
* A `#fragment` stays anchored while the rest of the page loads. The scroll
  position was computed once, from whatever layout existed at navigation
  time, so images decoding above the target pushed it down afterwards and
  the view landed short of the heading it was asked for. The target is held
  and its position re-applied until the document goes quiet or the reader
  scrolls away.
* A navigation that ends with nothing to render gets an error page whatever
  its scheme. Only `https://` failures had one, so a missing `file://` path
  came back 404 with an empty body and rendered as a blank white page --
  no heading, no URL, nothing to act on. The classifier has also stopped
  blaming the network for everything it does not recognise: an unmatched
  transport message falls through to the status code, and a file URL is
  described as a file rather than as an unreachable server.
* Resolving an `ex`, `ch`, `cap` or `ic` length no longer shapes four probe
  glyphs every time. The metrics oracle built a layout and measured `x`,
  `H`, `0` and the water ideograph on each call, and the cascade asks once
  per element a rule matches, so a stylesheet that sizes fields in `ch`
  paid for four layouts on every one of them. The answer depends on nothing
  but the family, size, weight and slant, so it is measured once per font
  and kept until the font map changes under it.
* An IndexedDB write no longer walks the origin's whole storage directory.
  Every `put` recomputed the origin's quota by opening each `.sqlite` file
  beside the current one, asking it for `page_count` and closing it again --
  a directory scan and a fresh SQLite connection per record written, inside
  the write transaction. A page that stores a burst of records stalled the
  browser in the filesystem for as long as the burst lasted: starting a game
  on chess.com left the main thread inside `CreateFile` and it never came
  back. Cache the siblings' total for five seconds; the current database is
  still measured live, so the limit is enforced as before.
* `document.styleSheets` includes the sheets a page links to, with their
  `href` and their rules. Only inline `<style>` blocks had a populated
  `CSSStyleSheet`; a `<link rel=stylesheet>` produced one with a null href
  and an empty `cssRules`, because the sheet was built from the element's
  own text content and a link has none. nrk.no went from 13 reachable rules
  to 1895. The engine keeps a reference to the CSS it already fetched for
  the cascade, so nothing is downloaded or stored twice.
* `getComputedStyle(el).cssFloat` reports the used float. The accessor read
  the declaration block directly rather than going through whichever
  `getPropertyValue` the object carries, so on a computed style -- which has
  its own -- it always came back as the empty string, while the equivalent
  `getPropertyValue('float')` answered correctly.
* Instantiating a module through the `WebAssembly` JS API runs the module's
  start section and nothing else. WAMR, built for a standalone runtime, also
  called `_initialize`, `__wasm_call_ctors` and `__post_instantiate` from
  inside `wasm_runtime_instantiate` -- but on the web those are ordinary
  exports the JS glue calls itself, after it has pointed its heap views at
  the instance's memory. Running them first meant an Emscripten module tore
  down on its own first WASI call: chess.com's analysis engine died in
  `environ_sizes_get` before `new WebAssembly.Instance` had returned.
* A finished keyframe animation keeps the value `animation-fill-mode`
  says it should. The engine sampled the last keyframe correctly, then threw
  the sample away: every getter the painter calls required the animation to
  still be running, so at the moment it ended the box snapped back to its
  specified value. The common `opacity: 0` plus a `fade ... forwards`
  animation therefore faded in and vanished again within one frame, and the
  content stayed invisible for the life of the page -- chess.com's bot
  gallery, which is exactly that pattern, was a set of empty boxes.
* `AbortSignal` is an interface object, not a bare namespace. It was a plain
  object carrying `abort()`, `timeout()` and `any()`, and the signals an
  `AbortController` hands out did not inherit from it, so `signal instanceof
  AbortSignal` -- the guard every fetch wrapper writes -- threw "invalid
  'instanceof' right operand" instead of answering. chess.com's RPC client
  turned that TypeError into a 500 and never issued its first request.
  `MessagePort` gains the same treatment in the window: it existed only in
  workers, so ports came back with no prototype at all.
* A grid container's max-content width is the sum of its columns, not the
  width of its widest item. Anything that shrink-wraps a grid -- a float, a
  table cell, an inline-grid, `width: max-content` -- was sized as if the
  columns were stacked, so the tracks overflowed the box they were given.
  bbc.com's "LIVE" flag is a floated two-column grid, and the headline
  beside it started inside the flag rather than after it.
* Flex and grid items measure their intrinsic sizes in the font they will
  actually be drawn in. The base size of a flex item and the min-content
  floor of a flex or grid item were measured against the item's own style
  rather than the style its text inherits, so text in a web font was sized
  by the fallback face. An item then got a base size a pixel or two under
  what the real font needs and wrapped mid-phrase however much room the
  container had -- dn.no's nav pills broke "DN Helg" and "DN i VM" across
  two lines inside a box wide enough for either.
* An absolutely positioned box with `width: auto` gets the shrink-to-fit
  width CSS 2.1 asks for -- its max-content size clamped to the available
  space and floored at min-content -- measured by shaping the text. It used
  to be guessed from a character count at 0.65em each, so every tooltip,
  dropdown, badge and popover came out at a width unrelated to its
  contents: a seven-character label was sized 81px where the text needs 63.
* Event-listener objects follow the Web IDL callback-interface algorithm.
  `handleEvent` is looked up for every dispatch, non-callable values and
  throwing getters are reported as uncaught listener exceptions, and generic
  `EventTarget` objects no longer discard object listeners. The focused DOM
  event test moves from three passing subtests out of six to all six.
* Checkbox and radio activation keeps the state required by HTML's legacy
  pre-activation and canceled-activation steps. `indeterminate` is a real
  cloned input state, a canceled radio click restores the previously checked
  group member, synthetic `click()` events are untrusted and cannot recurse on
  the same element, and the resulting `input` and `change` events are not
  cancelable. Three focused input tests move from 52/80 to 80/80 subtests.
* `CSSStyleSheet.insertRule()` and `deleteRule()` enforce their required
  arguments, while the deprecated but web-visible `addRule()` and
  `removeRule()` methods mutate both constructed and document sheets. The
  CSSStyleSheet interface test moves from 8/17 to 17/17 subtests.
* A worker scope gets the same JavaScript platform the page does.
  Workers were built from a hand-kept list of C-side globals and never
  ran the polyfill bundle, so `indexedDB`, `Headers`, `Blob`,
  `AbortController`, `AbortSignal`, `ReadableStream`, `WritableStream`,
  `TransformStream` and `caches` were all absent inside one. The bundle
  now runs in the worker too, up to the end of the IndexedDB section and
  no further -- everything past that point is DOM and window surface a
  worker must not have. Of the 27 globals a worker is expected to carry,
  14 were missing and 3 are: `FileReader`, `WebSocket` and nested
  `Worker`. chess.com's play page opened its database inside a worker
  and threw on the first line.
* `min-content` and `max-content` grid tracks size to their content
  instead of stretching like `auto`. A box with a definite width now
  contributes that width to min-content, an intrinsic track is measured
  against the space left once `minmax()` tracks are at their minimums
  rather than their maximums, a `min-content` track is never scaled below
  its content, and a grid container's own min-content is the sum of its
  columns rather than its widest child. Together these stop a nested grid
  -- chess.com's board -- from overflowing the panel beside it.
* An SVG with a `viewBox` but no width or height is sized the way CSS
  says: its ratio fitted inside the 300x150 default object size. It was
  rasterised into a square, so the artwork was letterboxed and then
  stretched into the page's box; Wikipedia's wordmark came out a smear.
* A declaration that uses `var()` no longer overrides the declarations
  that follow it. Such a declaration is held back until custom properties
  are known, and it was then re-inserted after every plain declaration in
  its block rather than at the place it was written, so it won every
  conflict with a later one. `color: var(--c); color: green` computed
  blue, `background: var(--c); background-color: red` computed blue, and
  `margin: var(--w); margin-left: 40px` kept the shorthand's margin. The
  New York Times sets `* { outline: var(--size) solid var(--accent);
  outline-color: #0000 }` -- an outline on every element that is
  transparent until something takes focus -- so the front page was drawn
  as a grid of blue boxes, one around every element on it. A held-back
  declaration now keeps its position in its block.
* A regexp search skips the positions that cannot start a match. A
  pattern without the sticky flag is compiled with a `.*?` prologue, so
  the matcher was re-entered at every index of the subject: `/^zebra/`
  walked all 880KB of a string to fail at the first assertion 880,000
  times. The search now reads what a match must begin with -- a start
  anchor, a single character, or a character class turned into a
  256-bit table -- and skips ahead with `memchr` or a table probe
  instead, the way V8's Irregexp does. A failing literal search over
  880KB drops from 5.6ms to 0.25ms, a leading character class from
  10.8ms to 0.35ms, a case-insensitive literal from 8.5ms to 0.85ms,
  and an anchored pattern from 6.1ms to nothing. Ten thousand
  `test()` calls that miss on a short string fall from 200ms to 2.2ms.
  A pattern that starts with an alternation is not covered and still
  runs as before. Verified by differential fuzzing: 40,000 random
  pattern/subject/flag combinations produce byte-identical `exec`,
  `replace`, `split` and `search` results before and after.
* The local `\p{RGI_Emoji}` tables are gone. Upstream quickjs-ng has
  since grown the general properties-of-strings machinery, which covers
  `RGI_Emoji` along with `Basic_Emoji` and the flag, tag, ZWJ, modifier
  and keycap sequences, and composes with the `v` flag's set operations
  -- so the vendored Emoji 17.0 sequence data, its generator and the
  local emit path were dropped for it.
* Grid items can be placed on named lines. A line named in the track
  list was parsed as a track, rejected, and dropped, so `grid-column:
  main` resolved to nothing and the item was auto placed, which collapses
  the layout of any page that names its lines. Names are now recorded and
  resolved, an area called `foo` also defines `foo-start` and `foo-end`,
  an end line repeating the start's name means the next line with that
  name, and an item with a column but no row keeps its column. chess.com's
  play page draws its board again.
* Headless `--viewport` takes `WIDTHxHEIGHT`. It read an integer and
  insisted the argument ended there, so `--viewport=1280x900` was dropped
  without a word and the page laid out at the default width. Anything it
  cannot read is now an error rather than silence.
* A page on an origin that does not speak QUIC no longer stalls for the
  whole connect timeout. Whenever libcurl was built with HTTP/3, every
  request asked for it, so the first hop to an origin that silently drops
  UDP on 443 waited out the 15-second navigation connect timeout (6 for a
  subresource) and returned a timeout rather than falling back. The
  timeout also counted as a connection failure, which parked the host in
  the unreachable cache for two minutes and failed every subsequent
  request to it -- so acid3.acidtests.org took 15 seconds to answer and
  then lost all of its subresources. Requests now ask for HTTP/2 and are
  upgraded to HTTP/3 by the alt-svc cache, the way an origin advertises
  it; `NS_FORCE_HTTP3=1` still asks for HTTP/3 outright. Acid3 loads in
  2.2 seconds instead of 15.5, and three seconds in it has run 65 of its
  tests rather than 12.
* Table cells centre their content vertically again. A cell with no
  `vertical-align` of its own fell back to the initial `baseline`, so in
  a row taller than the cell's own line the text sat at the top. Every
  browser's user-agent sheet gives cells `middle`, which is what pages
  written as tables expect. Cells now default to `middle`, and an author
  rule or a `valign` attribute still overrides it.
* An image is drawn inside its own borders, padding and margin. The
  painter placed the bitmap at the box's margin-box origin and gave it
  the content size, so a bordered image covered its own top and left
  borders and a margin shifted the picture instead of the box. Replaced
  content now starts where the content box starts, the way inline SVG
  and MathML already did, and the placeholder, alt text and drop shadow
  follow it.
* `OfflineAudioContext` renders audio instead of silence. Every `create*`
  method returned the same generic node, `connect()` recorded no edge,
  `start()` and `stop()` did nothing, `AudioBuffer` could not hold
  samples -- `getChannelData` minted a fresh zeroed array on every call --
  and `startRendering()` resolved a buffer of zeros. Nodes now carry their
  kind, the graph is recorded, buffers keep one array per channel, and
  `src/webaudio.c` renders oscillators, gain, a dynamics compressor, the
  RBJ biquad types, delay, wave shaping, constant sources and buffer
  playback. Rendering is mono, summed into every channel, and `AudioParam`
  automation is not applied. The context, nodes and buffers also brand
  themselves for `Object.prototype.toString`.
* The toolbar reads by colour again: back and forward green, reload blue,
  and a red stop button between reload and home that appears only while a
  page is loading. Stop marks the in-flight frame stale, ends the loading
  state and drops the busy cursor; it does not abort the network request.
  The title bar is shorter, the home button is set off from the address
  bar, and the security shield is drawn smaller than the buttons.
* `about:nordstjernen` lists the user agent, resolved the way a request
  resolves it.
* The start page is titled "Home" rather than "Nordstjernen", so its tab
  and window title say what the page is.
* The embedded ns-pango build no longer asks for link-time optimization.
  The rest of the tree links without LTO, so a clang build on Windows
  archived the fork as LLVM bitcode that the mingw linker could not read
  ("archive has no index" / "file format not recognized"). The subproject
  keeps `-O3` and the release `NDEBUG`.
* The CSSOM rule interfaces carry their real names and classes. Every
  interface the polyfill synthesises reported `name: "ctor"`, because the
  constructor was an anonymous function expression, and `@import` and
  `@keyframes` were plain `CSSRule` objects even though their `type` said
  3 and 7, so `instanceof` and `Object.prototype.toString` disagreed with
  `type`. They now get `CSSImportRule` and `CSSKeyframesRule`, `@namespace`
  and `@counter-style` get theirs, and the grouping at-rules that were all
  lumped under `CSSGroupingRule` get `CSSContainerRule`,
  `CSSLayerBlockRule` and `CSSScopeRule`.

Java
----
* The URL bar warned "Not encrypted" on every HTTPS page. Both the Java and
  the Android shell numbered the engine's transport-security states
  themselves and got them backwards -- 1 is a validated chain, not plain
  HTTP -- so a valid certificate showed the warning and an untrusted one
  showed the lock. They now follow `ns_security` and tell an untrusted
  certificate apart from an unencrypted connection.
* `java/pom.xml` builds the library with Maven: the same sources, the same
  manifest, and the jar, sources and javadoc artifacts, plus the metadata a
  public repository needs and a `release` profile that signs them and
  publishes to the Maven Central portal. `build.gradle` grows the matching
  `maven-publish` block, so `gradle publishToMavenLocal` and `mvn install`
  produce interchangeable artifacts, and it reads its version back out of
  `pom.xml` so the number lives in one place.
* `RemoteBrowser` spoke about a third of the renderer's control protocol. It
  now covers transport security and the server address, the live page size
  the render headers carry, the scroll position a page asks for (anchors,
  `scrollTo`, focus), camera prompts, window actions, overflow scrolling,
  in-page scrollbar dragging, `contextmenu` delivery, file drops, page dumps,
  the focused editable, idle ticks, caret blinking, and back/forward-cache
  traversal. `RemotePage` grows the `text`, `links`, `linkAt`, `dump`, `eval`
  and `renderToFile` it always claimed to mirror from `Page`, and `links`
  becomes a `/dump` kind so it has an endpoint to reach.
* Both clients had their own copy of a JSON reader that found a key anywhere
  in the document, including inside a string value. They now share one that
  only matches a quoted token immediately followed by a colon. Frames convert
  the renderer's BGRA to the raster's ARGB in one bulk copy rather than a
  per-pixel loop.
* The Swing browser uses all of it: a lock or warning beside the URL, wheel
  notches offered to the scroller under the pointer before the page,
  draggable in-page scrollbars, pages that draw their own context menu,
  camera prompts, page-source and layout/network/performance inspectors,
  History and Settings entries, files dropped onto the page, a blinking
  caret, and several windows (`Ctrl+N`, or `Ctrl+Shift+N` for a private one
  whose renderer keeps no cookies, cache or history) each with its own
  renderer process.

Android
-------
* The app had a back stack but no forward, no find-in-page, and no way to
  select page text -- and it silently dropped the WebGL and camera
  permission requests the engine raised, so a page asking for either got
  neither a prompt nor an answer. The JNI bridge now carries find, selection,
  favicons, transport security, both permission prompts and their
  resolutions, media resolution, PDF export, `contextmenu` delivery, `eval`,
  the scroll position a page asks for, and viewport changes; it also stops
  leaking the camera, audio and window-action strings on every rendered
  frame.
* The shell grows a forward button and a real history list that survives
  process death, a find bar, long-press-to-select with a copy/share action
  bar, a lock or warning at the head of the URL bar, and back/forward that
  reuses the renderer's back/forward cache instead of refetching.
* Rotation no longer refetches the page: the open document is re-laid out at
  the new viewport, so scripts, form state and the reading position survive
  turning the device.
* Platform integration: sharing a page or a selection, pull-to-refresh,
  printing through the system print service (Android's Save as PDF), pinning
  a page to the launcher, static app shortcuts, handling shared text,
  `WEB_SEARCH` and `PROCESS_TEXT` intents, and handing media the engine
  cannot play inline to another app.

CSS
---
* `prefers-color-scheme` and `prefers-reduced-motion` were hardcoded to
  `light` and `no-preference` with no way to change them.
  `ns_browser_set_color_scheme` and `ns_browser_set_reduced_motion` let an
  embedder mirror the platform's theme and animation settings into the
  cascade; the Android shell follows the system dark theme and the "remove
  animations" accessibility switch through them.

Text layout
-----------
* Desktop builds shape text through ns-pango, a fork of Pango carried as a
  meson subproject, instead of the system Pango. Pango keeps no cache that
  outlives a `PangoLayout`, so the same bytes were shaped by HarfBuzz once to
  measure an inline run and again to paint it, and a table cell was shaped
  for `min-content`, for `max-content` and once more to lay out. The fork
  caches finished glyph strings process-wide -- keyed on the font, bidi
  level, gravity, script, language, analysis and show flags, text transform,
  OpenType features and the item bytes -- and caches
  `pango_context_get_metrics` per font description, which resolving
  `line-height: normal` asks for on every inline run. On a table-heavy page
  the cache serves 92% of shaping requests and cuts layout time 24%; a
  text-heavy page falls 13%. Every symbol in the fork is renamed, because
  GTK loads the system Pango into the same process and GObject aborts when
  two libraries register the same type name. Android and iOS keep the system
  Pango, which has the backends they need, and `src/ns_pango_names.h` maps
  the renamed API back for them. Rendering is unchanged: the fixture smoke
  set matches its baselines and a corpus covering RTL and bidi, CJK, the
  white-space modes, intrinsic sizing, spacing, tabs, ellipsis, columns,
  inline atomics, decorations and font features renders byte-identically on
  both paths.

1.0.21:
======

Media and images
* The vendored pl_mpeg no longer reads past a frame plane. Half-pel motion
  compensation samples `s[si + 1]`, `s[si + dw]` and `s[si + dw + 1]`, but
  `plm_video_process_macroblock` bounds only `s[si]`, so a macroblock on
  the bottom row reads up to one row plus one byte beyond the plane it
  samples -- absorbed by the next plane for interior planes, and off the
  end of the allocation for the last one. This is reachable from page
  content: `ns_video_player_new` hands an MPEG-1 `<video>` body to
  `plm_create_with_memory`, and `ns_video_backend_next` decodes it. Fuzzing
  the decoder under AddressSanitizer with mutated streams reported it as a
  heap-buffer overflow read. The three frames are allocated as one chunk,
  which is now padded by that overshoot and zeroed, so the read stays
  inside the allocation and a corrupt stream decodes deterministically.
  Valid video is unaffected: no bound is tightened, so no macroblock that
  decoded before is rejected now.
* Animated images decode as animations on the engine's own fetch path.
  `ns_image_decode_body` routed GIF, APNG and animated WebP to the
  animation decoder, but the two fetch handlers in `engine.c` -- the ones
  headless rendering and the browser's own image pass use -- called
  `ns_image_decode_bytes` instead, which only ever returns a still frame.
  An animated image fetched through those paths therefore froze on frame
  one. Both now go through `ns_image_cache_insert_encoded`, and the
  still-versus-animated decision lives in one function rather than three
  copies that had already drifted.

Media capture and WebRTC
* `MediaStream` and `MediaStreamTrack` are constructors, and the streams
  and tracks `getUserMedia` hands back are instances of them, so
  `stream instanceof MediaStream` holds and `new MediaStream([track])`
  works. They were plain object literals with the right methods, which
  passes a duck-typing check and fails everything else.
* `RTCSessionDescription` and `RTCIceCandidate` exist. Signalling code
  wraps the objects it receives before handing them to the peer
  connection, so their absence stopped a session at the first offer.
  `RTCIceCandidate` rejects an initialiser carrying neither `sdpMid` nor
  `sdpMLineIndex`, as the specification requires.
* `RTCRtpSender`, `RTCRtpReceiver` and `RTCRtpTransceiver` exist, with
  `getCapabilities` on the two that define it.
* `RTCPeerConnection` gains `addTrack`, `removeTrack`, `addTransceiver`,
  `getConfiguration`, `setConfiguration`, `restartIce` and the
  `generateCertificate` static, and `getSenders`, `getReceivers` and
  `getTransceivers` return what was added to the connection instead of
  always returning an empty array.
  This is API surface: there is still no ICE agent, no DTLS and no SRTP,
  so a connection never leaves the `new` state. What changes is that
  feature detection and object construction no longer throw partway
  through a page's setup code.

Adaptive streaming
* HLS and DASH play. `data/js/streaming.js` adopts any `<video>` or
  `<audio>` whose source is an `.m3u8` or `.mpd` -- by extension or by
  `type` -- parses the manifest, picks a rendition, and feeds segments
  through Media Source Extensions, which is how the sites that use these
  formats already expect to be served. HLS covers master and media
  playlists, `EXT-X-MAP` initialisation segments, `EXT-X-BYTERANGE`,
  separate `EXT-X-MEDIA` audio renditions, and live playlists, which are
  re-fetched on the target-duration cadence and merged by media sequence.
  DASH covers `SegmentTemplate` with either `SegmentTimeline` or a fixed
  segment duration, `$Number$`/`$Time$`/`$RepresentationID$`/`$Bandwidth$`
  substitution with `%0Nd` padding, `BaseURL`, and separate audio and
  video adaptation sets. Rendition choice prefers the highest bandwidth at
  1080p or below among the codecs the build can actually decode. The
  player keeps roughly thirty seconds buffered ahead of the playhead and,
  on `QuotaExceededError`, evicts everything more than ten seconds behind
  it and retries rather than giving up.
* `video/mp2t` is an accepted Media Source type. Browsers reject it
  because their Media Source pipelines take fragmented MP4 and WebM only,
  which is why HLS players written in JavaScript transmux MPEG-TS before
  appending. Appended bytes here go to libavformat, which demuxes
  transport streams natively, so the transmuxing step is wasted work and
  segments can be handed over as they arrive.

Media Source Extensions
* `navigator.mediaCapabilities.decodingInfo()` answered from a hardcoded
  substring list -- WebM, VP8, VP9, Opus and WAV -- so it reported
  `supported: false` for every MP4 and AAC configuration even though
  `canPlayType` reported `probably` for the same string. Adaptive players
  ask `decodingInfo` which rendition to fetch, so a browser that denies
  H.264 there selects nothing and never starts. Container and codec
  support now resolve through one shared table that `canPlayType`,
  `decodingInfo` and `MediaSource.isTypeSupported` all consult, and a
  configuration is supported only when every stream in it is -- audio and
  video both, not whichever one was inspected first.
* `MediaSource.isTypeSupported` is a native call rather than a
  round-trip through `document.createElement('video').canPlayType`, and
  answers are memoised. Adaptive players probe it hundreds of times while
  building the format ladder; each probe used to allocate an element.
  It also answers for the segmented containers only, as the specification
  requires, instead of inheriting `canPlayType`'s whole-file container
  list.
* A single failed `appendBuffer` no longer wedges a `SourceBuffer` for
  the rest of the page's life. Any native rejection set a `_quotaFull`
  latch that made every later append throw `QuotaExceededError`, and the
  latch cleared only on a successful `remove()`. Quota is now checked
  against the buffer's real byte count before the append is queued, so it
  throws `QuotaExceededError` synchronously the way the specification
  says and the way players expect when they run their eviction path; a
  genuine decode failure runs the append-error steps instead, ending the
  media source. A zero-length append is a no-op rather than an error.
* `SourceBuffer` reports `audioTracks`, `videoTracks` and `textTracks`,
  and `AudioTrackList`, `VideoTrackList` and `TextTrackList` exist as
  constructors.
* `MediaSource.readyState`, `sourceBuffers` and `activeSourceBuffers`,
  and `SourceBuffer.updating`, moved from per-instance properties to
  prototype accessors, where feature detection looks for them.
* `MediaSourceHandle`, `MediaSource.prototype.handle`,
  `MediaSource.canConstructInDedicatedWorker`, `ManagedMediaSource` and
  `ManagedSourceBuffer` are present.
* AC-3, E-AC-3, FLAC, ALAC, MP3-in-MP4 (`mp4a.69`, `mp4a.6b`) and the
  AAC object types beyond `mp4a.40` resolve to their decoders, and
  `video/quicktime`, `audio/aac`, `audio/flac` and `audio/wav` are
  recognised containers.
* The headless renderer builds a video cache, wires the Media Source
  callbacks, ticks the cache from the settle loop and forwards media
  events back to the document, so appended segments reach the demuxer
  there instead of every `appendBuffer` failing for want of a callback,
  and `video.buffered` reports the real demuxed range. Media Source
  behaviour is now reproducible from `--headless`.

Images and graphics
* Animated PNG plays. Wuffs already decoded APNG frames and the
  animation loop is format-agnostic, but the callers only routed GIF
  magic to it and the animation decoder itself hardcoded the GIF
  signature check and the GIF decoder, so a PNG was rejected inside the
  function meant to decode it. Both now use the same format detection
  the still path uses. An APNG is recognised as the spec defines it, by
  an `acTL` chunk before the first `IDAT`, so a still PNG never pays for
  the animation decoder. The decode-pipeline documentation had listed
  APNG as supported already; it is now accurate.
* The vendored Wuffs moves to v0.4.0-alpha.10, nine months newer than
  the alpha.9 the tree carried. The release adds the VP8 decoder, so
  lossy WebP -- the common case on the web -- now decodes through the
  memory-safe path instead of libwebp. `MODULE__VP8` was already set in
  the subproject's build flags, where it had been a no-op because the
  module did not exist in alpha.9. libwebp and libwebpdemux are left
  serving animated WebP and nothing else.
* gdk-pixbuf no longer decodes page images. Every format the web
  actually uses is already handled in-tree -- ICO, then Wuffs for PNG,
  GIF, BMP and JPEG, then libwebp, then libavif, then the in-engine SVG
  renderer -- so the pixbuf fallback had been reduced to TIFF, TGA, PPM
  and ICNS, none of which Chrome or Firefox render either. What it cost
  was the ability to know what parses untrusted bytes:
  `gdk_pixbuf_get_formats` enumerates loader plugins installed on the
  user's machine, so the set of decoders reachable from a web page was
  decided at runtime, varied per system, and could not be audited from
  the build. The decode chain now ends after SVG: an unsupported format
  fails to decode instead of falling through to a plugin. GTK 4 still
  depends on gdk-pixbuf for its icon theme, so a desktop build links it
  either way -- what goes away is the browser feeding it. The mobile
  builds, which never had it, are unaffected.
  `ns_image_pixbuf_supports_mime` is renamed `ns_image_supports_mime`.
* libavif is optional on the desktop builds too. It was a hard
  `dependency()` off the mobile path, so a desktop tree without it would
  not configure at all, even though every AVIF call site already sat
  behind `NS_HAVE_AVIF` and `image_avif.c` was already compiled
  conditionally. The new `avif` meson feature defaults to `auto`, so a
  host that has libavif is unchanged; `-Davif=disabled` drops it and
  AVIF images fail to decode like any other unsupported format. libavif
  pulls in a complete AV1 decoder for a format that is rare on the web.
  The README listed libavif as both required and optional; it is now
  listed once, as optional.
* `var()` resolves inside SVG presentation attributes. A custom property
  set by a stylesheet rule now reaches `r="var(--radii)"` or
  `fill="var(--tint)"`, so a class can retheme an inline icon's colour
  and geometry the way it does for ordinary CSS properties.
* `mask` is honoured on SVG elements. The referenced `<mask>` renders to
  an offscreen surface whose sRGB luminance becomes the alpha the
  element is composited through, so a white mask shows the element,
  black hides it, and a gradient fades it. Group opacity and masking
  combine.
* `marker-start`, `marker-mid` and `marker-end` draw their `<marker>` on
  path, line, polyline and polygon vertices. Vertices and their tangents
  come from the built Cairo path, so arcs and curves orient the same way
  straight segments do, and a mid vertex uses the bisector of its two
  tangents. `markerUnits="strokeWidth"` scales the marker with the
  stroke, `orient="auto"` and `auto-start-reverse` rotate it, and
  `refX`/`refY` are mapped through the marker's own `viewBox` before
  positioning.
* `vector-effect: non-scaling-stroke` keeps a stroke's width in device
  space instead of scaling it with the current transform.
* SVG is rendered by the engine instead of librsvg. `src/svg.c` walks
  the SVG DOM and paints it through the same Cairo surface, cascade and
  font stack that HTML uses, and `librsvg` is gone from the dependency
  list, the packaging manifests and the CI images. Inline `<svg>` was
  previously re-serialised to XML and handed to librsvg as an opaque
  raster, so the document's own stylesheet could never reach inside it:
  `fill: currentColor`, `svg .icon { fill: … }` and script-driven
  geometry changes were invisible. SVG elements now take part in the
  normal cascade, so `fill`, `stroke`, `stroke-width`,
  `stroke-dasharray`, `fill-rule`, `stop-color`, `text-anchor`,
  `paint-order` and the SVG geometry properties `x`, `y`, `cx`, `cy`,
  `r`, `rx`, `ry` are real CSS properties that inherit like the rest.
  Covered: paths including elliptical arcs and smooth-curve
  continuation, rect/circle/ellipse/line/polyline/polygon, `viewBox`
  and `preserveAspectRatio`, nested `<svg>`, `<g>`, `<use>`,
  `<symbol>`, `<switch>`, `<defs>`, linear and radial gradients with
  `href` inheritance, `spreadMethod`, `gradientUnits` and
  `gradientTransform`, `clipPath`, group opacity, dashing, and `<text>`
  shaped through Pango. A standalone `.svg` document sizes to the
  viewport rather than to a 300x150 default. Android and iOS, which
  dropped librsvg with the rest of the desktop stack, gain SVG for the
  first time.

CSS
* `text-decoration-color` reaches the painted line. The colour was only
  read from the *block's* style, never from the inline run that carries
  the decoration, so an underline set on an `<a>` was always drawn in
  the text colour -- and a fully transparent one, the idiom behind every
  "underline grows in on hover" teaser, was drawn as a solid line. On
  Tidens Krav and the other Amedia fronts that put an underline under
  every headline and nav link. The decoration attributes now carry the
  style of the element that turned them on, so
  `text-decoration-color: #e00` paints red, and a decoration whose
  resolved colour is fully transparent is not emitted at all. A
  decoration propagated from an ancestor still paints in the ancestor's
  colour, as the spec requires.
* A flex item that is itself a flex or grid container re-aligns its own
  children after the cross-axis stretch resizes it. The item laid its
  children out at its content height, and the stretch then overwrote
  that height in place without a second pass, so anything the item
  centred or bottom-aligned stayed where the pre-stretch height had put
  it. VG's masthead is the shape that shows it: a 56px-tall `header`
  flex row, a logo link inside it that is a flex container with
  `align-items: center`, and a 24px logo that rendered flush against the
  top of the bar instead of centred on it. The column-flex path already
  re-ran layout for a resized item; the row and wrapped-row paths now do
  the same, and only when the stretch actually changed the height.
* A `container-type: inline-size` (or `size`) element no longer sizes
  itself from its own contents. CSS Contain 3 gives such an element
  inline-size containment, so its intrinsic inline sizes are computed as
  if it had no children; the engine measured the children anyway. On a
  page whose container queries feed back into the container -- headlines
  sized in `cqw`, the pattern VG, Aftenposten and the other Schibsted
  fronts use -- that closed a loop: wide contents made the container
  measure wide, `cqw` then resolved against the inflated width and made
  the contents wider still. VG's lead teaser laid out 1245px wide inside
  a 734px column and its headline computed to 276px where the site asks
  for 157px. The three intrinsic-width paths (`measure_natural_width`,
  `measure_min_width`, `estimate_natural_width`) now return zero content
  contribution for such a box, so an explicit `width` still wins and a
  flex or grid item shrinks to the space its parent gives it.
* A square border is painted inside its border box rather than centred
  on the edge. Each side was stroked along the border-box boundary with
  the line width set to the border width, and Cairo centres a stroke on
  its path, so every bordered element rendered half a border wider than
  it laid out on each side -- a 4px border occupied 6..9 and 60..63
  where the box model puts it at 8..11 and 58..61. Layout was always
  right; only the paint was wrong, so borders overlapped whatever sat
  next to them. Rounded borders and border-image already inset
  correctly and are unchanged.
* An `<iframe>` becomes visible as soon as its document loads, on a
  quiet page as well as a busy one. The UA sheet hides frames until the
  engine stamps `data-nd-frame-loaded` on them, but that stamp is
  written by the loader rather than through the scripted attribute
  path, so it never invalidated style. The frame kept the cached
  `display: none` and produced no box at all — its document parsed and
  its scripts ran, entirely unpainted — until some unrelated mutation
  happened to force a restyle. Pages with continuous script activity
  masked it; a page whose only content was a frame never showed it. The
  three places that add or remove the attribute now mark it dirty.
* Inline atomic boxes contribute their full height to the individual
  wrapped line that contains them. Multi-line form controls and table
  cells now reserve the correct vertical space instead of allowing later
  lines to overlap following content, fixing the Google footer position.
* Inherited properties set on the root element reach the rest of the
  page. The UA stylesheet declared `color`, `font-family`, `font-size`
  and `line-height` on `html, body` together, and a UA declaration on
  `body` outranks inheritance from `html` — so a page styling only
  `html` (`html{font-family:"Helvetica Neue","Segoe UI",Arial,
  sans-serif}` on lite.duckduckgo.com) had its font, colour and size
  dropped at `body` and rendered in the UA serif default. The
  declarations now sit on `html` alone and `body` inherits them.
* A concrete font family is used when the system actually has it.
  `Arial`, `Helvetica`, `Segoe UI`, `Roboto` and the SF Pro names were
  rewritten to generic `sans-serif` unconditionally, which resolved
  through fontconfig to whatever the default sans happened to be —
  Noto Sans rather than the requested Segoe UI or Arial. Each name is
  now resolved against the installed families first, and substituted
  by `sans-serif` only when it is missing.
* `calc()` serializes per CSS Values 4 instead of being echoed back as
  authored. A typed math sum — one coefficient per unit, sitting beside
  the px/pct/em/rem value layout uses — sums terms of the same type,
  folds absolute lengths, angles, times, frequencies and resolutions to
  their canonical unit, and distributes products and quotients by a
  number. A sum that cannot reduce to a single term serializes sorted:
  number, then percentage, then dimensions in ASCII-alphabetical unit
  order. `calc(1px + 1%)` is `calc(1% + 1px)`,
  `calc(1px + 2em + 3rem + 4%)` is `calc(4% + 2em + 1px + 3rem)`,
  `calc(2 * (1px + 1em))` is `calc(2em + 2px)`, and a single-argument
  `min()`/`max()` reduces to `calc()`. The quad shorthands serialize the
  same sum rather than dropping every term but px, so
  `margin: calc(1px + 1em) 2px` no longer reads back as `1px 2px`.
  Comparisons that need layout, such as `min(20px, 10%)`, still stay as
  authored.
* `border-image` is implemented (CSS Backgrounds 3): the five longhands
  (`border-image-source`/`-slice`/`-width`/`-outset`/`-repeat`), the
  `border-image` shorthand and its `-webkit-` alias parse, cascade,
  serialize canonically and reach `getComputedStyle`; the `border`
  shorthand resets them. The painter nine-slices the source — raster
  `url()` images and gradients alike — honouring `fill`, percentage and
  number slices, `auto`/length/percentage/number widths, outsets and all
  four `stretch`/`repeat`/`round`/`space` tiling modes, and replaces the
  element's border style while it renders.
* Media Queries Level 4: the heuristic matcher is replaced by a real
  evaluation engine (`src/css_media.c`) with grammar-complete parsing
  (range syntax, boolean context, nested conditions, `and`/`or`/`not`,
  general-enclosed), Kleene three-valued logic and CSSOM media-list
  serialization (`not all` for unparseable queries). Iframe documents
  evaluate against their own viewport, `matchMedia` and
  `CSSMediaRule.media` expose the serialized form, `CSSMediaRule.media`
  is a real `MediaList`, and changing a `media` attribute restyles.
* The native `sheet` / `document.styleSheets` stubs that shadowed the
  real CSSOM are gone. `style.sheet.cssRules` now returns the parsed
  rule tree and `style.sheet === document.styleSheets[i]` holds.
* A script read of a resolved value flushes every pending mutation, not
  just the first one in the task. `getComputedStyle`,
  `getBoundingClientRect`, `offsetWidth`, `scrollIntoView` and the rest
  force a synchronous reflow whenever the document is dirty; the
  wall-clock interval and the oscillation dampener now apply only to the
  rendering tick, where they belong. Mutating a style and reading it
  back in the same task returns the new value, and `:has()`
  invalidation, inset resolution and stylesheet insertion are observable
  immediately.
  Two gaps this makes visible, which the frozen styles had been hiding:
  `scrollWidth`/`scrollHeight` are wrong on inline-level boxes
  (`inline-block`, `inline-flex`, `inline-grid`), and `attr()` is
  substituted when generated content is rendered but not when
  `getComputedStyle().content` is serialized.
* Cascade layers are ordered as a tree rather than by first-declaration
  order across the whole document: sublayers sort inside their parent,
  a layer's own declarations act as its implicit final sublayer, and
  nested anonymous layers stay nested instead of escaping to the top
  level.
* The incremental restyle pass identifies stylesheets by a parse-time
  serial instead of by address. A reparsed `<style>` reusing the freed
  block of the sheet it replaced used to look unchanged, which froze the
  page at stale styles.
* `@scope` preludes are parsed against the grammar and invalid ones drop
  the rule; the prelude is serialized canonically.
* `StyleSheet.media` is a live `MediaList` that writes back to the
  owner node's `media` attribute, and `ShadowRoot.styleSheets` is empty
  for a disconnected tree.
* CSS Display Level 3: `display` is a structured computed value — outer
  type, inner type, list-item flag and layout-internal kind — resolved
  once in the cascade instead of a keyword string that layout, paint and
  the CSSOM each re-read with `strcmp`. Multi-word canonical forms now
  reach layout, so `display: flow-root list-item` keeps its box instead
  of losing it; blockification of floated and absolutely positioned
  boxes has one implementation rather than three; `-webkit-box` and
  `-webkit-inline-box` map to flex and inline-flex.
* Anonymous table boxes are generated around any run of table-internal
  siblings, per CSS 2.1 17.2.1, so `display: table-row` and
  `display: table-row-group` outside a table lay out as tables instead
  of collapsing into the surrounding inline content.
* CSSOM: declaration blocks are canonicalized, rule mutations apply
  synchronously, constructed stylesheets are backed by live rules,
  at-rules are exposed on declarations, and shorthand serialization
  covers the quad shorthands, `all` and the complete shorthand
  families.
* `getComputedStyle` resolved values: insets absolutize against the
  correct containing block, static insets follow the writing mode,
  automatic minimum sizes resolve, and pseudo-element styles compute.
* Declaration grammar is enforced: values with unbalanced brackets or
  quotes, stray `!` or `;`, out-of-place `auto`/`normal`, negative
  values on non-negative properties, or transform functions with bad
  units are dropped instead of being half-parsed.
* `content-visibility: hidden` applies size containment, and
  `writing-mode: vertical-rl/lr` with `text-orientation`
  `upright`/`mixed`/`sideways` measures and paints vertical inline runs.
* Flexbox honours the automatic (content-based) minimum size, and
  column stretch no longer double-counts item margins.
* Media queries inside a frame evaluate against the frame's own size,
  not a 300x150 guess. The viewport pushed while collecting a frame's
  stylesheets came from the frame's inline `style` attribute or its
  `width`/`height` content attributes, so a frame sized by a stylesheet
  rule -- `iframe { width: 100% }`, the common responsive-embed pattern --
  was measured as 300x150 and its `@media (min-width: ...)` blocks
  resolved against a size the frame never had. Layout now records each
  frame's content box, collection prefers it over the default, and when
  the recorded size disagrees with the one a viewport-dependent frame
  sheet was collected under, style and layout run once more so the frame
  settles on its real size. Frames whose CSS carries no width, height,
  aspect-ratio or orientation query never trigger the extra pass. Acid3
  goes from 98/100 to 99/100.
* A frame document's own stylesheet can style its root element. Sheets
  inside an iframe are rewritten to be scoped to the frame's root, and
  every selector whose subject was not literally `html` or `:root` got a
  descendant combinator — so `* { … }` or `.cls { … }` in a framed
  document matched everything inside the frame except the frame's own
  `<html>`, and `getComputedStyle` on that element reported no value for
  any property. The scope marker now also attaches directly to the
  subject compound, and lands before a pseudo-element rather than after
  it. Shadow scopes are unchanged: a shadow host is still not styled by
  its own shadow tree. Acid3 goes from 97/100 to 98/100.
* `getComputedStyle(el).someUnknownName` is `undefined` rather than the
  empty string. The proxy in front of a computed declaration answered
  every string key through `getPropertyValue`; its `has` trap already
  distinguished supported properties from unknown ones, and `get` now
  draws the same line. jQuery's `css()` returns
  `computed.getPropertyValue(name) || computed[name]` and expects
  `undefined` for a property the engine does not know.

Scripting
* Removed the IE-only `attachEvent` and `detachEvent`. They were exposed
  on Element, Document and Window as no-op stubs that returned true and
  registered nothing. Libraries still feature-detect them to select a
  legacy path: RequireJS, finding a native-looking `attachEvent`, bound
  its script-load callback to `onreadystatechange` instead of
  `addEventListener`, the stub swallowed it, and every module load ended
  in "Load timeout for modules". jQuery's test suite could not get past
  its RequireJS bootstrap before this.
* `DOMParser` reports the line and column of an XML parse error. The
  synthesized `parsererror` document carried the bare text "XML parsing
  error"; it now names the position the parser stopped at.

Scripting
* `Intl.DateTimeFormat` names months and weekdays in the requested
  language, and picks the clock the locale actually uses. The month and
  weekday tables held English names only and the hour cycle defaulted to
  12-hour whatever the locale, so `new Date().toLocaleTimeString()` on a
  Norwegian desktop read "2:47:00 PM" instead of "14:47:00", and
  `toLocaleDateString('nb-NO', {weekday: 'long', month: 'long'})` read
  "Tuesday, July 28" instead of "tirsdag 28. juli". Every Nordic news
  front page shows a formatted date, so this was visible on all of them
  -- Aftonbladet's masthead read "TUESDAY, JULY 28, 2026". Month and
  weekday names are now carried for the fourteen languages whose date
  *patterns* the formatter already knew (the Nordic five plus German,
  Dutch, French, Spanish, Italian, Portuguese, Polish and Russian);
  `short` and `narrow` are derived from the long name by UTF-8-safe
  truncation rather than byte truncation, which previously cut a
  multi-byte name mid-character. The 12-hour default is now restricted
  to the locales that use one, `en` (outside GB/IE/ZA) and a dozen
  others; everything else formats h23. Swedish and Lithuanian numeric
  dates serialize in ISO order (`2026-07-28`), and the day-month-year
  languages get their own literals -- the ordinal period in Norwegian,
  Danish, German, Finnish and Icelandic, ` de ` in Spanish and
  Portuguese -- instead of the English comma layout. An explicit
  `hour12`/`hourCycle` option still wins, and `en-US` output is
  unchanged.

Networking
* A top-level navigation follows a redirect that leaves HTTPS. Any
  redirect off `https://` was refused outright, which is the right rule
  for a subresource -- that is mixed content -- but not for a document
  the user asked for. Sunnmørsposten is the common shape: `smp.no`
  answers 302 to `http://www.smp.no/`, whose server immediately sends
  302 back to `https://www.smp.no/`, so the whole site was unreachable
  and rendered as "That address looks malformed". Navigations now follow
  the hop and report the resulting scheme in the security indicator;
  subresource fetches are still blocked exactly as before.
* HTTP/3 can receive a response larger than a megabyte. nghttp3's
  `nghttp3_conn_read_stream()` returns the bytes it consumed *excluding* the
  DATA frame payload — the application is required to extend QUIC's stream
  and connection flow-control credit for the body itself, from the
  `recv_data` callback. The backend extended only for what nghttp3 reported,
  so credit for body bytes was never returned: every HTTP/3 transfer
  deadlocked the moment it reached the 1 MB
  `initial_max_stream_data_bidi_local` advertised at connection setup, and
  sat there until the request timeout expired 30 seconds later. A 10.7 MB
  script that HTTP/2 fetched in 140 ms failed outright over HTTP/3; it now
  completes.
* HTTP/3 resolves the origin over IPv6 as well as IPv4. The QUIC socket
  asked `getaddrinfo` for `AF_INET` only and used the first result, so on an
  IPv6-only network every HTTP/3 hop failed to connect and fell back to
  HTTP/2. It now asks for `AF_UNSPEC` and tries each address in turn, the
  way the TCP path does.
* A UDP socket that reports `EAGAIN` no longer fails the HTTP/3 request. The
  QUIC socket is non-blocking, so a full send buffer is an ordinary
  condition under load; it was treated as a fatal write error and abandoned
  the connection. The datagram is now dropped and left to QUIC loss
  recovery, which is what it is for.
* HTTP/3 loss-recovery and idle timers fire while packets are arriving.
  `ngtcp2_conn_handle_expiry()` was called only when the poll timed out, so
  a connection with steady inbound traffic never processed an expired PTO or
  ACK timer. It is now called every iteration, which is a no-op when nothing
  is due.
* The nghttp2 backend reads the final response of a request that begins
  with an informational one. A `103 Early Hints` — what Cloudflare, Fastly
  and Shopify send ahead of the real response — arrives as a first HEADERS
  block, and libnghttp2 categorises the *final* HEADERS that follows as
  `NGHTTP2_HCAT_HEADERS` rather than `NGHTTP2_HCAT_RESPONSE`, the same
  category it gives trailers. The header callback accepted only
  `HCAT_RESPONSE`, so the page kept the interim status and every real
  header was discarded: no `Content-Type` (an HTML document rendered as
  plain text, its source visible), no `Content-Encoding` (a gzip body
  handed to the sink still compressed), no `Set-Cookie`. The callback now
  admits the block that follows an informational response and drops the
  interim headers instead of the final ones; trailers are still ignored.
  The HTTP/1.1 fallback had the same defect and worse — it treated the
  blank line ending the interim response as the end of all headers, so the
  entire second response, status line included, became the body. It now
  skips interim blocks and parses the response after them.
* A timed-out or cancelled HTTP/2 stream is detached from its session.
  `ns_h2_io_scan_timeouts` sent RST_STREAM and released the request, which
  lives on the requesting thread's stack, but left the session's
  `stream_user_data` pointing at it. DATA or HEADERS already in flight for
  that stream — the ordinary case, since RST_STREAM races a response — then
  drove the header and body callbacks through a dangling pointer and wrote
  into a returned stack frame. Streams are now detached with
  `nghttp2_session_set_stream_user_data()` before the request is released.
* HTTP/2 downloads are no longer capped by the default connection-level
  flow-control window. The session advertised an 8 MB
  `SETTINGS_INITIAL_WINDOW_SIZE`, but per RFC 9113 §6.9.2 that setting
  governs streams only: the connection window stayed at the protocol
  default of 65535 bytes, which throttles *aggregate* throughput on a
  connection to one window per round trip — about 640 KB/s at 100 ms RTT no
  matter how many streams are multiplexed over it. The connection window is
  now raised to match with `nghttp2_session_set_local_window_size()`.
* A request the server refused is retried on a fresh connection. When a
  pooled connection goes away, libnghttp2 closes the streams the server
  never processed with `NGHTTP2_REFUSED_STREAM` — the code exists precisely
  so the request can be sent again — but the retry only covered streams
  that had not been submitted, so a subresource lost this race and failed
  outright instead of being refetched.
* Idle HTTP/2 connections are closed. The pool defined an idle timeout, a
  reuse ceiling and a per-origin cap and enforced none of them: every
  origin visited kept a connection, its TLS state, three file descriptors
  and a live I/O thread polling four times a second until the browser
  exited. A connection with no streams for a minute now stops its I/O
  thread, and the pool drops connections that are dead, idle-expired, over
  the reuse ceiling or beyond the per-origin cap.
* The nghttp2 backend uses the `nghttp2_ssize` API on the versions that
  have it. Upstream deprecated the `ssize_t`-based entry points in favour
  of `…2` variants in 1.60.0 and lets an application compile the old ones
  out entirely with `NGHTTP2_NO_SSIZE_T`; the backend now defines that
  macro and calls `nghttp2_submit_request2()`,
  `nghttp2_session_mem_recv2()` and
  `nghttp2_session_callbacks_set_send_callback2()` when the headers are new
  enough, keeping the deprecated names only as the fallback for older
  libnghttp2. This is what a toolchain without `ssize_t` needs, and it
  makes a future upstream removal a non-event.
* An informational response no longer contributes headers to the HTTP/3
  response that follows it, matching the HTTP/2 and HTTP/1.1 paths.
* The request identity a fetch coalesces and preloads on no longer depends
  on the order its `Accept`-style headers happen to be listed in; the header
  lines are sorted into the key.
* `Vary: Origin` no longer defeats the HTTP cache. `Origin` is now one of
  the headers the cache can resolve at lookup time: `net.c` computes the
  value it will send once and uses that same string both as the request
  header and as the cache selector, so the two can never disagree, and
  the absence of an `Origin` selects distinctly from any present one.
  Google serves its stylesheets `public, immutable, max-age=31536000`
  with `Vary: Origin`; those were being refetched on every load and are
  now cached.
* ES module fetches join the same request identity as every other
  subresource. The module loader passed no top-level URL, so a module was
  partitioned in the HTTP cache under its own site rather than the
  document's — two unrelated sites importing the same module shared one
  cache entry — and it neither coalesced with nor consumed the preload
  issued for the same `<script type=module src>`, since that preload
  carries the JavaScript `Accept` and the module fetch did not. It now
  passes the document URL and the script `Accept`.
* Shutting down no longer hangs a caller waiting on a coalesced fetch.
  `ns_net_drain` discarded queued fetch tasks without telling the
  coalescer, so a task that led a group left the group behind: blocking
  joiners waited on a condition nobody would signal again, and
  asynchronous joiners never had their callback run. A blocking joiner
  now also gives up when the network layer starts aborting, and in any
  case five seconds past the longest transfer timeout a leader can have,
  instead of waiting without a bound. These waits happen on worker
  threads that teardown joins, so one that never returned took the
  joining thread down with it.
* The HTTP cache selects the right variant of a negotiated response.
  `cache.c` keyed entries on URL and partition and stored nothing about
  `Vary`, so a resource served `Vary: Accept` and referenced both as a
  stylesheet and as a script was fetched once and that single variant
  handed to both — a `<script>` element could receive CSS. Entries now
  carry the response's `Vary` and are keyed on a selector built from the
  request headers it names, with an indexed base key so a lookup can walk
  the variants stored for a URL and match the right one. `Accept`,
  `Accept-Language` and `User-Agent` are resolved; `Accept-Encoding` is
  ignored because bodies are stored decoded, which keeps the web's most
  common `Vary` from fragmenting the cache; anything else, including
  `Vary: *`, is not stored rather than stored wrongly. The preload scan
  deduplicates candidates on (URL, destination) instead of URL alone, so
  both variants are preloaded. The cache schema is versioned through
  `PRAGMA user_version` and an upgrade discards the old cache.
* The speculative preloader hands its bytes to the loader that needs
  them through a single deduplication point keyed on the request's
  identity. Preload responses used to be parked in a private store
  keyed on the bare URL and consulted ahead of the HTTP cache. That
  store ignored the cache partition, so within its 20-second window one
  site could be served bytes another site had fetched with that site's
  cookies; it ignored `no-store`; it recorded a placeholder for every
  fetch it started but only removed entries when a loader consumed one,
  so failed preloads and preloaded images — which nothing consumed —
  permanently occupied its 32 slots until the preloader silently
  stopped preloading anything. Deduplication now happens in one place.
  The in-flight coalescer keys on method, URL, cache partition and
  request headers rather than URL plus referrer, and every entry point
  joins it — `ns_net_request_async` and the blocking fetchers as well
  as `ns_net_fetch_async` — so a loader that arrives while a preload is
  still in flight waits for it instead of issuing a second request. A
  preload that finishes first is held in a preload map under that same
  key, handed over by the fetch layer itself so there is no window in
  which a resource is in neither place, and dropped when the next
  navigation begins. The preloader now sends the `Accept` header its
  consumer will send, so content-negotiated resources match. The
  separate external-script prefetcher, a third path over the same URLs,
  is gone. A page with six scripts and five stylesheets issues exactly
  one request per resource, counted at the origin.

Layout and rendering
* Box `x`/`y` uniformly means the margin-box origin, which fixes flex
  and grid items with margins rendering and measuring double-shifted;
  `getBoundingClientRect` derives the border box the same way the
  painter does. Grid row placement was still adding the item's top
  margin on top of that origin, so a negative margin moved the item the
  wrong way by twice the amount.
* Flex items are sized by the flex algorithm rather than by their own
  `width`. `layout_block` used to read `width` back out of the style and
  ignore the main size the container had assigned, so nothing ever
  shrank — `flex-shrink: 1` is the initial value, so every
  over-constrained flex row overflowed instead of fitting.
* The flex main axis is reversed when exactly one of
  `flex-direction: row-reverse` and `direction: rtl` applies, and items
  are then packed from the opposite edge. `row-reverse` used to reverse
  the item order but still pack against the left edge, and `rtl` was
  ignored for the main axis entirely.
* `scrollWidth`/`scrollHeight` measure the real scrollable overflow
  region from descendant border boxes, including overflow from
  negative margins on non-scrolling boxes.
* Escaping floats and compact line boxes lay out correctly, the
  non-rendering elements (`area`, `base`, `link`, `meta`, `param`,
  `source`, `track`, …) are excluded from inline layout, anonymous
  table cells lay out as blocks, and malformed legacy declarations are
  skipped rather than derailing the rest of the block.

HTML, DOM and JavaScript
* Core content-attribute reflection is complete, and the text-control
  selection APIs (`selectionStart`/`End`/`Direction`,
  `setSelectionRange`, `textLength`) match the spec including per-type
  applicability and the `IndexSizeError`/`InvalidStateError` cases.
* Mutation observers and shadow trees align with the spec: old-value
  records for attributes and character data, `attachShadow` options,
  `assignedSlot`, and `ShadowRoot.styleSheets`/`activeElement`/
  `elementFromPoint`.
* Frames are isolated per document: events, event handlers and the
  `Performance` objects belong to the frame's own realm, so scripts
  inside an iframe see their own `window` and `document`.
* Speedometer 3.1 fixes: nested custom-element upgrades during
  construction, unhandled rejections queued to the microtask
  checkpoint, module scripts in iframes seeing the frame realm,
  `ownerDocument` returning the realm wrapper, frame windows in the
  event propagation path, images in frames resolving relative URLs
  against the frame document, and the always-rejecting
  `navigator.wakeLock` stub removed so feature detection falls back
  cleanly.
* Promise rejection events: cancelable `unhandledrejection` carrying
  `promise`/`reason`, with `rejectionhandled` for rejections handled
  later.
* `XMLHttpRequest` gains the upload object, the full progress event
  set, `responseXML` and method/state validation; `MessageEvent` and
  `ExtendableMessageEvent` follow the messaging spec.
* `Navigator` and friends (`MimeTypeArray`, `PluginArray`,
  `NetworkInformation`, `StorageManager`, `UserActivation`,
  `NavigatorUAData`, `MediaCapabilities`, `MediaDevices`) are real
  interfaces with non-constructible prototypes, so brand and
  `instanceof` checks agree.
* Cookie Store API: promise-based `cookieStore.get`/`getAll`/`set`/
  `delete` over the document cookie jar.
* Legacy media and timing surfaces: the `HTMLMediaElement`/
  `MediaError` constants, `PerformanceTiming` and
  `PerformanceNavigation`; `blob:` URLs work as module script sources.
* `<input type=email>` validation follows the HTML email grammar, and
  Trusted Types plus void-element serialization are tightened.
* Reading an `<iframe>`'s `contentDocument` or `contentWindow` more than
  once no longer aborts the process. The realm document's API was
  installed onto the per-node wrapper every time it was requested, and
  the second install hit QuickJS's "property already exists" abort in
  `JS_DefineAutoInitProperty`. The install now runs once per wrapper, so
  repeat reads return the same document, as the DOM requires.

Identity and privacy
* `Sec-CH-UA` and `navigator.userAgentData` report `"Nordstjernen"`
  instead of impersonating Chromium and Google Chrome;
  `getHighEntropyValues` fills in `formFactors`, `platformVersion`,
  `architecture`, `bitness`, `wow64`, `model`, `uaFullVersion` and
  `fullVersionList`.
* New `--private` command-line flag starts the browser in private
  browsing mode.
* The Chrome compatibility token in the user agent moves to 150.

Media
* MSE segments are tracked with their byte offset and probed time
  range. `SourceBuffer.remove()` rebuilds the helper input from the
  initialization segment plus the retained segments, which keeps
  long-running adaptive playback (YouTube, Vimeo) inside its byte
  quota, and `SourceBuffer.buffered` reports the retained demux range
  instead of a synthetic zero-based one. `remove()` no longer refuses
  ranges that are not a clean prefix.
* `@font-face` sources are only fetched when their `unicode-range`
  covers a codepoint the page actually uses, so font-heavy pages stop
  competing with streaming playback for bandwidth.
* `<video>`/`<audio>` expose `seeking` and `played`, and fire
  `ratechange` and the seek events.

Layout
* A flex item with padding or a border is no longer sized smaller than
  its content. The automatic flex base size came from
  measure_natural_width(), which already reports a content width, and
  then subtracted the item's own padding and border a second time — the
  surrounding code tracks those separately. Items came out exactly one
  padding-and-border narrower than they should be, so buttons and labels
  in a flex row wrapped mid-phrase for no reason. A row of consent
  buttons that Chrome lays out as two single-line pills was wrapping to
  two lines each; it now matches. Items without padding were unaffected,
  which is why this survived so long.

Headless
* `--dump=png` no longer crashes on pages that keep scripting busy while
  media is fetched. The video prefetch held box pointers across a
  blocking fetch, and the nested main loop that fetch runs can relayout
  the page and free them underneath it. It now resolves the URLs, then
  re-finds the box before attaching, so no box outlives a fetch.

Performance
* A page using container units settles in two container passes instead
  of three. Container queries are resolved by iterating cascade and
  layout until the styles stop changing, up to three times. The loop
  compared the two style tables to decide, which meant the third pass
  always ran a full cascade before discovering it had nothing to do.
  It now compares the container geometry the cascade actually reads --
  and only the axis a query can observe, so the block size of a
  `container-type: inline-size` element, which no query and no `cqh`
  unit can ever see, no longer counts as a change. VG's front page is
  the shape this was costing: seventeen inline-size containers whose
  heights kept moving while their widths had already converged, so
  every relayout paid for three cascades and three layouts. A relayout
  there drops from ~1.6 s to ~0.6 s, and the page, which previously
  never finished rendering at all, now settles in 23 s.
* Nested flex rows no longer lay out in exponential time. A flex item
  that stretches to the line's cross size was laid out once against its
  natural height and then, because its own children had been aligned
  against the wrong height, laid out a second time. `align-items:
  stretch` is the default, so this doubled the work at every level of
  flex nesting: layout cost 2^depth. A synthetic page of nested
  stretched rows took 6.4 s at depth 14, 71 s at depth 16, and killed
  the renderer at depth 18. The stretched cross size is known before the
  item is laid out, so it is now handed down as the item's definite
  height on the first pass and the second pass is skipped. The same page
  takes 39 ms at depth 14, 89 ms at depth 16, and 1.3 s at depth 20.
  Real pages are shallower but wide: this is layout work removed from
  every flex row on every page, not only deep ones.
* Container queries no longer defeat incremental restyle. The second
  cascade pass — the one that runs with container sizes known — took the
  branch that throws the previous pass's computed styles away, so every
  page using `@container` re-cascaded every element from scratch on
  every relayout, forever. The cache now survives that pass, and rules
  carrying a container condition no longer force conservative
  invalidation keys either: those rules are inert in the first pass,
  which is the only one incremental restyle runs in. On a 1610-element
  container-query page churning through 13 relayouts, cascade time drops
  from 47ms to 12ms and style reuse goes from 0 to 1609 of 1610
  elements per pass.
* A `:has()` selector whose subject is matched by an attribute — say
  `[data-state]:has(...)` — keys invalidation on that attribute instead
  of switching incremental restyle off for the whole page. `class`, `id`
  and `style` are excluded, since keying on those matches nearly every
  element and floods the document anyway.
* `:nth-child`/`:nth-last-child` sibling indices are computed once per
  selector-matching batch instead of per element.
* Live DOM collections use the QuickJS array-index atom fast path, and
  childlist invalidation is narrowed to the affected parent.

User interface
* The `about:start` splash is redesigned as a 1997 Netscape release
  screen — beveled chrome, dithered sky, receding cyberspace grid —
  then reworked into a daytime scene whose ground is a procedurally
  generated planet surface sampled through the real pinhole
  projection, with an ocean-to-snow terrain ramp, clouds, a specular
  sun column and atmospheric haze into the limb. The loading bar is
  gone and the animation is 322 KB, down from 1.8 MB.
* Android: the kebab icon is readable on the toolbar, nested consent
  dialogs scroll, and the renderer thread no longer frees the
  shell-owned framebuffer.

Documentation, build and CI
* A whole-system architecture poster (`docs/Software-Architecture.png`)
  maps the process tree, IPC boundaries, engine pipeline, module
  dependencies and information flows; the generators live in
  `scripts/arch-diagram/`.
* The CSS and HTML compatibility documents are refreshed, and
  `docs/media.md` describes the MSE eviction model.
* CI hardens the V8 artifact download with retries and integrity
  checks, and the V8 smoke test now covers microtasks and timers.
* The readme links the openSUSE RPM directly.

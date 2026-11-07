# The engine protocol

The engine protocol is the contract between the browser shell (the GTK
window process, the Android and JVM shells, the headless driver) and an
engine renderer. It is the seam along which the engine is pluggable: a shell
that speaks this protocol can drive any renderer that implements it, and a
renderer that implements it can be driven by any shell. Everything the shell
knows about a page passes through it; the shell never links the engine
(see [Software-Architecture.md](Software-Architecture.md), §2).

The protocol is deliberately plain: HTTP/1.1 POST requests with small JSON
bodies over an inherited socket (`AF_UNIX` socketpair on fd 3, stdio pipes on
Windows, stdio for JVM/Android embedders), and a shared-memory framebuffer
for pixels. It is readable in a trace and trivially extensible. The framing
lives in `src/ipc_http.c`, the shell-side client in `src/rproc_http.c`, the
renderer-side dispatcher in `src/renderer_serve.c`, and the shared constants
in `src/engine_protocol.h`.

## Versioning and the handshake

`src/engine_protocol.h` defines `NS_ENGINE_PROTOCOL_VERSION`. The first
request a shell sends after spawning a renderer is `POST /hello`; the
renderer answers

```json
{"protocol": 1, "engine": "nordstjernen", "version": "1.0.44-dev"}
```

A shell refuses a renderer that does not answer, or whose `protocol` differs
from its own (`spawn_checked` in `src/rproc_http.c`), so a mismatched install
fails at start-up with a clear message instead of misbehaving later.

Rules for evolving the protocol:

- Adding an endpoint, a JSON field, or a side-channel header does **not**
  change the version. A shell must ignore fields and headers it does not
  know; a renderer answers `404` for paths it does not know.
- Changing the meaning of an existing endpoint, field or header, or removing
  one, **bumps** `NS_ENGINE_PROTOCOL_VERSION`.
- The `engine` string names the implementation; a shell may use it for
  display and diagnostics only, never to change behaviour.

## Requests (shell to renderer)

Every request is `POST <path>` with an `application/json` body. The reply is
`200` with a JSON or empty body, or an HTTP error status.

| Area | Paths |
|------|-------|
| Lifecycle | `/hello`, `/quit`, `/viewport`, `/colorscheme`, `/clear-cache`, `/policy` |
| Navigation and rendering | `/open`, `/render`, `/tick`, `/favicon`, `/dump`, `/export`, `/print`, `/screenshot`, `/pdf` |
| Pointer and keyboard | `/click`, `/hover`, `/scroll`, `/scrollbar-press`, `/scrollbar-drag`, `/scrollbar-release`, `/release`, `/key`, `/select`, `/link`, `/dropfiles`, `/contextmenu` |
| Editing | `/focused-editable`, `/focused-editable-state`, `/focused-editable-selection`, `/clipboard`, `/find` |
| Scripting and diagnostics | `/eval`, `/console` |
| Popups and downloads | `/popup-poll`, `/popup-deliver`, `/download-start`, `/download-poll`, `/download-cancel` |
| Permissions and capabilities | `/permission`, `/capability` (`/camera` and `/webgl` are deprecated aliases) |
| Storage | `/session-storage` |
| Media | `/media`, `/audio-event` |

`/render` and `/tick` are the heartbeat: the shell calls them from its frame
clock and the renderer reports page geometry, animation state and the
side-channel events below in the reply.

## Side channels (renderer to shell)

A renderer never initiates a request. Anything it needs from the shell rides
the next `/render` or `/tick` reply as an `X-*` header (or a field of the
`/tick` JSON), drained from the engine's pending queues:

| Header | Meaning |
|--------|---------|
| `X-Nav` | a script- or link-initiated navigation the shell must perform |
| `X-Download` | a download offer (the shell decides, prompts and writes the file) |
| `X-Popup` | a `window.open` request |
| `X-Capability` | one or more `<kind> <origin>` entries separated by `;`: the page is suspended until the shell decides (see "Capabilities") |
| `X-Camera` | deprecated: the origin of a pending camera request, kept for shells that predate `X-Capability` |
| `X-WebGL` | WebGL activity, for the status bar |
| `X-Audio` | commands for the audio helper the shell runs |
| `X-Config` | a settings change requested by a privileged page |
| `X-Window-Action` | fullscreen enter/exit |
| `X-Pointer-Lock` | pointer-lock state |

The shell validates every side-channel value before acting on it (allowed
schemes, allowed actions, allowed paths; see
[SECURITY.md](../SECURITY.md), "Process isolation"). The renderer is the
untrusted side of this boundary.

## Capabilities

Everything a page may do beyond rendering its own content is a
**capability** with a name, a user-facing label and prompt, a default state
and a revoke action. The vocabulary lives in `src/capability.h` and is linked
into both the shell and the engine, so the two sides never disagree about a
name or a default:

| Kind | Default | On change |
|------|---------|-----------|
| `camera` | ask | live tracks are stopped when the state becomes deny |
| `webgl` | allow | the page is reloaded when the blocked state flips |
| `downloads` | allow | gated in the shell: an offer (`X-Download`) is held while the prompt is open and dropped on deny |
| `popups` | allow | `window.open()` returns null on deny (the engine keeps the state); on ask the shell holds the open request while the prompt is open |
| `pointer-lock` | allow | `requestPointerLock()` fires `pointerlockerror` on deny; on ask the element waits in the engine and the lock engages (`pointerlockchange`) or fails when the shell answers |
| `fullscreen` | allow | `requestFullscreen()` rejects with `TypeError` and fires `fullscreenerror` on deny; on ask the promise stays pending until the shell answers |

The flow is the same for every kind:

1. The engine reaches a gate (for the camera, `getUserMedia`). With a
   recorded decision it proceeds or refuses at once. Without one it
   **suspends** the operation at its asynchronous point (the pending promise)
   and queues a request.
2. The next `/render` or `/tick` reply carries `X-Capability: camera
   https://example.org`. The engine does not poll and has no timeout; the
   page simply waits.
3. The shell looks the origin up in its ledger (`src/gtk/permissions.c`,
   `permissions.ini`). A stored or default decision is pushed back at once;
   otherwise the shell shows its prompt bar.
4. The decision reaches the engine as `POST /permission` with `entries` of
   `<kind> <state> <origin>` lines (the ledger format, also used to push a
   site's whole state on navigation and whenever the user changes a setting),
   or as `POST /capability` with `{"kind", "origin", "allow"}` for a single
   answer. The engine records it, resumes or rejects the suspended operation
   and, on a later deny, runs the kind's revoke action.

A grant or a revocation therefore works the same whether it happens before
the page loads (site settings) or while it runs (the prompt bar or a settings
change), and adding a capability means adding one row to the vocabulary, a
gate in the engine and nothing in the shell.

## What a shell may depend on

A shell depends on `src/engine_protocol.h`, `src/rproc_http.h` and
`src/ipc_http.h`, plus the platform library (settings, i18n, history,
bookmarks, the sandbox). The GTK shell still includes three engine headers
for one purpose, rasterising its own SVG icons through the engine's SVG
renderer (`src/gtk/icons.c`); that is the remaining coupling to remove, by
moving icon rasterisation behind the protocol or pre-rendering the icons at
build time.

## Screenshots

`POST /screenshot` with `{"x", "y", "width", "height", "scale"}` (CSS pixels
and a device scale; a zero `width` or `height` means up to the page's edge,
so an empty object is the whole page) answers with an `image/png` body and
`X-W` / `X-H`, the image's size in device pixels. The renderer paints the
document in one pass into a surface of its own, so the result is not bound
by the framebuffer the shell asked for at spawn and a `position: fixed`
element appears once. Each side is capped at 30,000 device pixels. An empty
body means there was nothing to capture.

## Paged PDF

`POST /pdf` answers with an `application/pdf` body and `X-Pages`, the number
of sheets. Every field is optional: `width`, `height` and the four
`margin_*` values in CSS pixels (A4 and half an inch by default), `scale`,
`landscape`, `ignore_css_page_size`, `screen_media`, `title`, `author`,
`creation_date`, `page_ranges`, and `header_left` / `header_center` /
`header_right` / `footer_left` / `footer_center` / `footer_right`. See
[printing.md](printing.md), "PDF from a program". An empty body means the
document could not be printed.

## A document from a string

`POST /open` with an `html` field shows that markup as the document instead
of fetching `url`, which then only names the document: relative URLs resolve
against it and it decides the origin. Leave `url` out and the document is
`about:blank`. Nothing is fetched for the document itself and it is never
taken from or parked in the back/forward cache; its subresources load as
they would for any page at that URL, so a `file:` URL lets it read files in
that directory and below.

## Resource policy

`POST /policy` says what the renderer may load from then on. It replaces the
previous policy; a renderer starts with none.

| Field | Meaning |
|-------|---------|
| `offline` | non-zero: no `http`, `https`, `ws`, `wss` or `ftp` request at all |
| `block_file` | non-zero: no `file:` URL loads, navigations included |
| `block_private` | non-zero: no private, link-local or loopback address |
| `allow_hosts` | host patterns, separated by newlines, commas or spaces; when present only these hosts load, so an empty string allows none |
| `deny_hosts` | host patterns that never load; checked before `allow_hosts` |

A pattern is a host name, `*.example.com` for the subdomains of
`example.com` (not `example.com` itself) or `*`. The policy is checked for
every hop of every request the page causes: navigations, redirects,
subresources, `fetch`, `XMLHttpRequest`, preconnects, WebSocket and
EventSource. `data:`, `blob:` and `about:` URLs are not requests and always
load. `block_private` looks at the host in the URL and again at the address
the connection is made to, so a public name that resolves to `127.0.0.1` is
refused as well; through a proxy only the name can be judged. A refused
request fails with `blocked by the embedder's resource policy`.

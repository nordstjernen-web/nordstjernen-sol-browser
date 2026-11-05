# The engine protocol

The engine protocol is the contract between the browser shell (the GTK
window process, the Android, iOS and JVM shells, the headless driver) and an
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
| Lifecycle | `/hello`, `/quit`, `/viewport`, `/colorscheme`, `/clear-cache` |
| Navigation and rendering | `/open`, `/render`, `/tick`, `/favicon`, `/dump`, `/export`, `/print` |
| Pointer and keyboard | `/click`, `/hover`, `/scroll`, `/scrollbar-press`, `/scrollbar-drag`, `/scrollbar-release`, `/release`, `/key`, `/select`, `/link`, `/dropfiles`, `/contextmenu` |
| Editing | `/focused-editable`, `/focused-editable-state`, `/focused-editable-selection`, `/clipboard`, `/find` |
| Scripting and diagnostics | `/eval`, `/console` |
| Popups and downloads | `/popup-poll`, `/popup-deliver`, `/download-start`, `/download-poll`, `/download-cancel` |
| Permissions and capabilities | `/permission`, `/camera`, `/webgl` |
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
| `X-Camera` | a request to use a capability that needs the user's decision |
| `X-WebGL` | WebGL activity, for the status bar and the WebGL permission |
| `X-Audio` | commands for the audio helper the shell runs |
| `X-Config` | a settings change requested by a privileged page |
| `X-Window-Action` | fullscreen enter/exit |
| `X-Pointer-Lock` | pointer-lock state |

The shell validates every side-channel value before acting on it (allowed
schemes, allowed actions, allowed paths; see
[SECURITY.md](../SECURITY.md), "Process isolation"). The renderer is the
untrusted side of this boundary.

## What a shell may depend on

A shell depends on `src/engine_protocol.h`, `src/rproc_http.h` and
`src/ipc_http.h`, plus the platform library (settings, i18n, history,
bookmarks, the sandbox). The GTK shell still includes three engine headers
for one purpose, rasterising its own SVG icons through the engine's SVG
renderer (`src/gtk/icons.c`); that is the remaining coupling to remove, by
moving icon rasterisation behind the protocol or pre-rendering the icons at
build time.

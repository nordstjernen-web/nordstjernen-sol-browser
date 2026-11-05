# Direction

This page states who Nordstjernen is for, what makes it different, and the
four architectural commitments the codebase is being moved towards. It is
the reference for deciding what to build next and what to decline. The
measured state of the engine lives in [wpt-scores.md](wpt-scores.md) and
[HTML-compatibility.md](HTML-compatibility.md); the current structure in
[Software-Architecture.md](Software-Architecture.md).

## Who it is for

Nordstjernen is for people who need to know what their browser is doing:
developers, security and privacy practitioners, systems administrators,
researchers, and the technically curious who read source. They run Linux,
macOS or Windows, keep many tabs for a long time, and are prepared to grant
a site a capability deliberately rather than have it granted by default.
They are not served by a browser whose process boundary, permission state
and session state are opaque, and they distrust a browser they cannot build.

The answer to "why not one of the many generated browsers" is concrete for
this group and must stay true:

1. **A readable, single-maintainer-scale engine** written from scratch,
   with no upstream browser engine code, that one person can build, read and
   audit (see "Minimalism" in [README.md](../README.md)).
2. **An inspectable process boundary.** The shell and the engine talk HTTP
   and JSON over a socket, readable in a trace; the engine runs in a
   sandboxed process per tab and is treated as untrusted by the shell
   ([engine-protocol.md](engine-protocol.md), [SECURITY.md](../SECURITY.md)).
3. **Capabilities are granted, revocable and visible.** Nothing a site can
   do with the user's machine or data is on by default; what is granted can
   be withdrawn while the page runs (commitment B below).
4. **Sessions are data, not state.** Tabs, their history and their storage
   live in the profile database as the source of truth, not in a window's
   memory that is saved on exit (commitment C).
5. **Standards, measured honestly**: scores against the shared test suites
   are published per area, including the weak ones.

Everything else, from page speed to media support, is table stakes that
must not regress but does not distinguish the project.

### Monthly user test

Once a month, five to eight people from the audience above use a current
build for their own work for an hour, with a maintainer watching and not
helping. The session has three fixed questions: what did the browser do
that you could not see or explain; what did you have to grant, and did you
understand what you granted; what broke. Findings go into issues tagged
`usertest`, and the differentiation list above is re-read against them. A
difference nobody in the room noticed is not a difference.

## Architectural commitments

### A. The engine is pluggable behind one versioned contract

The shell is engine-free and drives a renderer only through the engine
protocol. The protocol is versioned (`src/engine_protocol.h`, the `/hello`
handshake) and documented endpoint by endpoint in
[engine-protocol.md](engine-protocol.md). The target: a shell links nothing
from `src/` except `rproc_http`, `ipc_http`, `engine_protocol.h` and the
platform library; the engine links nothing from `src/gtk/`; and the renderer
executable is replaceable by any program that answers the protocol.

Remaining work, in order:

1. Move icon rasterisation out of the GTK shell's engine includes
   (`src/gtk/icons.c` is the last engine-header user in the shell).
2. Make the side channels (`X-*` reply headers) a documented, enumerated
   event list instead of ad hoc headers, so a second engine can produce them.
3. Give the JVM, Android and iOS shells the same handshake.

### B. Capabilities: granted, suspended, revocable

Every ability a page has beyond rendering its own content is a
**capability** with an origin, a state (`ask`, `allow`, `deny`) and a
decision point. The shell owns the ledger (`src/gtk/permissions.c`); the
renderer owns the gate. The vocabulary is shared (`src/capability.h`),
the request channel is generic (`X-Capability`, `/capability`,
`/permission`; see [engine-protocol.md](engine-protocol.md),
"Capabilities"), the ledger falls back to each kind's default, and the
shell's prompt bar and site settings are driven by the vocabulary. Today it
holds camera (ask), WebGL (allow, revocable) and downloads (allow, gated in
the shell where the file is written). The target extends the
vocabulary without changing the shape:

- **One vocabulary.** A capability enumeration shared by shell and renderer:
  camera, microphone, geolocation, notifications, clipboard read, downloads,
  popups, fullscreen, pointer lock, autoplay with sound, WebGL, WebGPU,
  WebAssembly, workers, WebSocket and EventSource, storage (localStorage,
  IndexedDB, cookies), third-party requests, private-network access, local
  file access.
- **Halt and continue.** A request for a capability in state `ask`
  suspends the operation at its natural asynchronous point (the pending
  promise, the blocked request, the unstarted worker) and reports it to the
  shell as one generic side-channel event with an id. The shell answers with
  `/permission` for that id. Nothing in the renderer polls or times out
  waiting for the user.
- **Revocation during execution.** A change of state in the ledger is
  pushed to every renderer immediately (`ns_perm_add_listener` already
  fans out to tabs), and each capability has a defined revoke action: stop
  the camera tracks, close the sockets, deny the next storage access, drop
  the lock. Granting and revoking work the same whether done before the
  page loads (site settings) or while it runs (the permission bar).
- **Default deny for new capabilities.** Adding a capability to the
  vocabulary adds it in state `ask`; the existing default-on behaviours
  (WebGL, WebGPU, storage, workers) are moved into the vocabulary one at a
  time with a visible setting, never silently switched to `ask`.

### C. Sessions are core data

A session is the set of tabs with their navigation entries, scroll
positions and per-tab storage. It is stored in `session.db` in the profile
(`src/gtk/sessionstore.c`) as the source of truth: a window is a view of
rows it currently owns, a crashed or closed instance leaves its rows for the
next instance to claim, and there is no "restore session" step because
nothing was ever only in memory. The target closes the remaining gaps:

1. Write every navigation and scroll change through to the database as it
   happens, not on a timer or at exit.
2. Give per-tab storage (`sessionStorage`, form state) the same treatment,
   keyed by the tab row, so a tab survives its renderer, its window and its
   process.
3. Let two instances show the same session (the second one read-only) and
   let a tab move between windows and machines by moving its row.

### D. Isolation first

The process boundary is the security boundary. Per-tab renderers are
sandboxed today; the next steps are listed in
[tab-isolation.md](tab-isolation.md) and ranked in SECURITY.md: broker
network, cookies, cache and storage through the shell so a renderer holds
only its own site's data, then run cross-site frames in their own renderer.
Every capability in B is enforced on the shell side of the boundary where
it can be, so a compromised renderer cannot grant itself anything.

## How to use this page

When a change is proposed, it must name which of A, B, C or D it serves, or
which item of the differentiation list it protects. A change that serves
none is not wrong, but it waits behind the ones that do.

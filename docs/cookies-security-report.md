# Cookie store security review

Scope: the browser-owned cookie implementation described in
[cookies.md](cookies.md) as checked out in this tree (`src/cookies.c`,
the cookie paths of `src/net.c`, `src/js.c`, `src/ws.c`,
`src/eventsource.c`, `src/engine.c`, `src/renderer_serve.c`,
`data/js/polyfills.js`, `src/db.c`, the Landlock grant in
`src/security.c`). The review is a source reading; no finding below was
exercised against a running build, so each one names the code path it
rests on. Severity follows the usual CVSS-ish buckets: High means a
remote page defeats a security boundary the web platform promises
(SameSite, Secure, sandbox, site isolation of cookies); Medium means a
boundary is weakened in a realistic setup; Low means a hardening gap or
a correctness problem with a narrow security consequence.

Findings are ordered by severity. Each one ends with a suggested fix.

## Summary

| # | Severity | Title |
| --- | --- | --- |
| 1 | High | SameSite is not enforced on a navigation that reaches a cross-site target through a redirect |
| 2 | Medium | Any hostname beginning with `127.` is a secure context, so `Secure` cookies travel over plaintext HTTP |
| 3 | Medium | WebSocket and EventSource cookies are keyed by the frame origin, bypassing partitioning and the first-party policy |
| 4 | Medium | A sandboxed iframe without `allow-same-origin` reads its origin's cookies through `cookieStore` |
| 5 | Low | A cross-site response may set `SameSite=Lax`/`Strict` cookies |
| 6 | Low | "Remove site cookies" and "Clear all" can be undone by another renderer's write-behind queue |
| 7 | Low | User-activated navigations whose URL differs from the recorded one lose their initiator |
| 8 | Low | `Max-Age` arithmetic can overflow |
| 9 | Low | The public-suffix check silently disappears when libpsl has no built-in list |
| 10 | Low | EventSource and WebSocket ignore the credentials mode |
| 11 | Low | File-mode hardening has gaps |
| 12 | Info | Every renderer process has read-write access to every site's cookies (documented, accepted) |
| 13 | Info | `document.cookie` cache is keyed only by store generation |

What the implementation gets right is listed at the end.

## 1. High: SameSite is not enforced across a redirect that leaves the site

**Status: fixed.** The redirect loop now carries `X-ND-Initiator` and a
sticky `X-ND-Redirect-Cross-Site` marker into later hops, and a
navigation's partition is its own destination.

**Where.** `src/net.c`, the redirect loop in `ns_fetch_sync` (the
`for (;;)` loop that calls `ns_fetch_sync_hop` per hop) and the context
computation in `ns_fetch_sync_hop`; `src/engine.c`
`engine_request_blocking`; `src/renderer_serve.c` (the `/open` handler
that calls `ns_browser_set_next_referrer` / `ns_browser_set_next_initiator`).

**What happens.** The SameSite context of a request is derived from
`same_site_ref = initiator ? initiator : top_url`. For a navigation,
`initiator` is read from the `X-ND-Initiator` header only when
`top_url` is NULL; `engine_request_blocking` only emits that header when
it was given an initiator. The renderer hands the shell's navigation
request to `browser_open_common` with one of two things: when the
target is same-origin with the current page, the current page URL
becomes the *referrer* and is passed as `top_url`; only when it is
cross-origin does it become the *initiator*. So a same-origin navigation
carries no initiator header at all.

In the redirect loop, after the first hop of a GET navigation,
`cur_top` is set to NULL and the hop headers are reused. The second hop
therefore has `top_url == NULL`, no `X-ND-Initiator`, hence
`same_site_ref == NULL` and `samesite_cross == 0`: the request is
treated exactly like a URL the user typed, and every cookie of the
target, including `SameSite=Strict`, is attached.

**Attack.** The attacker's page on `evil.example` links to
`https://evil.example/go`, which answers `302 Location:
https://bank.example/transfer?to=attacker`. The click is same-origin, so
the renderer records a referrer, not an initiator. Hop 1 is same-site
(correct). Hop 2 reaches `bank.example` with `samesite_cross == 0` and
sends Strict and Lax cookies. A direct cross-site link would have been
handled correctly (`samesite_cross == 1`, Lax only), so the open
redirect on the attacker's own site is the whole bypass.

With a form `POST` to the attacker's own origin answered by `307`, the
method and body are preserved (`cur_method` stays `POST` for 307/308)
and the second hop is a cross-site `POST` carrying Lax and Strict
cookies: SameSite is defeated entirely, which is the CSRF defence most
sites now rely on.

**Why it matters.** `SameSite=Lax` is the default and `Strict` is the
documented protection against cross-site request forgery. A page on any
origin can forge authenticated top-level GET and POST requests to any
other site.

**Fix.** Keep the site-for-cookies across the chain:

- In `ns_fetch_sync`, before clearing `cur_top` for a navigation, add
  `X-ND-Initiator: <origin of cur_top>` to `hop_headers` when none is
  present, so later hops still know who started the navigation.
- Better, follow Chromium and the 6265bis "redirect chain" rule: a
  navigation is cross-site if the initiator *or any URL in the redirect
  chain* is cross-site with the final URL. Track a `chain_cross_site`
  flag in the loop and force `samesite_cross` to at least 1 (and 2 for
  non-GET) when it is set.
- In `renderer_serve.c`, always record the initiator for page-initiated
  navigations (same-origin ones too); the referrer can stay a separate
  value.

## 2. Medium: hostnames beginning with `127.` are secure contexts

**Status: fixed.** `url_is_secure_context` parses the host with
`g_inet_address_new_from_string` and asks for a loopback address.

**Where.** `src/cookies.c` `url_is_secure_context`:

```c
if (g_ascii_strcasecmp(host, "localhost") == 0 ||
    g_str_has_suffix(host, ".localhost") ||
    strcmp(host, "[::1]") == 0 || g_str_has_prefix(host, "127."))
    return TRUE;
```

**What happens.** `g_str_has_prefix(host, "127.")` was meant for the
loopback block but matches any DNS name whose first label is `127`,
such as `127.bank.example`. Such a host is treated as a secure context
for both directions of the store: `Secure` cookies are *sent* to
`http://127.bank.example/` and a `Set-Cookie: ...; Secure` (and
`__Secure-` prefixed) cookie is *accepted* from it over plaintext.

**Attack.** An on-path attacker (or anyone who controls DNS for the
victim) answers `127.bank.example` with their own address and injects a
redirect to `http://127.bank.example/` into any plaintext page. The
browser attaches every `Domain=bank.example; Secure` cookie in
cleartext. The same host can plant `__Secure-` cookies covering
`bank.example` (session fixation). HSTS on `bank.example` with
`includeSubDomains` would block this; most sites do not set it.

**Fix.** Replace the prefix test with a real loopback check: parse the
host with `g_inet_address_new_from_string` and test
`g_inet_address_get_is_loopback`, which covers `127.0.0.0/8` and `::1`
without matching names. Keep `localhost` and `*.localhost` as they are
(those are the WHATWG secure-context rules).

## 3. Medium: WebSocket and EventSource use the frame origin as the partition

**Status: fixed.** `ns_net_curl_apply_cookies` takes the top-level URL;
the WebSocket and EventSource bindings pass `ns_js_top_url`.

**Where.** `src/net.c` `ns_net_curl_apply_cookies`; callers in
`src/ws.c` and `src/eventsource.c`; the origin they pass is
`ns_url_origin_from(js->current_url)` in `src/js.c`, i.e. the origin of
the *document opening the connection*, not of the top-level page.

**What happens.** For HTTP fetches the partition is the top-level
site, the first-party policy compares the request with the top-level
site, and `samesite_cross` is computed against the top-level site or
initiator. `ns_net_curl_apply_cookies` has none of that: it partitions
by `ns_url_site_from(origin)`, applies the first-party policy by
comparing the URL with `origin`, and computes `samesite_cross` against
`origin`.

Inside a cross-site iframe this means:

- With the default `first_party` policy, `<iframe src="https://tracker.example">`
  on `news.example` gets no cookies on HTTP requests, but a
  `new WebSocket("wss://tracker.example/")` or `new EventSource(...)`
  from inside the frame sends `tracker.example`'s first-party cookies,
  so the third-party-cookie block is bypassed and the embedded tracker
  can identify the user.
- With the `always` policy, HTTP from the frame reads and writes the
  partition `https://news.example` while the socket reads the
  unpartitioned `https://tracker.example` jar, linking the partitioned
  identity to the first-party one.
- `samesite_cross` is 0 for a socket to the frame's own site, so
  `SameSite=Strict` and `Lax` cookies of the embedded site are sent from
  a cross-site context, which a fetch from the same frame would not do.

**Fix.** Give `ns_net_curl_apply_cookies` the top-level URL (the JS
side has it as `ns_js_top_url(js)`), derive the partition and the
first-party decision from it exactly as the fetch path does, and set
`samesite_cross` to 2 when the frame is cross-site with the top-level
site.

## 4. Medium: a sandboxed iframe reads its origin's cookies through `cookieStore`

**Status: fixed.** `ns_window_cookie_list` returns `[]` for a realm whose
frame is sandboxed without `allow-same-origin`, and the denied
`document.cookie` throws `SecurityError` on get and set.

**Where.** `src/js.c`: the iframe setup around `ns_make_realm_document`
calls `ns_realmdoc_deny_cookie` for frames whose sandbox lacks
`allow-same-origin`, but `ns_iframe_build_native_globals` →
`ns_js_setup_window_globals` binds `__nsCookieList` into every frame
realm, and `ns_window_cookie_list` answers from `js->current_url`, the
frame's URL, with no sandbox check.

**What happens.** `ns_realmdoc_deny_cookie` replaces only the
`document.cookie` getter. `cookieStore.get()/getAll()` (and
`__nsCookieList` itself if the polyfill has not run in that realm) go
through `ns_window_cookie_list`, which resolves the frame's own URL and
returns the matching non-HttpOnly cookies.

**Attack.** A site that sandboxes untrusted same-origin content (user
uploaded HTML, a markdown preview, an ad slot served from the site's own
origin) relies on `sandbox` giving that content an opaque origin so it
cannot read the session cookie. Here `await cookieStore.getAll()` inside
the sandboxed frame returns the embedding origin's cookies.

**Fix.** In `ns_window_cookie_list`, look up the frame of the calling
realm (`ns_js_frame_of_realm`) and return `[]` when its effective
sandbox has `NS_SANDBOX_ACTIVE` without `NS_SANDBOX_ALLOW_SAME_ORIGIN`;
`ns_iframe_is_cross_origin` already encodes that test. For completeness
make the denied `document.cookie` throw `SecurityError` on get and set
as the HTML standard requires, instead of returning `""` and silently
dropping the write.

## 5. Low: cross-site responses can set `SameSite=Lax` and `Strict` cookies

**Where.** `src/cookies.c` `ns_cookie_store_set` and
`ns_header_store_cookie` in `src/net.c`.

**What happens.** RFC 6265bis §5.6 step 15 says a cookie whose
same-site flag is not `None` is ignored when the request is cross-site.
The store never learns the request context: `ns_header_ctx` carries the
partition but not `samesite_cross`, and the script path has no notion
of the frame being cross-site with the top-level page. Under the
`always` policy a cross-site subresource or iframe can therefore install
Lax/Strict/default cookies into the partition, and script in a
cross-site frame can do the same. Under the default `first_party`
policy the request carries no partition and nothing is stored, so the
issue is limited to the opt-in setting.

**Fix.** Pass `samesite_cross` through `ns_header_ctx` into a new
parameter of `ns_cookie_store_set`, and reject a line whose resolved
same-site value is not `None` when it is non-zero. For the script path
compute cross-siteness from the frame's URL versus `ns_js_top_url`.

## 6. Low: deletion races with other renderers' write-behind queues

**Where.** `src/cookies.c` `partition_matches_locked`,
`store_queue_locked`, `ns_cookie_store_clear`,
`ns_cookie_store_clear_site`.

**What happens.** Every renderer process owns its own in-memory copy and
pending-operations table. Reading a cookie bumps `last_access`, and
once a minute per cookie that enqueues a *full upsert* of the cookie
(`store_queue_locked(p->name, c, FALSE)`). The upsert is written with
`INSERT OR REPLACE` up to 500 ms later. If the settings page in another
process runs "remove this site's cookies" or "clear all browsing data"
in that window, the delete lands first and the upsert then re-creates
the row, value included, with the same effect for a cookie that the
other process has just set. The deleting process does not see it (its
partition is dropped), but every other process and the next start do.

A tab that is actively talking to the site (requests every few hundred
milliseconds) keeps such an upsert pending most of the time, so the
user-visible effect is "I cleared the cookies and I am still logged in
in the other tab".

**Fix.** Make deletes stronger than stale upserts: either record a
`deleted_at` generation per partition in the database (a one-row
`clears` table) that flushers consult before applying an op queued
earlier than the clear, or stop enqueuing `last_access` as a full
upsert and write it as `UPDATE cookies SET last_access=? WHERE ...`,
which cannot resurrect a deleted row. The resurrection of a genuinely
new `Set-Cookie` queued before the clear is then the only remaining
window, and it is the same half-second Chrome and Firefox accept.

## 7. Low: user-activated navigations that do not match the recorded URL lose their initiator

**Where.** `src/renderer_serve.c` `/open` handler:

```c
gboolean page_initiated = !user_activated ||
    (s->page_nav && url && strcmp(s->page_nav, url) == 0);
```

and `src/gtk/procview.c`, where context-menu actions and "open in new
tab" call `do_load_full(..., user_activated = TRUE)`.

**What happens.** When the shell reports user activation, the renderer
only treats the navigation as page-initiated if the URL is byte-equal to
the last link it handed out. "Open link in new tab" never stashes the
href (`if (!newtab) session_stash_post(...)`), the context-menu "open
link" loads `v->ctx_link` directly, and any normalisation between the
href the renderer returned and the URL the shell sends back breaks the
comparison. In all those cases no initiator is attached and the
navigation is treated as user-typed: `samesite_cross == 0`, so
`SameSite=Strict` cookies go along with a navigation the page chose the
target of. Chromium treats "open link in new tab" as a cross-site
navigation and withholds Strict cookies.

**Fix.** Always send the initiator origin for a navigation whose target
came from page content, regardless of activation; use activation only
for the Lax-allowing-unsafe style decisions. Compare URLs after the
same normalisation the shell applies, or pass an opaque navigation id
instead of the URL.

## 8. Low: `Max-Age` arithmetic can overflow

**Where.** `src/cookies.c` `line_apply_attribute`:

```c
gint64 max_age = g_ascii_strtoll(tmp, &end, 10);
line->expires = max_age > 0 ? now + max_age : 0;
```

**What happens.** `g_ascii_strtoll` saturates at `G_MAXINT64`, and
`now + G_MAXINT64` is signed overflow, undefined behaviour in C. In
practice it wraps negative, the cookie is treated as expired on first
read and deleted, so the observable effect is a cookie that vanishes,
but the compiler is free to do otherwise under `-O2`.

**Fix.** Clamp: `line->expires = max_age > G_MAXINT64 - now ?
G_MAXINT64 : now + max_age;` (and consider capping at the 400-day limit
6265bis §5.5 recommends, which also bounds what `Expires` can ask for).

## 9. Low: the public-suffix check disappears when libpsl has no built-in list

**Where.** `src/cookies.c` `line_resolve_domain`; `src/net.c`
`ns_url_site_from`.

**What happens.** Both call `psl_builtin()` and skip the check when it
returns NULL. Distributions can build libpsl without the embedded list
(Debian builds it in; some others ship only the runtime-loaded list).
On such a build `Set-Cookie: id=1; Domain=com` from `a.com` is accepted
and sent to every `.com` host, and `ns_url_site_from` falls back to the
full hostname, so the same-site decisions quietly become same-host.

**Fix.** Fall back to `psl_latest(NULL)` (the system list) when
`psl_builtin()` is NULL, and when both are missing reject every
`Domain` attribute that has fewer than two labels or equals the
host's own last label.

## 10. Low: EventSource and WebSocket ignore the credentials mode

**Where.** `src/net.c` `ns_net_curl_apply_cookies`.

**What happens.** `EventSource` without `withCredentials` has
credentials mode `same-origin`: a cross-origin connection must not send
cookies. The helper has no credentials parameter and sends cookies to
any same-site host (and, under the `always` policy, to cross-site hosts
too, with `SameSite=None` filtering only). A page on `a.example.com`
therefore opens an authenticated event stream to `b.example.com`
without opting in, which the standard forbids.

**Fix.** Add a `credentials` flag to the helper; the EventSource binding
passes `withCredentials`, WebSocket passes TRUE (its handshake always
includes cookies).

## 11. Low: file-mode hardening gaps

**Where.** `src/net.c` `ns_net_cookie_dir`, `src/db.c` `ns_db_open`.

- `g_mkdir_with_parents(dir, 0700)` only sets the mode on directories
  it creates; an existing `cookies/` directory with a looser mode is
  left alone. The sandbox setup in `src/security.c` does `chmod(ns_cfg_root, 0700)`
  for the config root but not for the cookies subdirectory it creates.
- `ns_db_open` creates the database through SQLite (mode `0644 & ~umask`)
  and then `g_chmod`s it to `0600`, so there is a window at first start
  where the file is group/world readable under a permissive umask. WAL
  and shm files are created later and inherit the corrected mode.

**Fix.** `chmod(dir, 0700)` after `g_mkdir_with_parents`, and open the
database with a URI (`file:...?modeof=`) or create it beforehand with
`open(O_CREAT|O_EXCL, 0600)` so it never exists with a wider mode.

## 12. Info: every renderer process can read and write every site's cookies

**Where.** `src/security.c` (Landlock grants read-write on
`$XDG_CONFIG_HOME/nordstjernen/cookies` to each renderer);
[cookies.md](cookies.md) "Process model".

The design is documented and planned to change (tab-isolation step 6:
the shell owns the store, renderers get a read-only snapshot of their
partition). Until then, a memory-safety bug in any renderer, reached
from any site, yields every cookie of every site, HttpOnly included,
and lets the attacker write cookies for other sites. This is the single
largest cookie risk in the tree and is noted here so the report is
complete; the fix is the planned move, not a change in `cookies.c`.

## 13. Info: `document.cookie` cache is keyed only by store generation

**Where.** `src/js.c` `ns_document_refresh_cookie`.

The cached string is reused whenever the store generation has not
changed, without checking that `js->current_url` and the top URL are
the ones it was computed for. Frames currently escape this because
they use realm documents with their own uncached getter, and the main
realm's scope entry resets `current_url` to the top URL, so no leak was
found. The invariant is implicit, though; a future caller that reads
`document.cookie` while a frame scope is active would get the wrong
origin's cookies. Caching the URL pair alongside the generation closes
it for the cost of one `strcmp`.

## What the implementation gets right

- All SQL is prepared once and bound; cookie text never reaches SQL
  source. `ns_db_harden` turns on defensive mode and turns off
  extension loading, double-quoted strings and trusted schema.
- Control characters abort parsing; the name+value and attribute size
  limits match 6265bis; per-domain and per-partition counts are bounded
  with LRU eviction.
- Nameless cookies that would forge a named or prefixed cookie are
  rejected; `__Secure-`/`__Host-` are enforced for HTTP and script;
  `SameSite=None` requires `Secure`; script cannot set, replace or
  delete an HttpOnly cookie; the "leave secure cookies alone" rule is
  implemented.
- HttpOnly cookies are kept away from script on every path: the
  `document.cookie` getter and `__nsCookieList` ask without HttpOnly,
  and `Set-Cookie` lines are dropped from the raw header buffer that
  `XMLHttpRequest.getAllResponseHeaders()` and fetch headers are built
  from, so a page cannot read them off a response either.
- The retry after a failed certificate check sends no cookies and
  stores none from the response; cross-origin redirects strip
  caller-supplied `Cookie:` and `Authorization:` headers.
- `about:settings*` endpoints answer 403 unless the top-level page is a
  chrome page, and the cookie-clearing endpoints need a `POST`.
- Private mode keeps the store in memory and writes no file.

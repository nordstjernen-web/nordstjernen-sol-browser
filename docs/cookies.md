# Cookie storage: proposal for a browser-owned SQLite cookie store

A design proposal. Nothing in this document is implemented yet; the
*Today* section describes the code as it is in `src/net.c` and `src/js.c`
at the time of writing, the rest describes what should replace it.

Source today: `src/net.c` (`ns_cookie_store_impl`, `ns_cookie_collect`,
`ns_net_curl_apply_cookies`, `ns_net_cookie_jar_version`), `src/js.c`
(the `document.cookie` accessors and `cookieStore`).

## Summary

Replace the per-site Netscape cookie jar files, their `.js.txt` siblings
and the SameSite sidecars with one SQLite database per profile, owned by
browser code, with the in-memory cookie set as the source of truth and
the database as a write-behind backing store. libcurl stops reading
cookie files; the browser builds the `Cookie` header and parses
`Set-Cookie` itself, which it already does on every build that is not
linked against the forked libcurl.

What this buys:

- a `Set-Cookie` costs one in-memory update instead of two fsynced
  rewrites of whole files under a global lock, on the thread that drives
  every transfer of the tab;
- one cookie implementation instead of two (libcurl's engine on builds
  with `CURLOPT_COOKIE_SITE`, the browser's own on every other build), so
  behaviour no longer depends on which libcurl the distribution ships;
- a store the settings page can list, search and delete by site, and that
  `cookieStore` change events can be driven from;
- the shape the tab isolation plan asks for: cookies in the browser
  process, renderers credential-less (see
  [tab-isolation.md](tab-isolation.md)).

The same storage model is used by Chromium (`SQLitePersistentCookieStore`
behind an in-memory `CookieMonster`, batched write-behind) and Firefox
(`cookies.sqlite`, in-memory cookie service, asynchronous writes). Neither
reads the database on the request path and neither shares the file
between processes; one process owns the cookies.

## Today

Cookies are partitioned by the top-level site. Each partition is a
directory of text files under `$XDG_CONFIG_HOME/nordstjernen/cookies/`
(a private-mode profile keeps them under the private root instead):

| File | Contents |
| --- | --- |
| `<sha256(site)>.txt` | Netscape cookie jar of cookies set by HTTP responses |
| `<sha256(site)>.js.txt` | the same format, cookies set by `document.cookie` |
| `<jar>.samesite` | per cookie `SameSite` level, since the jar format has no column for it |

**Reading, HTTP request.** On a build with the forked libcurl
(`NS_CURL_HAVE_COOKIE_SITE`) both files are handed to libcurl with
`CURLOPT_COOKIEFILE` on every request, and libcurl parses them, applies
SameSite from `#curl:` lines and builds the header. On every other build
`ns_cookie_collect()` reads both files, loads the sidecar into a hash
table, matches domain, path, secure, expiry and SameSite in C and sets
`CURLOPT_COOKIE`. Both paths re-read the files for every request.

**Reading, `document.cookie`.** `ns_net_cookies_for_js()` is the same
collector without HttpOnly cookies. `src/js.c` caches the result and
revalidates it with `ns_net_cookie_jar_version()`, which stats both files
and mixes size and mtime into a version number.

**Writing.** `ns_header_store_cookie()` is called from the curl header
callback for every `Set-Cookie` line and `ns_net_cookie_store_from_js()`
from the `document.cookie` setter. Both land in `ns_cookie_store_impl()`,
which:

1. takes a process-wide mutex and an `flock` on `cookies/.lock`;
2. reads the whole jar, splits it into lines, drops the cookie being
   replaced and any expired line, appends the new line;
3. rewrites the file with `G_FILE_SET_CONTENTS_CONSISTENT` (temp file,
   fsync, rename), then `chmod 0600`;
4. loads the SameSite sidecar, updates it and rewrites it the same way;
5. releases both locks.

Every cookie a response sets therefore costs two fsynced file rewrites,
serialised across every partition and every tab of the process, and the
cross-process `flock` serialises the renderer processes too. Because the
header callback runs on the shared curl multi thread, every other
in-flight request of that renderer waits while the files are written. A
response that sets ten cookies holds the thread for tens of milliseconds
on an SSD and for hundreds on Android flash or a spinning disk.

**Other costs of the file layout.**

- The partition is only present as a hash in the file name. The settings
  page cannot list cookies by site, and "clear cookies" is
  `ns_net_cookies_clear()`, which empties the directory.
- Deleting one cookie is a full rewrite of its jar.
- Three files per partition, hundreds of partitions in a used profile.
- HttpOnly protection for `document.cookie` writes
  (`ns_cookie_jar_has_httponly()`) reads the HTTP jar again.
- `cookieStore` cannot fire change events; there is no change log.

## Goals

- `Set-Cookie` and `document.cookie` writes never wait for disk.
- Reads never touch disk after the partition's first use.
- One cookie implementation, independent of the libcurl build.
- Partition, SameSite, HttpOnly, source (HTTP or script), creation and
  last-access time stored as columns.
- Listing and deletion by site for the settings page; a change log for
  `cookieStore` events.
- Durable: a crash loses at most the last few seconds of cookie writes,
  never the database.
- Works unchanged on a distribution libcurl ≥ 8.5.

## Non-goals

- Encrypting cookie values at rest with the operating system keychain,
  as Chrome does. The file is `0600` like today; encryption can be added
  later as a column transform without a schema change elsewhere.
- Reading other browsers' cookie databases.
- Changing the cookie policy (first-party only by default, block all,
  allow all). The policy stays where it is, at the request path.

## Design

### One database per profile

`cookies/cookies.db`, opened through `ns_db_open()` like the HTTP cache,
history and IndexedDB stores (hardened open, `0600`, 2.5 s busy
timeout), with `journal_mode=WAL` and `synchronous=NORMAL` like
`src/cache.c`. A private-mode profile opens `:memory:`, so nothing is
written and nothing needs deleting on exit.

```sql
PRAGMA user_version = 1;

CREATE TABLE cookies (
  partition    TEXT    NOT NULL,            -- top-level site, "https://example.com"
  domain       TEXT    NOT NULL COLLATE NOCASE,  -- without a leading dot
  host_only    INTEGER NOT NULL,            -- 1: exact host, 0: domain cookie
  path         TEXT    NOT NULL,
  name         TEXT    NOT NULL,
  value        TEXT    NOT NULL,
  expires      INTEGER NOT NULL,            -- unix seconds, 0 = session cookie
  secure       INTEGER NOT NULL,
  httponly     INTEGER NOT NULL,
  samesite     INTEGER NOT NULL,            -- 0 default, 1 none, 2 lax, 3 strict
  source       INTEGER NOT NULL,            -- 0 http, 1 script
  created      INTEGER NOT NULL,            -- unix milliseconds
  last_access  INTEGER NOT NULL,
  PRIMARY KEY (partition, domain, host_only, path, name)
) WITHOUT ROWID;

CREATE INDEX cookies_partition ON cookies (partition);
CREATE INDEX cookies_expires   ON cookies (expires) WHERE expires > 0;
```

The partition is the top-level site string, not its hash, so the settings
page can group by it and show it. The domain is stored without the
leading dot and `host_only` carries what the dot meant; matching uses
`host_only` and a case-insensitive tail match, which is what
`ns_cookie_collect()` does today on the text form.

The partition column also covers CHIPS (`Partitioned` cookies): a cookie
set in a third-party context under the default first-party policy is
dropped today, and if the policy is ever relaxed to allow partitioned
third-party cookies the partition column is already the key they need.
No separate partition key is required.

`user_version` is bumped on every schema change and the open path runs
the migrations in order, as `src/cache.c` does for the cache index.

### The in-memory store is authoritative

A new `src/cookies.c` owns `ns_cookie_store`:

```
ns_cookie_store
  GHashTable partition -> ns_cookie_partition
  GQueue     pending       (ns_cookie_op: upsert | delete, with the cookie)
  guint      generation    (bumped on every change)
  sqlite3   *db
  GMutex     lock
```

An `ns_cookie_partition` holds its cookies in a hash table keyed by
`(domain, host_only, path, name)`, plus a `loaded` flag. A partition is
loaded from the database the first time anything asks for it (one
`SELECT ... WHERE partition = ?`), and stays in memory. Sites a session
never visits are never loaded, so startup cost is zero and the first
request for a site pays one indexed query.

Every read and write goes to memory. `Set-Cookie` parsing stays in
`ns_cookie_store_impl()`, which is reduced to parsing and policy: domain
checks against the host and the public suffix list, default path, the
`__Secure-`/`__Host-` prefix rules now in `src/js.c`
(`ns_cookie_prefix_ok()`, which moves here so HTTP and script share it),
HttpOnly protection for script writes, and the overlay rule that a
non-secure cookie may not replace a secure one. The result is one
`ns_cookie_store_set(partition, cookie)` call, which updates the table,
bumps `generation`, and queues an op.

Limits, applied on set: 4096 bytes for name plus value, 180 cookies per
domain and 3000 per partition, evicting expired cookies first and then
the least recently accessed. These are the RFC 6265bis recommendations
Chromium and Firefox follow.

### Write-behind

Writes reach the database from a flush, never from the request path:

- the queue is flushed in one transaction when 500 ms have passed since
  the first queued op, or when 512 ops are queued, or 30 s after the
  previous flush at the latest;
- `ns_cookie_store_flush()` is called on shutdown, before the private
  root is removed, before "clear cookies" and before a profile export;
- a flush that fails with `SQLITE_BUSY` after the busy timeout keeps the
  ops queued and retries at the next interval; the in-memory state is
  not touched by a failed flush;
- `last_access` updates are coalesced: one op per cookie per flush.

The flush runs on a GLib worker, not on the multi thread. A response
setting ten cookies now does ten hash table updates and no I/O; the
database sees one transaction a little later. Expired rows are deleted in
the same transaction (`DELETE FROM cookies WHERE expires > 0 AND expires <
?`), so there is no separate sweep.

Durability is the same as the HTTP cache's: WAL with `synchronous=NORMAL`
survives a process crash intact and may lose the last transaction on a
power cut. Losing a few seconds of cookies is the trade Chromium and
Firefox make too.

### Reading for a request

`ns_net_curl_apply_cookies()` and the hop setup in `ns_hop_transport()`
stop setting `CURLOPT_COOKIEFILE`, `CURLOPT_COOKIE_SITE` and
`CURLOPT_COOKIE_CONTEXT`. Instead one call

```
char *ns_cookie_store_header(store, partition, url, is_navigation,
                             same_site, include_httponly);
```

produces the `Cookie` header value, applied with `CURLOPT_COOKIE` as the
non-forked build already does. The matching rules are the ones in
`ns_cookie_collect()` today: host and domain match, path match, secure
only over https, not expired, SameSite against the request context
(`same_site` and `is_navigation` decide which of Lax, Strict and None are
sent), ordered longest path first and then by creation time as RFC 6265
asks. Matching cookies get `last_access` bumped.

The forked libcurl's `CURLOPT_COOKIE_SITE` path is thereby not needed. Its
SameSite logic is duplicated by `ns_cookie_collect()` already, and the
non-forked path is what every Linux distribution build runs. Removing the
`#ifdef` leaves one implementation.

The `CURLOPT_COOKIEFILE ""` plus `CURLOPT_COOKIELIST "ALL"` dance on the
TLS fallback retry (today near line 6220 of `src/net.c`) goes away with it;
a handle that must not send cookies simply has no `CURLOPT_COOKIE` set.

### `document.cookie` and `cookieStore`

The getter asks the store for the header of the document's partition and
URL without HttpOnly cookies. `src/js.c` keeps its cached string but
revalidates it against `store->generation` instead of stat-ing two files.
The setter parses into the same `ns_cookie_store_set()` with `source =
script`; the HttpOnly rule and the prefix rules apply in the store, so
the JS copy of the prefix check is deleted.

`cookieStore.set` and `cookieStore.delete` call the same two functions.
Every change appends to a small ring of `(generation, partition, cookie,
deleted)` records; a document registered for `change` events is told the
generation it last saw and gets the records after it, which fills the gap
noted in [HTML-compatibility.md](HTML-compatibility.md) for the Cookie
Store API.

### Settings page and management

Cookies become queryable:

- the settings page lists partitions with their cookie counts
  (`SELECT partition, count(*) ... GROUP BY partition`) and the cookies
  of one site, from the database after a flush;
- "Clear cookies" flushes, then `DELETE FROM cookies` and drops every
  partition from memory, in one place instead of `ns_net_empty_dir()`;
- deleting one site is `DELETE ... WHERE partition = ?` plus dropping the
  partition from memory;
- "Clear session cookies" is `WHERE expires = 0`.

### Process model

Renderers fetch for themselves today, so the store cannot yet live in one
process. The proposal lands in two steps that track the tab isolation
plan.

**Step 1, store in each renderer process.** Every renderer opens the same
`cookies.db` (WAL, so readers and the one writer at a time do not block
each other; the busy timeout covers two renderers flushing at once). To
keep tabs consistent with each other:

- a flush is also triggered at the end of each response that set cookies,
  so another tab sees the cookie within one transaction's time rather
  than after 500 ms;
- before serving a read, the store compares `PRAGMA data_version` with
  the value it saw at its last load or flush; a change means another
  process wrote, and the partition being read is reloaded with one
  indexed `SELECT`. `data_version` is a memory read in WAL mode and does
  not change for the connection's own writes;
- an own-process write to a partition that another process also changed
  is resolved last-writer-wins at the row level, which is what the jar
  rewrite does today.

Reloading only the partition being read keeps this cheap: a partition
holds a handful to a few dozen rows.

**Step 2, store in the browser process.** When networking moves behind the
renderer IPC, as [tab-isolation.md](tab-isolation.md) step 6 plans, the
shell owns the single store, attaches cookies to the requests it makes on
the renderer's behalf, and `data_version` polling is dropped. The
renderer keeps a read-only snapshot of its partition for the synchronous
`document.cookie` getter: the shell includes the partition's cookies that
script may see in the navigation commit, and sends a delta with
`generation` on every later IPC response for that tab. A `document.cookie`
write is applied to the snapshot at once and sent to the shell as a
`cookie-set` message, so script that sets then reads sees its own write.
Landlock then removes the cookies directory from the renderer's
filesystem view, and the renderer holds no credentials at all.

### Migration from the jar files

Jar file names are `sha256(site)`, so the partition cannot be read back
from the file. The first start after the change imports every jar:

1. for each `<hash>.txt` and `<hash>.js.txt`, parse the lines with the
   existing jar parser, read `SameSite` from the sidecar, and insert the
   rows with `partition = "legacy:" || <hash>`;
2. when a request arrives for partition `S`, the store looks for rows
   under `"legacy:" || sha256(S)` and `UPDATE`s them to partition `S`
   before loading. Sites the user visits again recover their name; sites
   never visited again keep the opaque key until they expire;
3. the jar files are deleted once imported, and the importer runs only
   while `cookies/*.txt` exists.

Session cookies in the jars (expiry 0) are dropped by the import, which
is what a restart does today.

### Security

- The file is created `0600` by `ns_db_open()`; WAL and shm siblings
  inherit the mode from SQLite.
- `ns_db_harden()` applies: defensive mode, no extension loading, no
  double-quoted strings, untrusted schema off.
- Every statement is prepared once and bound; no SQL is built from cookie
  text.
- A cookie name or value containing a control character, a `;`, or a
  tab or newline is rejected at parse time, as RFC 6265 requires; the
  text format today relies on tabs never appearing in a value.
- Values are not encrypted at rest (see *Non-goals*). The database is in
  the config directory the sandbox already protects.
- The renderer sandbox tightens in step 2 above.

### What happens to the libcurl cookie engine

Nothing in libcurl is used for cookies after this change: no
`CURLOPT_COOKIEFILE`, no `CURLOPT_COOKIEJAR`, no `CURLOPT_COOKIE_SITE`.
This is the design the libcurl maintainers point applications with their
own storage toward, it needs no fork, and the browser behaves the same on
the Debian, Fedora, openSUSE and Arch libcurl as on a bundled one. The
curl fork's cookie changes
([SameSite and Partitioned](https://github.com/nordstjernen-web/curl/pull/1),
[SQLite store](https://github.com/nordstjernen-web/curl/pull/3)) stay
useful for curl users but are not a dependency of the browser.

## Expected effect

| | Today | Proposed |
| --- | --- | --- |
| `Set-Cookie`, one cookie | read jar, rewrite jar (fsync), rewrite sidecar (fsync), under a global lock, on the multi thread | one hash table update, no I/O on the request path |
| Response setting 10 cookies | 20 fsyncs, roughly 20 to 300 ms of blocked transfers depending on the disk | about 10 µs, one transaction later off-thread |
| `Cookie` header | parse 2 files per request | hash table lookup; one indexed `SELECT` the first time a site is used |
| `document.cookie` revalidation | 2 `stat()` calls | integer compare |
| Delete one cookie | rewrite its jar | one row, in the next flush |
| List cookies by site | not possible | one query |
| Files in a used profile | 3 per site visited | 1 database plus WAL and shm |

The win is removing stalls from the transfer thread, not faster page
loads in general; cookie work is a small slice of a page load.

## Testing

- A `cookies-test` executable in `src/` run by `meson test`: set, get,
  replace, expire, host-only against domain, path matching, SameSite
  contexts, HttpOnly against script writes, prefix rules, the limits and
  eviction, flush and reload, migration from a jar directory, and two
  connections on one file with `data_version` detection.
- The WPT `cookies/` directory is not in the tracked runs
  (`docs/wpt-subtests.tsv` covers `dom`, `url`, `html`, `WebCryptoAPI`,
  `xhr`, `console`, `hr-time`). Add it to `scripts/wpt-run.sh` and record
  its score before and after in [wpt-scores.md](wpt-scores.md).
- Site checks by hand: login on Hacker News, GitHub and reddit survive a
  restart; a cookie set in one tab is sent by a second tab of the same
  site; private mode leaves no `cookies.db`.

## Implementation order

Small commits, each building and smoke-launching:

1. `src/cookies.c` with the store, schema, flush and the test executable.
   No caller yet.
2. Route `ns_cookie_store_impl()` writes into the store and build the
   `Cookie` header from it; remove `CURLOPT_COOKIEFILE` and the
   `NS_CURL_HAVE_COOKIE_SITE` branch; keep the jar importer.
3. `document.cookie` and `cookieStore` on the store; delete the
   version-by-stat cache and the JS prefix check.
4. `data_version` reload for multi-renderer consistency.
5. Settings page: list and delete by site; "clear cookies" through the
   store.
6. `cookieStore` change events from the change ring.
7. With the network broker of tab-isolation.md: move the store to the
   shell, snapshot plus delta over IPC, Landlock the cookies directory
   away from renderers.

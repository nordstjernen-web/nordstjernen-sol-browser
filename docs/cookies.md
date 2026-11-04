# Cookie storage: the browser-owned SQLite cookie store

Cookies live in one SQLite database per profile, owned by browser code
(`src/cookies.c`). The in-memory cookie set is the source of truth and the
database is a write-behind backing store. libcurl never reads or writes
cookie files: the browser builds the `Cookie` header and parses
`Set-Cookie` itself, so the browser behaves the same on a distribution
libcurl as on any other build. No fork of libcurl is involved.

Source: `src/cookies.c` (the store), `src/net.c` (partition choice, the
request path, the settings page), `src/js.c` (`document.cookie`),
`data/js/polyfills.js` (`cookieStore`).

## What the store does

- A `Set-Cookie` costs one hash-table update; nothing is written on the
  request path. The database sees one transaction a little later, on a
  worker thread.
- Reads never touch the disk after a partition's first use.
- One cookie implementation, independent of the libcurl build.
- Partition, SameSite, HttpOnly, source (HTTP or script), creation and
  last-access time are columns, so the settings page can list cookies by
  site and delete them.
- Durable: a crash loses at most the last half second of cookie writes,
  never the database.

The same storage model is used by Chromium (`SQLitePersistentCookieStore`
behind an in-memory `CookieMonster`, batched write-behind) and Firefox
(`cookies.sqlite`, in-memory cookie service, asynchronous writes).

## Design

### One database per profile

`cookies/cookies.db` under the config directory
(`$XDG_CONFIG_HOME/nordstjernen/` on Linux, `%LOCALAPPDATA%\nordstjernen\`
on Windows), opened through `ns_db_open()` like the HTTP cache, history
and IndexedDB stores (hardened open, `0600`, 2.5 s busy timeout), with
`journal_mode=WAL` and `synchronous=NORMAL`. A private-mode profile opens
`:memory:`, so nothing is written and nothing needs deleting on exit.

```sql
PRAGMA user_version = 1;

CREATE TABLE cookies (
  partition    TEXT    NOT NULL,            -- top-level site, "https://example.com"
  domain       TEXT    NOT NULL COLLATE NOCASE,  -- without a leading dot
  host_only    INTEGER NOT NULL,            -- 1: exact host, 0: domain cookie
  path         TEXT    NOT NULL,
  name         TEXT    NOT NULL,            -- "" for a nameless cookie
  value        TEXT    NOT NULL,
  expires      INTEGER NOT NULL,            -- unix seconds, 0 = session cookie
  secure       INTEGER NOT NULL,
  httponly     INTEGER NOT NULL,
  samesite     INTEGER NOT NULL,            -- 0 unspecified, 1 none, 2 lax, 3 strict
  source       INTEGER NOT NULL,            -- 0 http, 1 script
  created      INTEGER NOT NULL,            -- unix microseconds
  last_access  INTEGER NOT NULL,
  PRIMARY KEY (partition, domain, host_only, path, name)
) WITHOUT ROWID;

CREATE INDEX cookies_partition ON cookies (partition);
CREATE INDEX cookies_expires   ON cookies (expires) WHERE expires > 0;
```

The partition is the top-level site (scheme plus registrable domain, plus
the port when the URL has one; the host itself for an IP address), spelled
`https://example.com`, so the settings page can group by it and show it.
The domain is stored without the leading dot and `host_only` carries what
the dot meant. `created` is in microseconds and strictly increasing within
a process, which gives the `Cookie` header the stable creation order RFC
6265 asks for.

`user_version` is bumped on every schema change; a database with another
version is dropped and recreated.

### The in-memory store is authoritative

`src/cookies.c` keeps one `ns_cookie_store`: a hash table of partitions,
each a hash table of cookies keyed by `(domain, host_only, path, name)`, a
table of pending database operations keyed the same way (so repeated
writes to one cookie coalesce into one row write), a generation counter
bumped on every change, the SQLite connection and two mutexes (one for
the memory state, one for the connection).

A partition is loaded from the database the first time anything asks for
it, with one indexed `SELECT`, and stays in memory. Sites a session never
visits are never loaded, so startup cost is zero and the first request for
a site pays one query.

`ns_cookie_store_set()` does the parsing and the policy for both
`Set-Cookie` headers and `document.cookie` writes, following RFC 6265bis:

- control characters abort; a name plus value over 4096 bytes aborts; an
  attribute value over 1024 bytes is ignored;
- `Max-Age` wins over `Expires`; a non-positive age or a past date deletes
  the cookie;
- a `Domain` that is a public suffix is rejected (or made host-only when it
  is the host itself); a domain that does not cover the host is rejected;
- a cookie without `Path`, or with a path not starting with `/`, gets the
  default path of the request URL;
- `Secure` is accepted only from a secure context (`https`, or a loopback
  host); `SameSite=None` requires `Secure`; the `__Secure-` and `__Host-`
  prefixes are enforced for HTTP and script alike;
- script may not set an `HttpOnly` cookie nor replace or delete an
  existing one;
- a cookie set over an insecure connection may not overwrite a secure
  cookie of the same name that covers it ("leave secure cookies alone");
- a replaced cookie keeps the creation time of the one it replaces.

Limits apply on set: 180 cookies per domain and 3000 per partition,
evicting expired cookies first and then the least recently accessed.

### Write-behind

Writes reach the database from the `ns-cookies` thread, never from the
request path:

- the pending operations are flushed in one transaction 500 ms after the
  first one was queued, or at once when 512 are queued, when a response
  that set cookies has finished, or when script set a cookie;
- `ns_cookie_store_flush()` runs synchronously at shutdown and before the
  settings page lists cookies;
- a flush that fails (another process held the write lock past the busy
  timeout) keeps the operations queued and retries; the in-memory state
  is not touched by a failed flush;
- `last_access` is written at most once a minute per cookie;
- expired rows are deleted in the same transaction, so there is no
  separate sweep.

Durability is the HTTP cache's: WAL with `synchronous=NORMAL` survives a
process crash intact and may lose the last transaction on a power cut.

### Reading for a request

Every hop of a fetch (redirects are followed by the browser, one easy
handle per hop) calls

```
char *ns_cookie_store_header(partition, url, samesite_cross, include_httponly);
```

and applies the result with `CURLOPT_COOKIE`. The matching rules are RFC
6265's: host or domain match, path match, `Secure` only in a secure
context, not expired, SameSite against the request context
(`samesite_cross` is 0 for a same-site request, 1 for a cross-site
top-level navigation, which sends Lax and None, and 2 for a cross-site
subresource, which sends None only; an unspecified SameSite counts as Lax),
ordered longest path first and then by creation time. Matching cookies
get `last_access` bumped. WebSocket and EventSource connections take the
same path through `ns_net_curl_apply_cookies()`.

A handle that must not send cookies, such as the retry after a failed
certificate check, has no `CURLOPT_COOKIE`. Nothing in libcurl is used for
cookies: no `CURLOPT_COOKIEFILE`, no `CURLOPT_COOKIEJAR`, no cookie engine.

### `document.cookie` and `cookieStore`

The getter asks the store for the header of the document's partition and
URL without HttpOnly cookies. `src/js.c` caches the string and revalidates
it against the store's generation, an integer compare. The setter calls
`ns_cookie_store_set()` with `source = script`; the HttpOnly and prefix
rules live in the store, so there is no second copy in JavaScript.

`cookieStore` (`data/js/polyfills.js`) reads the cookies script may see,
with their real attributes, through `__nsCookieList()`
(`ns_cookie_store_visible_json()`), and writes through `document.cookie`
with the Cookie Store API defaults: path `/`, `SameSite=Strict`, `Secure`
in a secure context. `set()` and `delete()` fire a `CookieChangeEvent`
named `change` on `cookieStore` with the cookies they changed or deleted;
changes made by HTTP responses or by `document.cookie` do not fire events.

### Settings page

`about:settings` has a Cookies section listing partitions with their
cookie counts (`about:settings-cookies`, a `GROUP BY partition` query after
a flush) with a button to remove one site's cookies
(`about:settings-cookies-delete`, which deletes the rows and drops the
partition from memory) and one to remove them all. "Clear all browsing
data" clears the store the same way.

### Process model

Renderers fetch for themselves, so every renderer process opens the same
`cookies.db` (WAL, so readers and the one writer at a time do not block
each other; the busy timeout covers two renderers flushing at once). To
keep tabs consistent with each other:

- a flush is triggered at the end of each response that set cookies and
  after each script write, so another tab sees the cookie within one
  transaction's time rather than after 500 ms;
- before serving a read, the store compares `PRAGMA data_version` with
  the value it saw last; a change means another process wrote, and the
  partition being read is reloaded with one indexed `SELECT`, with this
  process's own unflushed operations applied on top. `data_version` is a
  memory read in WAL mode and does not change for the connection's own
  writes;
- a row changed by two processes resolves last-writer-wins.

When networking moves behind the renderer IPC, as
[tab-isolation.md](tab-isolation.md) step 6 plans, the shell owns the
single store, attaches cookies to the requests it makes on the renderer's
behalf, and `data_version` polling is dropped. The renderer then keeps a
read-only snapshot of its partition for the synchronous `document.cookie`
getter, and Landlock removes the cookies directory from its filesystem
view.

### Security

- The file is created `0600` by `ns_db_open()`; WAL and shm siblings
  inherit the mode from SQLite. The cookies directory is `0700`.
- `ns_db_harden()` applies: defensive mode, no extension loading, no
  double-quoted strings, untrusted schema off.
- Every statement is prepared once and bound; no SQL is built from cookie
  text.
- A cookie name or value containing a control character is rejected at
  parse time, as RFC 6265 requires.

**Values are not encrypted at rest.** Firefox stores `cookies.sqlite` in
the clear; Chrome encrypts values with a key held by the operating
system's keychain. That key is available to any program running as the
same user, which is exactly the attacker a profile-reading infostealer is,
so the encryption protected against copies of the file taken off the
machine but not against the common attack; Chrome has since moved to
binding the key to its own binary through a privileged service, which is
infrastructure of its own. Doing the same here would add a libsecret
dependency on Linux, keychain code on each platform, and keychain access
from the sandboxed renderer processes, while the realistic protection for
a profile directory is the file mode plus the sandbox, which the store
already has. The `value` column is plain text so an encrypted
representation can be added later as a column transform without touching
the rest of the schema.

### Private mode

A private-mode profile opens the database in memory. Each renderer
process has its own memory, so a cookie set in one private tab is not seen
by another private tab; this matches the private-mode root today, which
is a per-process temporary directory.

## Effect

| | Jar files (before) | Store (now) |
| --- | --- | --- |
| `Set-Cookie`, one cookie | read jar, rewrite jar (fsync), rewrite sidecar (fsync), under a global lock, on the transfer thread | one hash table update, no I/O on the request path |
| Response setting 10 cookies | 20 fsyncs, roughly 20 to 300 ms of blocked transfers depending on the disk | about 10 µs, one transaction later off-thread |
| `Cookie` header | parse 2 files per request | hash table lookup; one indexed `SELECT` the first time a site is used |
| `document.cookie` revalidation | 2 `stat()` calls | integer compare plus one `PRAGMA data_version` |
| Delete one cookie | rewrite its jar | one row, in the next flush |
| List cookies by site | not possible | one query |
| Files in a used profile | 3 per site visited | 1 database plus WAL and shm |
| libcurl | forked build decided SameSite; stock build re-parsed files | stock libcurl, no cookie engine |

The jar files of earlier versions are not read or migrated; the first
start with the store begins with an empty cookie database.

## Checking a change

- `scripts/wpt-score.sh --wpt-root=~/wpt cookies` scores the WPT
  `cookies/` directory.
- By hand: a login survives a restart; a cookie set in one tab is sent by
  a second tab of the same site; `about:settings` lists the site and
  removing it logs the tab out; private mode leaves no `cookies.db`.

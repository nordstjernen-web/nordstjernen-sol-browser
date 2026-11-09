# WebDriver

Nordstjernen has a built-in [W3C WebDriver](https://www.w3.org/TR/webdriver2/)
server, so Selenium, WebdriverIO, Nightwatch and other WebDriver clients can
drive it for automated testing. It speaks the classic HTTP protocol;
WebDriver BiDi is not implemented. The browser is its own driver: there is no
separate `nordstjernendriver` binary.

```sh
nordstjernen --webdriver            # port 4444
nordstjernen --webdriver=9515       # another port
nordstjernen --webdriver=0          # a free port, printed at startup
```

The server prints one line when it is ready,
`Nordstjernen WebDriver 1.0.46 listening on http://127.0.0.1:4444`, and runs
until it gets `SIGINT` or `SIGTERM`. `--viewport=WxH` sets the window size a
session starts with (default 1280×800), `--settle-ms=N` how long each load
settles (default 200) and `--debug` logs every request and reply to stderr.

## Clients

Ask for `browserName: "nordstjernen"`. With Selenium for Python:

```python
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.common.options import ArgOptions

options = ArgOptions()
options.set_capability("browserName", "nordstjernen")
driver = webdriver.Remote(command_executor="http://127.0.0.1:4444",
                          options=options)
driver.get("https://example.com/")
print(driver.find_element(By.TAG_NAME, "h1").text)
driver.quit()
```

With WebdriverIO, set `hostname: "127.0.0.1"`, `port: 4444` and
`capabilities: { browserName: "nordstjernen" }`. A `/wd/hub` prefix on every
path is accepted, for clients set up for a Selenium Grid URL.

## Capabilities

| Capability | Value |
|------------|-------|
| `browserName` | `nordstjernen`; a request for another browser is refused |
| `browserVersion` | the release; a requested version must be a prefix of it |
| `platformName` | `linux`, `mac`, `windows`, `android`, `freebsd`, `openbsd` or `netbsd` |
| `acceptInsecureCerts` | `false`; asking for `true` does not match, because untrusted certificates are only ever accepted through the `tls_allow_insecure_override` setting |
| `pageLoadStrategy` | `normal` (wait for `readyState` `complete`, the default), `eager` (`interactive`) or `none` |
| `proxy` | `direct`, `system` or `manual` (`httpProxy` / `sslProxy`, passed to the renderers as `http_proxy` / `https_proxy`); `pac` and `autodetect` do not match |
| `timeouts` | `script` 30000, `pageLoad` 300000, `implicit` 0 ms unless requested |
| `setWindowRect` | `true` |
| `strictFileInteractability` | as requested |
| `unhandledPromptBehavior` | `dismiss and notify`; no prompt ever blocks (see below) |
| `ns:options` | `{"private": true}` by default (see below); `{"private": false}` runs the session in the regular profile |

`webSocketUrl` is accepted and ignored, so a client that asks for BiDi falls
back to the classic protocol. Other extension capabilities (any name with a
`:`) are ignored. One session runs at a time.

A session is private unless `ns:options` says otherwise: its renderers start
from empty cookies and storage, write nothing to the profile, and each window
has its own cookie jar, as a private tab does. With `"private": false` the
session uses the profile's cookies, history and storage like an ordinary tab.

## Commands

Every endpoint of WebDriver Classic is routed:

- **Sessions**: New Session, Delete Session, Status, Get/Set Timeouts (the
  Selenium 3 `{"type", "ms"}` form too).
- **Navigation**: Navigate To, Get Current URL, Back, Forward, Refresh, Get
  Title. Back and Forward step through a page's `pushState` entries first,
  then through the documents the window loaded.
- **Contexts**: Get/Close/Switch To Window, Get Window Handles, New Window,
  Switch To Frame (by index, element or `null`), Switch To Parent Frame,
  Get/Set Window Rect, Maximize, Minimize and Fullscreen Window. Each window
  is a renderer process of its own. A window has no chrome, so its size is the
  viewport size; it is clamped to 2560×1600 (or the `--viewport` size), and
  Maximize and Fullscreen make it 1920×1080. A click that opens a new tab
  (Ctrl or the middle button) opens a new window.
- **Elements**: Find Element(s) from the document, an element or a shadow
  root, by `css selector`, `link text`, `partial link text`, `tag name` and
  `xpath`, with the implicit wait; Get Active Element, Get Element Shadow
  Root, Is Element Selected, Get Element Attribute, Property, CSS Value, Text,
  Tag Name and Rect, Is Element Enabled, Get Computed Role and Label, and the
  non-standard `GET .../element/{id}/displayed` that several clients call.
- **Interaction**: Element Click (scrolled into view, checked for being in
  view and not obscured, then a real press and release through the
  renderer's hit testing; an `<option>` is selected as the specification
  describes), Element Clear and Element Send Keys (keydown, keypress, input
  and keyup per character, with the WebDriver key code points
  U+E000–U+E05D for Enter, Backspace, the arrows, the modifiers and the
  rest).
- **Document**: Get Page Source, Execute Script and Execute Async Script.
  Scripts may return promises, which are awaited up to the script timeout.
  Elements, shadow roots, windows and frames pass both ways as WebDriver
  references; a cyclic result is a `javascript error`.
- **Cookies**: Get All Cookies, Get Named Cookie, Add Cookie (`httpOnly`,
  `secure`, `domain`, `path`, `expiry` and `sameSite` included), Delete
  Cookie and Delete All Cookies, against the cookie store partition of the
  page.
- **Actions**: Perform Actions and Release Actions with `key`, `pointer`
  (`mouse`, `pen`, `touch`), `wheel` and `none` sources: pauses, key down and
  up, pointer move (with `viewport`, `pointer` or element origins,
  interpolated over its duration), down, up and cancel, and wheel scroll.
  Pointer and wheel input is the renderer's own: a second click at the same
  point within 500 ms is a double click (with `dblclick` and word selection),
  the secondary button opens the context menu, and a wheel action fires a
  trusted `wheel` event and scrolls whatever the pointer is over.
- **Screen capture**: Take Screenshot (the viewport as the window shows it)
  and Take Element Screenshot (the element's box, painted by `/screenshot`,
  so an element larger than the viewport is captured whole).
- **Print**: Print Page, through the renderer's `/pdf` with `orientation`,
  `page`, `margin`, `scale` and `pageRanges`.

Errors carry the WebDriver error codes and HTTP statuses (`no such element`
404, `stale element reference` 404, `element click intercepted` 400 and so
on). A reference to an element of a document that has since been replaced
is a `stale element reference`.

## Differences and limitations

- **User prompts never block.** `alert()`, `confirm()` and `prompt()` write
  to the console and return at once (`confirm()` with `false`, `prompt()`
  with `null`), so there is never a prompt to accept or dismiss and the alert
  commands answer `no such alert`.
- **`window.open()` does not open a WebDriver window**, and a plain
  `target="_blank"` link loads in the window it is in.
- **File inputs** cannot be given files: sending keys to one is
  `unsupported operation`.
- Capability requests (camera, pointer lock, fullscreen, …) are answered with
  each capability's default state, so `allow` kinds proceed and `ask` kinds
  are refused; nothing waits on a prompt.
- **No BiDi**, no browser log endpoint, and one session at a time.

## Security

The server listens on the loopback interface only (127.0.0.1, and ::1 where
the machine has IPv6) and refuses:

- requests whose `Host` header is not `localhost`, `127.0.0.1` or `[::1]`
  (DNS rebinding),
- requests carrying an `Origin` header, which is what a web page sends,
- `POST` bodies typed `application/x-www-form-urlencoded`,
  `multipart/form-data` or `text/plain`, the types a page can send
  cross-origin without a CORS preflight.

There is no token: any local program that can reach the port can drive the
browser, so do not forward it. Unlike remote control it is not reachable at
all unless the browser was started with `--webdriver`. Pages run in the same
sandboxed renderers as in the browser window, and the WebDriver server itself
parses no page content.

The page-side half of every command, `data/js/webdriver-page.js`, runs in a
QuickJS context of its own beside the page (as the remote-control helper
does), created on the page's first WebDriver request and dropped with the
page's realm. Page script cannot name or reach it, its built-ins are its own,
and its natives (a frame's document and window past the cross-origin checks,
compiling a script in the page's or a frame's realm) live only in its
closure.

## How it works

`src/gtk/webdriver.c` is a shell in the sense of
[engine-protocol.md](engine-protocol.md): it never links the engine. A thread
accepts connections and one thread serves each connection, queueing every
request for the main thread. Each WebDriver window is one renderer process
spawned like a tab (`ns_rproc_http_spawn_shm_ex`), and between requests the
main thread sends each of them `/tick` once a frame and `/render` when the
page changed, as the GUI's frame clock does, so timers, fetches, navigations
and animations go on.

DOM work goes to the renderer as `POST /webdriver` with the request as JSON.
The renderer (`ns_js_webdriver_call` in `src/js.c`) passes it to the helper,
which finds elements, keeps the registry of element references, serializes
values both ways and works out where to click; the server turns that into
real input over the protocol (`/click`, `/release`, `/hover`, `/key`, the
wheel fields of `/render`), so events are trusted and default actions are the
engine's own. Cookies go through `POST /cookies`, which reads and writes the
page's partition of the cookie store, HttpOnly cookies included.

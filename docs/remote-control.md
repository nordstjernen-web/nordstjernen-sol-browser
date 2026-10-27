# Remote control over MCP

Nordstjernen can be driven by a local AI agent such as
[Claude Code](https://claude.com/claude-code), the way the "Claude in
Chrome" extension drives Chrome. The browser ships its own
[Model Context Protocol](https://modelcontextprotocol.io) server:
`nordstjernen --mcp` speaks MCP over stdio and forwards each tool call
to the browser window you have open, through a control channel that
only listens on the loopback interface and only while you allow it.

Nothing listens by default. Remote control is off until you turn it on.

## Setup

1. Register the MCP server with your agent. For Claude Code:

   ```sh
   # Linux / macOS
   claude mcp add nordstjernen -- /usr/bin/nordstjernen --mcp

   # Windows (MSYS2 build tree)
   claude mcp add nordstjernen -- C:\dev\nordstjernen\builddir\src\gtk\nordstjernen.exe --mcp
   ```

   Use the path of your own `nordstjernen` binary. Add `--scope user` to
   make it available in every project.

2. Allow remote control in the browser, either way:

   - **Settings → Privacy → Allow remote control** (the `remote_control`
     config key). It takes effect immediately in a running browser, and
     turning it off closes the channel at once.
   - Start the browser with `--remote-control` to allow it for that
     session only.

3. Ask the agent to use the browser ("open example.com in Nordstjernen
   and click the first link").

If no browser with remote control is running when the agent makes its
first tool call, `nordstjernen --mcp` starts one with `--remote-control`
and waits for its control channel. A browser that is already open with
remote control off is left alone; the agent gets a second window.

While a client is connected the toolbar shows an orange **Controlled by
…** badge naming the client (for example "Controlled by Claude Code").
Click it to stop remote control for the rest of the session: the
channel closes, and the MCP server reports that the user stopped it
instead of starting another browser. Turn the Settings switch off and on
again, or restart the browser, to allow it again.

## Tools

Tools take an optional `tab_id` (from `tabs_list`); without it they act
on the active tab of the most recently focused window.

| Tool | What it does |
| --- | --- |
| `tabs_list` | Open tabs with id, window, URL, title, active/private/loading flags. |
| `tabs_create` | Open a foreground tab at `url` (default `about:blank`) and wait for it to load. |
| `tabs_close` | Close a tab. The last tab of the last window is refused. |
| `tab_select` | Bring a tab to the front and present its window. |
| `navigate` | Load `url` and wait for the load to finish; `"back"` and `"forward"` walk the tab's history. Non-URL text is searched. |
| `reload` | Reload the page and wait for it. |
| `read_page` | Accessibility-tree-like outline: role, accessible name, state, viewport bounding box, and a ref such as `ref_12`. `filter: "interactive"` lists only controls. `max_chars` caps the output (default 40000). |
| `find` | Elements matching a CSS selector, or whose text, accessible name, placeholder or value contains the query. Text inside a `<label>` resolves to its control. |
| `click` | Real mouse click on a `ref` (scrolled into view first) or at `x`/`y` (or `coordinate: [x, y]`) in viewport pixels; optional `modifiers`. Waits for any navigation to settle. |
| `double_click` | Two clicks (with word selection, as a real double click) and a `dblclick` event. |
| `hover` | Moves the pointer over a `ref` or point: hover styles and mouse events fire. |
| `type` | Inserts `text` into the focused element, or clicks `ref` first. `\n` presses Enter. |
| `key` | Key presses and shortcuts, space separated: `Enter`, `Tab`, `shift+Tab`, `ctrl+a`, `ArrowDown`, `PageDown`, `Escape`, `F5`, single characters. `repeat` repeats the sequence. |
| `scroll` | Mouse-wheel scroll `up`/`down`/`left`/`right` by `amount` ticks of 100 px at a ref, a point, or the viewport centre. A `ref` without a direction scrolls it into view. |
| `form_input` | Sets an input, textarea, select (by value or label), checkbox/radio (`true`/`false`) or contenteditable, firing `input` and `change`. |
| `screenshot` | PNG of the tab's viewport as MCP image content. One image pixel is one CSS pixel, so screenshot coordinates are what `click`, `hover` and `scroll` take. |
| `javascript` | Evaluates `code` in the page's global scope and returns the last expression's value (objects as JSON, elements as HTML). A returned Promise is awaited for up to 30 s. |
| `get_page_text` | Title, URL and the body's rendered text. |
| `wait` | Sleeps for `ms` milliseconds (at most 30000). |

Refs are assigned per document and stay stable while it lives; after a
navigation call `read_page` or `find` again.

## How it works

- **`src/gtk/remotectl.c`** runs inside the GUI. When allowed it binds a
  TCP listener to `127.0.0.1` on a random port and writes
  `remote-control.json` (port, a 256-bit random token from OpenSSL's
  `RAND_bytes`, pid, version) to the user state directory
  (`g_get_user_state_dir()`: `~/.local/state/nordstjernen/` on Linux and
  macOS, `%LOCALAPPDATA%\nordstjernen\` on Windows) with mode `0600`. That
  directory is outside every path the sandboxed renderers may read or
  write, so a compromised page cannot learn the token or plant a forged
  file; the `--mcp` client also refuses a file that is not a regular file
  owned by the user with no group/other permission bits. The protocol is
  newline-delimited JSON; every request carries the token. Besides the
  tab, navigation, input, `eval` and `screenshot` commands the MCP tools
  are built on, a `console` command drains the tab's console log (page
  and worker `console.*` output, uncaught errors and unhandled
  rejections, resource errors) as `{"text": ...}` — the quickest way to
  see why a site's script or WebAssembly module stopped. It drives tabs through `procwindow.c` and
  pages through the same `NsProcView` input path the mouse and keyboard
  use (`ns_proc_view_remote_*` in `procview.c`), so clicks hit-test in the
  renderer, focus moves, and links navigate exactly as for the user.
- **`src/gtk/mcp.c`** is the `--mcp` stdio server: JSON-RPC 2.0, one
  message per line, MCP protocol `2025-06-18` (also answers clients that
  ask for `2025-03-26` or `2024-11-05`). It implements `initialize`,
  `notifications/initialized`, `ping`, `tools/list` and `tools/call`, and
  keeps one connection to the browser open for the session.
- **`data/js/remote-page.js`** is the page-side helper `read_page`,
  `find`, `form_input`, `get_page_text` and `javascript` evaluate through
  the renderer's console-eval path. It stores itself under
  `Symbol.for('nordstjernen.remote')`.
- **`src/gtk/rjson.c`** is the small JSON parser/writer both sides use.

## Security model

- **Off by default.** No socket exists unless the Settings switch is on
  or the browser was started with `--remote-control`. Turning the switch
  off closes the listener and every connection and deletes the control
  file.
- **Loopback only.** The listener binds `127.0.0.1`, never a public
  interface.
- **Token per session.** A fresh random token is minted each time the
  channel opens. It is stored only in the control file, which only your
  user account can read. A connection that does not present the token in
  its first request within 10 seconds is dropped; a wrong token closes
  the connection. At most four connections are accepted at a time.
- **Visible.** The toolbar badge is shown while any authenticated client
  is connected, and clicking it ends remote control.
- **What a connected client can do** is what you can do at the keyboard:
  open, read and close tabs, type, click, run JavaScript in pages, and
  read page content, including pages you are logged in to. Only allow
  remote control for agents you trust, and stop it when you are done.
  Any program running as your user can read the control file, just as it
  could read your cookies.
- Web content cannot reach the channel or change the setting:
  `about:settings` only answers requests from the browser chrome.

## Limitations

- `screenshot` captures the viewport of a tab that is visible in its
  window; a minimised window cannot be captured.
- `type` inserts text the way an input method does: `input` events fire
  but no `keydown` per character. Use `key` for key events.
- File uploads, downloads dialogs, native popups and permission prompts
  are not driven.

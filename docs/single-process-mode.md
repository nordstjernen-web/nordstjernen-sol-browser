# Single-tab mode (`--single-process`)

Nordstjernen's default architecture is **process-per-tab**: each tab's
engine runs in its own sandboxed `nordstjernen-renderer` process, and
the GTK shell is a thin client that blits a shared-memory framebuffer
and forwards input over an HTTP/JSON IPC control channel (see
[`tab-isolation.md`](tab-isolation.md)).

Passing **`--single-process`** to the desktop shell limits the browser to
one tab and therefore to one renderer process:

```sh
./builddir/src/gtk/nordstjernen --single-process [URL]
```

Setting `NS_SINGLE_PROCESS=1` in the environment does the same (any
value other than `0` enables the mode). Multiprocess stays the
default; nothing changes unless the flag or the variable is given.

In this mode new tabs are disabled: the new-tab button is hidden, the
new-tab and new-private-tab actions are off, *Open Link in New Tab* is
not offered, and links or popups that would open a tab load in the one
tab instead.

## How it works

The one tab is served exactly like any tab in the default mode: the
shell spawns a sandboxed `nordstjernen-renderer` for it, shares the
framebuffer with it, and talks to it over the same IPC protocol. The
shell never runs the engine itself, so the renderer sandbox, crash
containment (a renderer crash restarts the tab, not the browser) and
the watchdog's ordinary 60-second hang timeout all apply unchanged.

The mode used to host the engine inside the shell process. It no longer
does: the GTK shell does not run page content in any mode. The
in-process host (`src/rproc_inproc.c` over `src/renderer_serve.c`) is
still used, inside the renderer executable, by the headless driver.

## When to use it

- **Low-memory machines and containers**: there is never more than one
  renderer process.
- **Kiosk-like or embedded use** where a single page should be shown and
  the user should not open more tabs.

The task manager (Shift+Esc) shows the one renderer process like any
other tab's.

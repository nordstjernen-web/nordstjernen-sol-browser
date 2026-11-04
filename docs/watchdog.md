# Watchdog supervisor

Nordstjernen supervises itself. **The watchdog is on by default** (GTK):
a normal launch turns the process you start into a small supervisor that
spawns the real **GUI shell** as a child, watches it, and restarts it if
it crashes or hangs. The shell is thin and engine-free, so this guards
the *UI* process; each tab's untrusted engine already runs in its own
sandboxed renderer process, and the shell restarts a dead renderer
per tab (see [tab-isolation.md](tab-isolation.md)). The two layers compose: a renderer
crash is contained to its tab, and a crash/hang of the shell itself is
caught by the supervisor.

```sh
nordstjernen                      # supervised
nordstjernen https://example.com/ # supervised
```

All arguments are forwarded unchanged to the child, so the supervisor is
transparent.

## Disabling it

Three ways, highest priority first:

1. **Command line** (one launch): `--no-watchdog` runs the shell
   directly with no supervisor.

   ```sh
   nordstjernen --no-watchdog
   ```

2. **Environment** (one shell): `NS_NO_WATCHDOG=1` does the same.

3. **Config file** (persistent): set `watchdog_enabled = false` in
   `nordstjernen.conf`.

`--watchdog` forces supervision on even when the config disables it.
One-shot and tooling modes are never supervised regardless of the
setting: `--headless`, `--print-config`, `--dump=…`, `--eval=…`,
`--inspect=…`, and `--inspect-at=…`.

## How it works

The design mirrors how real browsers stay alive, split across the same
two roles they use:

1. **A parent supervisor restarts crashed children** — like Chrome's
   privileged browser process respawning a renderer, or Firefox
   respawning a content process. Our supervisor spawns the browser, waits
   on it with `g_child_watch`, and is notified the instant it exits. A
   clean exit (status 0 — you closed the last window) means "the user is
   done": the supervisor exits 0 and does *not* restart. So does a child
   killed by a termination signal (`SIGTERM`, `SIGINT`, `SIGHUP`,
   `SIGQUIT`, `SIGKILL`), which is taken as a deliberate stop. Any other
   abnormal exit — non-zero status, or a fatal signal such as `SIGSEGV`
   or `SIGABRT` — is a crash, and triggers a restart.

2. **An in-process thread catches hangs** — like Firefox's
   `BackgroundHangMonitor` and SpiderMonkey's watchdog thread, or
   Chrome's GPU watchdog. The shell bumps a counter from inside its GTK
   main loop every couple of seconds; a dedicated watchdog thread (which
   keeps running even when the main loop is wedged) watches that counter,
   and if it stops advancing for longer than the hang timeout it calls
   `_Exit(70)` (`NS_WATCHDOG_HANG_EXIT` in `src/watchdog.c`). The hang
   thus *becomes a non-zero exit*, which the supervisor in role 1 treats
   as a crash and restarts — so there is exactly one restart path, not
   two.

The hang timeout is **60 s** of no heartbeat (`NS_WATCHDOG_HANG_MIN_SECS`).
Because page JavaScript runs in the renderer processes — not in the
shell's main loop — the shell loop is never legitimately blocked by a long
synchronous script; the watchdog is the backstop for *native* deadlocks in
the shell (a wedged blit or a stuck IPC call), in every mode. (Runaway
scripts are interrupted separately, in-engine, at the JS budget; see
`src/js.c`.)

There is no heartbeat file and no polling: the beat lives in process
memory, so nothing touches the filesystem on the hot path and there is no
world-writable temp file to harden.

## Crash-loop protection

A browser that crashes immediately on every launch would otherwise spin
forever. If the child crashes more than five times within 60 seconds the
supervisor gives up and exits non-zero instead of restarting again.
Successful restarts are spaced by a one-second backoff.

## Crash recovery

A silent relaunch to a blank page would throw away your work, so the
supervisor restores it — the way Chrome and Firefox reopen your tabs after
an "Aw, Snap!" / `about:tabcrashed`.

Tabs are not kept in a watchdog file. The shell writes every tab (its
back/forward list with each entry's title and scroll position, and the
tab's `sessionStorage`) to the profile's `session.db` as it changes
(`src/gtk/sessionstore.c`, synced each second by `session_sync_cb` in
`src/gtk/procwindow.c`), so nothing has to be saved at exit and a crash
loses at most the last second. Each running shell owns its rows through
an instance id and refreshes a heartbeat in the same database.

The supervisor hands the child a fresh instance id with
`--watchdog-session=<id>` and reuses it for every restart. When a crash or
hang triggers a restart, it also sets `NS_WATCHDOG_RECOVER=1`; the
respawned child, carrying the same id, reopens exactly the tabs the
crashed one owned and shows a status-bar note that it recovered after an
unexpected exit. A normal launch without a URL reopens the tabs of every
shell that is no longer running (no heartbeat for 15 seconds, or a clean
exit); a launch with a URL opens just that URL and leaves the stored tabs
for the next plain launch. Private windows never touch the database.
`about:` pages are not stored, so a window that had only `about:start`
simply opens the home page.

## Shutting it down

On Unix, sending the supervisor `SIGINT` (Ctrl-C) or `SIGTERM` sends the
child `SIGTERM`, escalates to `SIGKILL` if it has not exited after three
seconds, and then exits cleanly without restarting it. On Linux the child
also asks for `SIGTERM` when the supervisor itself dies
(`PR_SET_PDEATHSIG`), so killing the supervisor does not orphan the shell.

## Notes

- The supervisor is a deliberately tiny process: it reads the config to
  learn whether it is wanted, then only spawns, watches, and restarts.
  It does not initialise the network stack, the sandbox, the seccomp
  filter, or any UI — those belong to the child.
- The child is launched with the internal `--watchdog-child` and
  `--watchdog-session=<id>` flags, and recovery is signalled with the
  `NS_WATCHDOG_RECOVER` environment variable; you never set these
  yourself.

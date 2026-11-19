# Nordstjernen Security FAQ

This policy covers the three browsers of the Nordstjernen project. They
share most of one engine, so a bug found in one often affects the others:

- **Nordstjernen Sol**: the full browser, with tabs in sandboxed renderer
  processes ([nordstjernen-sol-browser](https://github.com/nordstjernen-web/nordstjernen-sol-browser)).
- **Northstar Web Browser**: the minimalist edition, with one process and
  one page per window ([northstar-web-browser](https://github.com/nordstjernen-web/northstar-web-browser)).
- **Southstar**: the browser rewritten in Rust
  ([southstar-browser](https://github.com/nordstjernen-web/southstar-browser)).

## I want to report a security bug!

E-mail [andreas.rosdal@hotmail.com](mailto:andreas.rosdal@hotmail.com).
Please do not open a public issue or pull request for a vulnerability.

Include:

- which browser you found it in, and its version (from the About dialog);
- your operating system;
- a minimal reproducer: a URL or a self-contained HTML file;
- your assessment of the impact.

You only need to report a bug once. Say which of the three browsers you
tested; the others are checked and fixed too.

## Which versions get security fixes?

Fixes land on the `main` branch of each repository. Only the latest
release of each browser is supported.

## What counts as a security bug?

Anything that lets a web page, or the network, get past what the browser
promises. For example:

- memory-safety bugs in the project's own code, including the projects it
  has forked into its tree, and panics a web page can trigger;
- escapes from the browser's sandbox (Landlock and seccomp on Linux,
  Seatbelt on macOS, the process mitigations on Windows);
- bypasses of the same-origin policy, CORS, or cookie or cache
  partitioning;
- bypasses of HSTS, mixed-content blocking, CSP or iframe sandboxing;
- URL-bar spoofing;
- a web page reaching one of the browser's local control interfaces, such
  as remote control or WebDriver.

Not in scope:

- bugs in third-party libraries a browser uses unmodified, such as GTK,
  GLib, FFmpeg, FreeType or SQLite; please report those upstream;
- features a browser does not implement;
- Spectre-class side channels;
- attacks that already need local code execution as the user;
- anything that needs a debugging switch such as `NS_NO_SANDBOX`.

Each browser describes its threat model, sandbox and known gaps in its own
`docs/security-model.md`:
[Nordstjernen Sol](https://github.com/nordstjernen-web/nordstjernen-sol-browser/blob/main/docs/security-model.md),
[Northstar](https://github.com/nordstjernen-web/northstar-web-browser/blob/main/docs/security-model.md),
[Southstar](https://github.com/nordstjernen-web/southstar-browser/blob/main/docs/security-model.md).

## How are fixes published?

Each fix is committed with a `Security-Fix:` trailer naming the bug, and
the same name is used when the fix is ported to the other two browsers.
User-visible fixes are listed in each repository's changelog.

## I have a question! Who can help?

Questions about security, or about a report you have sent, go to the same
address: [andreas.rosdal@hotmail.com](mailto:andreas.rosdal@hotmail.com).

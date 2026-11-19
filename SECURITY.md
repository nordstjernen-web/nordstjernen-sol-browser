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

- memory-safety bugs in the project's own code, including its forks of
  QuickJS and Pango and its Rust HTML and URL parsers;
- escapes from a renderer sandbox, where the browser has one;
- bypasses of the same-origin policy, or of cookie or cache partitioning;
- bypasses of HSTS, mixed-content blocking, CSP or iframe sandboxing;
- URL-bar spoofing.

Not in scope:

- bugs in third-party libraries the project uses unmodified, such as
  libcurl, GTK, GLib, Wuffs, libwebp, FFmpeg or SQLite; please report
  those upstream;
- features a browser does not implement;
- Spectre-class side channels;
- attacks that already need local code execution as the user.

Nordstjernen Sol's threat model, sandbox and defences are described in
[docs/security-model.md](docs/security-model.md). Northstar and Southstar
document theirs in their own repositories.

## Do you have a bounty program?

No. Nordstjernen is a small independent project and pays no bounties.
Reporters are credited in the changelog.

## How are fixes published?

Each fix is committed with a `Security-Fix:` trailer naming the bug, and
the same name is used when the fix is ported to the other two browsers.
User-visible fixes are listed in each repository's changelog.

## I have a question! Who can help?

Questions about security, or about a report you have sent, go to the same
address: [andreas.rosdal@hotmail.com](mailto:andreas.rosdal@hotmail.com).

# pip package: nordstjernen-browser

`pip/` is the [`nordstjernen-browser`](https://pypi.org/project/nordstjernen-browser/)
package: `pip install nordstjernen-browser`, then `nordstjernen <url>`. It is
the Python counterpart of the [npm package](npm.md) and installs the browser
the same way:

| System | Source |
| --- | --- |
| Linux, BSD, Intel macOS | The release tag's source tarball, checked against the SHA-256 that `prepare.py` recorded in `source.json`, built with meson against the system libraries and installed with `meson install`. A preflight check first names every missing or too-old build tool and library and the command that installs it. |
| macOS arm64 | The release's notarised `.dmg`, verified against `SHA256SUMS` and copied out with `ditto`. |
| Windows | The same source build with MSYS2's programs; the environment's `bin` is recorded in `runtime.json` so the launcher puts the GTK and FFmpeg DLLs on `PATH`. |

pip has no install hook, so the wheel carries only the launcher: the first
`nordstjernen` run (or `nordstjernen-install`) builds or copies the browser
into the user data directory (`nordstjernen-install --where`;
`NORDSTJERNEN_INSTALL_DIR` overrides it), and `nordstjernen-install --remove`
deletes it. The build log goes to `build-<version>.log` in the user cache
directory. The package is pure Python (`py3-none-any`) and has no
dependencies.

`src/nordstjernen_browser/product.py` mirrors `npm/lib/product.js`: the
tools and libraries with their package names, and the library floors from
`meson.build`. Keep the two in step.

## Publishing

`.github/workflows/pypi.yml` publishes when a GitHub release is published
(or by hand for a tag): `prepare.py <tag>` checks that the tag builds that
version, writes the version into the package, records the source tarball's
SHA-256 and copies the licence files; the sdist and wheel are built and
checked with `twine check --strict`; and they are uploaded through PyPI
[Trusted Publishing](https://docs.pypi.org/trusted-publishers/), so no API
token is stored. The workflow exchanges the job's OIDC identity for a
short-lived upload token itself instead of using a third-party action.

One-time setup on pypi.org, under *Your account → Publishing*: add a
(pending) GitHub publisher with project name `nordstjernen-browser`, owner
`nordstjernen-web`, repository `nordstjernen-sol-browser`, workflow
`pypi.yml` and environment `pypi`.

A published version cannot be replaced. To fix a broken package, publish
the next release, or yank the version on PyPI.

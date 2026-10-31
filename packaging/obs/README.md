# openSUSE / OBS packaging

The OBS package is built **directly from this git repo** via the
obs-scm-bridge (`scmsync`). Nothing is uploaded to OBS by hand and no
source tarball is stored anywhere — the bridge clones the repo, and the
build reconstructs the source tarball in the build VM.

The build recipe therefore lives at the **repo root**, where the bridge
can find it:

- `/nordstjernen.spec` — RPM build recipe (meson build). It carries no
  `Source0`: its `%prep` locates the tree the bridge laid down under
  `%{_sourcedir}` and copies it into the build directory.
- `/nordstjernen.changes` — the openSUSE changelog. Factory requires it;
  add an entry for each submitted version.

There is no `_service` file. The source comes from the scmsync checkout
alone.

## One-time OBS setup

The package must be named lowercase **`nordstjernen`** (so the
bridge-generated tarball is `nordstjernen-<version>` and matches the
spec's `%prep`). Create it if needed, then bind it to git:

    osc meta pkg home:andreasrosdal nordstjernen -e

and add inside `<package>`:

    <scmsync>https://github.com/nordstjernen-web/nordstjernen-sol-browser?trackingbranch=main</scmsync>

That is the whole setup. After it, git is authoritative: edit the spec in
this repo and push — never touch files in the OBS web UI.
OBS follows `main` and rebuilds when it advances; for instant rebuilds add
a git webhook backed by `osc token --create --operation runservice`.

## Why not a `_service` that fetches from git

build.opensuse.org does not run network-fetching source services
(`tar_scm` / `obs_scm`) on its source server for this package — they
produce no archive, so the chain dies at `recompress`
("no such file … nordstjernen-*.tar") or at the buildtime `tar`
("no .obsinfo file found"). The scmsync bridge is a separate, working
code path for cloning git, so the source comes from there and the spec
builds it in place.

## License

Nordstjernen is dual-licensed under the **Nordstjernen Source License
v1.0 (NSL-1.0)** or the **GNU General Public License version 3 or later
(GPL-3.0-or-later)**, at the recipient's option, so the spec tags it
`LicenseRef-NSL-1.0 OR GPL-3.0-or-later` and ships `License.md` and `COPYING`
as `%license`. NSL-1.0 alone is not OSI-approved (it forbids "Competing
Use"), but the GPL option is free software, which makes the package
eligible for openSUSE:Factory / Tumbleweed through the usual devel-project
submit request and legal review. Until it is accepted there, the home:
project is the distribution channel. See `docs/opensuse.md`.

## Build options

The spec disables one meson feature unavailable in OBS:

- `-Dwebgpu=disabled` — needs external wgpu-native.

Everything else (GTK shell, sandboxed renderer, SDL2 audio helper, WebGL,
enchant spell-check) builds from the declared `BuildRequires`.

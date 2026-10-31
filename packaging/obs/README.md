# openSUSE / OBS packaging

The OBS package `home:andreasrosdal/nordstjernen` is a classic OBS
package built from a release tarball, the form openSUSE:Factory
requires. Its sources are three files:

- `nordstjernen.spec` — the RPM build recipe, kept at the repo root.
  `Source0` is the GitHub archive of the release tag.
- `nordstjernen.changes` — the openSUSE changelog, kept at the repo
  root. Factory requires it; add an entry for each version.
- `nordstjernen-<version>.tar.gz` — the GitHub tag archive, downloaded
  from the `Source0` URL. Never committed to git.

The repo copies of the spec and changes file are authoritative; OBS
gets copies of them.

## Releasing a new version

After tagging `<version>` on GitHub, bump `Version:` in
`nordstjernen.spec`, add a `nordstjernen.changes` entry, merge, then:

    osc co home:andreasrosdal nordstjernen && cd home:andreasrosdal/nordstjernen
    cp ~/path/to/repo/nordstjernen.spec ~/path/to/repo/nordstjernen.changes .
    osc rm nordstjernen-<old>.tar.gz
    curl -Lo nordstjernen-<version>.tar.gz \
        https://github.com/nordstjernen-web/nordstjernen-sol-browser/archive/refs/tags/<version>.tar.gz
    osc add nordstjernen-<version>.tar.gz
    osc ci -m "Update to <version>"

Once the builds succeed, submit to the devel project:

    osc sr home:andreasrosdal nordstjernen network

## Path to openSUSE:Factory

New Factory packages come through a devel project. Nordstjernen's is
`network`: submit `home:andreasrosdal/nordstjernen` to
`network/nordstjernen`, and once the `network` maintainers accept it,
they (or the package maintainer) forward it to `openSUSE:Factory`. The
source and target package names must match — always lowercase
`nordstjernen`.

## License

Nordstjernen is dual-licensed under the **Nordstjernen Source License
v1.0 (NSL-1.0)** or the **GNU General Public License version 3 or later
(GPL-3.0-or-later)**, at the recipient's option, so the spec tags it
`LicenseRef-NSL-1.0 OR GPL-3.0-or-later` and ships `License.md` and
`COPYING` as `%license`. The GPL option is free software, which makes
the package eligible for openSUSE:Factory proper, not NonFree. See
`docs/opensuse.md`.

## Build options

openSUSE's `%meson` passes `--auto-features=enabled`, so every `auto`
feature in `meson_options.txt` must have its dependency declared in
`BuildRequires`, or `meson setup` fails. The spec disables one feature
unavailable in OBS:

- `-Dwebgpu=disabled` — needs external wgpu-native.

The package is built for 64-bit targets only (`ExcludeArch: %ix86
%arm`): the Temporal code in the in-tree QuickJS uses `__int128`.

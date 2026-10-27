# npm package: nordstjernen-browser

`npm/` is the [`nordstjernen-browser`](https://www.npmjs.com/package/nordstjernen-browser)
package. It carries no binaries: the `postinstall` script (or the first
`nordstjernen` launch, when install scripts are skipped) puts the browser in
place for the user's system.

| System | Source |
| --- | --- |
| Linux, BSD, Intel macOS | The release tag's source tarball, checked against the SHA-256 that `prepack` recorded in `lib/source.json`, built with meson against the system libraries and installed into `vendor/<version>/` with `meson install`. Before downloading, a preflight check names every missing or too-old build tool and library; the library floors mirror `meson.build` and `src/web-text-lib/meson.build` and must be kept in step with them. The build log goes to `~/.cache/nordstjernen-browser/build-<version>.log`, and progress is written to `/dev/tty` because npm hides install-script output. A prebuilt binary is not offered because the Linux zip links the distribution's own GTK 4, FFmpeg and libavif. |
| macOS arm64 | The release's notarised `.dmg`, verified against `SHA256SUMS` and copied out with `ditto`. |
| Windows | The same source build, run with MSYS2's UCRT64 (or MINGW64) programs: `lib/system.js` finds MSYS2 (`MSYS2_ROOT`, `C:\msys64`), builds with its clang, and records the environment's `bin` in `vendor/<version>/npm-runtime.json` so the launcher puts it on `PATH` for the GTK and FFmpeg DLLs. The build log goes to `%LOCALAPPDATA%\nordstjernen-browser`. The binary is unsigned, so Smart App Control blocks it; the Microsoft Store build remains the signed one. |

`lib/product.js` is the only file specific to Nordstjernen: the release
repository, the executable, the meson options, the macOS release asset, the
build tools and the required and optional libraries with their package names.
`install.js`, `system.js`, `dependencies.js`, `postinstall.js` and
`prepack.js` are the same files in the Northstar and Southstar packages, so
fix them in all three. On Unix the installer sets the executable bits on
everything in the install's `bin/` and `libexec/`, including installs made by
an older package version.

## Publishing a release

The npm version must name a published GitHub release, because the
installer downloads that release's assets. Publish only after the release is
public.

1. Set `"version"` in `npm/package.json` to the release tag, for example
   `1.0.37`. `prepack` refuses a version with no matching tag, and downloads
   the tag's source tarball to record its SHA-256, so publish only after
   the tag is pushed.
2. From `npm/`, check the package contents with `npm pack --dry-run`.
3. Run `npm publish` (add `--otp=<code>` if two-factor authentication asks
   for it).

A published version cannot be republished. To fix a broken package, publish
the next version, or run `npm deprecate nordstjernen-browser@<version> "<reason>"`.

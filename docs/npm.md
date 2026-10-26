# npm package: nordstjernen-browser

`npm/` is the [`nordstjernen-browser`](https://www.npmjs.com/package/nordstjernen-browser)
package. It carries no binaries: the `postinstall` script (or the first
`nordstjernen` launch, when install scripts are skipped) puts the browser in
place for the user's system.

| System | Source |
| --- | --- |
| Linux, BSD, Intel macOS | The release tag's source tarball, built with meson against the system libraries and installed into `vendor/<version>/` with `meson install`. A prebuilt binary is not offered because the Linux zip links the distribution's own GTK 4, FFmpeg and libavif. |
| macOS arm64 | The release's notarised `.dmg`, verified against `SHA256SUMS` and copied out with `ditto`. |
| Windows | None. Unsigned executables are blocked by Smart App Control, so the command opens the Microsoft Store page instead. |

## Publishing a release

The npm version must name a published GitHub release, because the
installer downloads that release's assets. Publish only after the release is
public.

1. Set `"version"` in `npm/package.json` to the release tag, for example
   `1.0.37`. `prepack` refuses a version with no matching tag.
2. From `npm/`, check the package contents with `npm pack --dry-run`.
3. Run `npm publish` (add `--otp=<code>` if two-factor authentication asks
   for it).

A published version cannot be republished. To fix a broken package, publish
the next version, or run `npm deprecate nordstjernen-browser@<version> "<reason>"`.

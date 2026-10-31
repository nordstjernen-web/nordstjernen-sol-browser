# Media: video and audio

How Nordstjernen plays `<video>` and `<audio>`. The engine ships a tiny,
in-tree decoder set plus one FFmpeg-backed WebM extension, and hands H.264
video and AAC audio to the operating system's own decoders; anything else
renders a poster and a play overlay.

Two processes share the work: the sandboxed renderer parses and paints the
page and decodes every video frame, MSE streams included, and the
`nordstjernen-audio` helper plays sound.

## Inline video

### MPEG-1 (always on)

`<video>` plays **inline** when the source is MPEG-1 (an `.mpg` / `.mpeg` /
`.m1v` stream). The bytes are decoded entirely in-tree by
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) — a single-file, MIT-licensed
MPEG-1 video decoder — inside the sandboxed renderer process; no external
library, no GPU API, and no syscalls beyond memory are needed, so it runs
comfortably under the seccomp filter. Decoded BGRA frames are advanced off the
renderer's existing animation tick, honouring the `autoplay`, `loop`, `muted`,
`width`/`height`, and `poster` attributes; a click toggles play/pause; and the
`HTMLMediaElement` events (`loadedmetadata`, `durationchange`, `canplay`,
`timeupdate`, `play`, `pause`, `ended`) fire on the element as it plays. MPEG-1
is the one always-on codec, by design — small, patent-free, and decoded in pure
portable C.

### WebM — VP9/VP8 (required on Linux and Windows)

`.webm` plays inline too when **FFmpeg's libav\*** (`libavformat` /
`libavcodec` / `libavutil` / `libswscale` / `libswresample`) is present at build
time (`-DNS_HAVE_LIBAV`). These are the component libraries of
[FFmpeg](https://github.com/FFmpeg/FFmpeg) (canonical git
<https://git.ffmpeg.org/ffmpeg.git>) — an actively maintained project, **not**
the defunct [Libav](https://github.com/libav/libav) fork (last release 2018),
whose `libav.org` domain is unrelated despite the shared library-name prefix.
libav demuxes the Matroska container
and decodes **VP9/VP8** frames, which are scaled to BGRA and served through the
same off-tick frame loop as the MPEG-1 path — both royalty-free codecs. It is
**required on Linux and Windows** (`meson setup` fails without it, or with a
pre-6.0 FFmpeg) because YouTube and most modern sites serve VP9/WebM and the
poster-and-overlay fallback there is unacceptable; on **macOS** it is
auto-detected, and a build without the FFmpeg libraries carries no libav symbol
or dependency and falls back to the poster-and-overlay path. The minimum is
**FFmpeg 6.0** (library sonames libavcodec ≥ 60, libavformat ≥ 60, libavutil ≥
58, libswscale ≥ 7, libswresample ≥ 4) — the oldest release carrying the
`AVChannelLayout` API this code uses.

How libav is obtained differs by platform, to keep it redistributable:

- **Linux** depends on the distribution's FFmpeg at runtime (never bundled), so
  no licensing obligation falls on the package. openSUSE Tumbleweed's own
  `libavcodec` (no Packman needed) decodes every codec YouTube serves: native
  VP9 and Opus, AV1 through `libdav1d`, AAC natively and H.264 through Cisco's
  `libopenh264` (the `libopenh264-8` package from the Cisco repository). For
  the GStreamer H.264/AAC path build with `gstreamer-devel
  gstreamer-plugins-base-devel` and install a decoder plugin at runtime:
  `gstreamer-plugins-bad` (VA-API `vah264dec`/`vavp9dec`/`vaav1dec`, and
  `openh264dec` against the Cisco library) or `gstreamer-plugins-libav`, plus
  `intel-media-driver` on Intel GPUs for hardware decoding. Cisco's own
  `gstreamer-plugin-openh264` package may lag behind its `libopenh264` and is
  not needed.
- **macOS / Windows** vendor a **minimal, LGPL-only FFmpeg** built from source
  (`scripts/build-ffmpeg-lgpl.sh`), carrying just the matroska/ogg demuxers and
  the native VP8/VP9/Theora/Opus/Vorbis decoders — no GPL parts, no external codec
  libraries.
- **Android / iOS** do not cross-build FFmpeg, so the inline libav WebM path is
  unavailable there; MP4/H.264 plays through the in-tree demuxer below.

### MP4 — H.264 and AAC through the operating system

H.264 video and AAC audio, the codecs of most MP4 files, HLS streams and
non-WebM MSE players, are decoded by the **operating system**, never by a
decoder shipped in the browser. The vendor of the OS already licenses these
codecs, so the browser carries no patent-encumbered decoder, pays no codec
licence and stays small. libav still demuxes the MP4 container (the LGPL
FFmpeg that `scripts/build-ffmpeg-lgpl.sh` builds for the macOS and Windows
packages has the `mov` demuxer and the H.264/AAC parsers, but no H.264 or AAC
decoder); the compressed frames then go through `src/media_os.h`:

| Platform | Backend | File |
|---|---|---|
| Windows | Media Foundation transforms (`IMFTransform`, H.264 and AAC decoder MFTs) | `src/media_os_mf.c` |
| macOS, iOS | VideoToolbox (`VTDecompressionSession`) and AudioToolbox (`AudioConverter`) | `src/media_os_vt.c` |
| Android | NDK MediaCodec (`AMediaCodec`) | `src/media_os_android.c` |
| Linux, BSDs | GStreamer (`decodebin`), which picks VA-API, V4L2 or an installed software decoder | `src/media_os_gst.c` |

Builds without libav (Android, iOS, and macOS without FFmpeg) demux MP4 with
a small in-tree parser instead (`src/mp4.c`: `moov` sample tables, edit
lists, and `moof`/`traf`/`trun` fragments as MSE appends them) and play it
through `src/video_mp4.c`, which feeds the same OS decoder, seeks to the
keyframe before the target and converts frames to BGRA in plain C. Those
builds play the video picture of MP4 and H.264 MSE streams; their audio has
no MP4 path yet, since the audio helper's container support comes from libav.

GStreamer is required on Linux and auto-detected on the BSDs
(`-Dgstreamer=disabled` drops it); the other
backends are always built on their platforms. A backend returns NV12 or I420
frames; the renderer converts only the frame it is about to show to BGRA
(through swscale), so frames skipped while seeking forward from a keyframe
cost no conversion. Video decodes in the sandboxed renderer and AAC in the
`nordstjernen-audio` helper. Media Foundation is loaded at runtime and
initialised before the renderer applies its process mitigation policies, so
Windows N editions without the Media Feature Pack still start (H.264 then
reports unsupported); GStreamer and its decoder plugins are loaded before the
Linux sandbox engages.

`canPlayType()` and `MediaSource.isTypeSupported()` report `avc1`/`mp4a` as
supported when the OS decoder is present. If an OS decoder cannot be created,
or fails before its first frame, the stream falls back to libavcodec where
that has an H.264 decoder (a distribution FFmpeg usually does). Two
environment variables help compare the paths: `NS_MEDIA_NO_OS_DECODER=1`
skips the OS decoders, and `NS_MEDIA_OS_CODECS_ONLY=1` makes
`isTypeSupported()` reject the video codecs only libav decodes (VP8/VP9/AV1),
so a site such as YouTube serves H.264.

YouTube itself prefers VP9 or AV1 when the browser accepts them, so on Linux
and Windows (where libav decodes those) it keeps using them; the OS H.264
path is what plays MP4/HLS video elsewhere on the web, and YouTube on builds
without libav's VP9/AV1. Encrypted media (EME/Widevine) is not supported.

### Hardware VP9, AV1 and HEVC through GStreamer

On GStreamer builds, VP9, AV1 and HEVC also go to GStreamer when it has a
**hardware** decoder for them (an element whose class names `Hardware`, such
as VA-API's `vavp9dec`, `vaav1dec` and `vah265dec`). The pipeline names that
element explicitly after its parser (`vp9parse`, `av1parse`, `h265parse`)
rather than letting `decodebin` choose, so a higher-ranked software decoder
never takes over. Without a hardware decoder those codecs stay on libav
(`libdav1d` for AV1), and a hardware pipeline that fails before its first
frame falls back to libav as H.264 does. HEVC (`hvc1`/`hev1`) is reported as
playable when a hardware decoder exists, since distribution FFmpeg builds
often lack an HEVC decoder.

VA-API needs the GPU's driver: `intel-media-driver` for Intel Broadwell and
newer, Mesa's `libva` drivers for AMD. `NS_MEDIA_NO_HW_DECODER=1` keeps these
codecs on libav, and `NS_MEDIA_GST_SOFTWARE_DECODER=1` lets the path accept
a software GStreamer decoder, which exercises it on machines without a
hardware decoder.

## Audio

The MPEG-1 stream's **MP2 audio track** plays too (unless the element is
`muted`), as does a WebM's **Opus/Vorbis** track when libav is built in. The
seccomp-sandboxed renderer can't open a sound device, so audio is handed to the
unsandboxed `nordstjernen-audio` helper. The helper decodes to PCM — pl_mpeg for
the MPEG-1/MP2 track, [minimp3](https://github.com/lieff/minimp3) (CC0, vendored)
for standalone `.mp3` files, libav for Opus/Vorbis (WebM/Ogg), and the
operating system's AAC decoder for MP4 audio — and plays it
through [SDL2](https://www.libsdl.org/)'s audio device (WASAPI on Windows,
CoreAudio on macOS, ALSA/PulseAudio on Linux), mixing and resampling the streams
itself. The inline player drives the helper — `open`/`play`/`pause`/`seek`/
`stop`/`loop`/`volume` commands ride the render response's `X-Audio`
side-channel to the shell, which spawns and pumps the helper
(`src/gtk/procview.c`), and looping re-syncs the audio at each wrap.

## Captions and subtitles (`<track>`)

A `<video>` may carry a `<track>` child pointing at a
[WebVTT](https://www.w3.org/TR/webvtt1/) file. When one is marked `default`
and its `kind` is `subtitles`/`captions` (or omitted — the missing-value
default), the renderer fetches it during video discovery
(`ns_video_discover_track` in `src/video.c`) and parses it into timed cues
(`ns_vtt_parse`): it reads `[HH:]MM:SS.mmm` start/end timestamps, skips the
`WEBVTT` header, `NOTE` comments, cue identifiers and the cue-setting suffix
after the end timestamp, strips inline `<…>` tags, decodes the common HTML
entities, and keeps multi-line cue text. Only the `default` track shows,
which is the spec's initial text-track mode; scripted `TextTrack.mode`
switching and cue positioning settings (`line`/`position`/`align`) are not
wired.

The cue active at the video's current time is drawn by `paint_video_caption`
(`src/paint.c`) as centred, white-on-translucent-black lines across the
bottom of the video box, right after the frame itself, so the caption pixels
land on top of the picture.

## Media Source Extensions

MSE streams (YouTube-style playback) decode in the renderer, like every other
video. `SourceBuffer.appendBuffer()` hands each segment to `src/video.c`, which
keeps the initialization segment and every media segment in memory together
with the time range a demux probe finds in it. The decoder is never fed the raw
append order. It gets a **window**: the initialization segment followed by the
contiguous run of media segments, in time order, that holds the playhead.
Segments that extend that run are appended to the live decoder as they arrive;
segments elsewhere on the timeline wait until playback reaches them.

That gives three properties:

- **Fast start.** The decoder opens on the first media segment; there is no
  byte threshold and no file to write first.
- **Seeking.** A small step forward inside the window decodes on from where the
  player is. Any other seek rebuilds the decoder at the segment holding the
  target and decodes forward from that segment's keyframe — no demuxer seek
  through a byte stream whose offsets do not match the original file. A seek
  into a range that is not buffered yet leaves the element `waiting` at the
  target time until the page appends that segment, then playback resumes
  there.
- **Accurate `buffered`.** `SourceBuffer.buffered` reports every disjoint
  range, merging segments whose gap is under half a second, so a player that
  seeks past its buffer sees the hole and fetches the right segment.
  `SourceBuffer.remove()` drops the overlapping segments; the playing decoder
  keeps its own copy of the window, so evicting behind the playhead never
  interrupts playback.

A new initialization segment (a quality switch) starts a new generation: the
current window plays on until the playhead reaches the first segment of the new
generation, and the decoder is rebuilt there.

The audio track still plays in `nordstjernen-audio`: the renderer materializes
the audio byte stream under `~/.cache/nordstjernen/msaudio/` and cues the
helper with the same play/pause/seek commands that drive the video clock.

## Other media

Beyond MPEG-1/MP2, MP3, and the optional WebM (VP9/VP8 + Opus/Vorbis) path,
Nordstjernen ships no media codecs. Other `<audio>` and other `<video>` codecs
render a poster and a play overlay instead of playing. Clicking one resolves
the source URL inside the sandboxed renderer process (`ns_browser_media_at`),
and the renderer's HTTP protocol reports it to whoever drives the renderer —
the C embedding API and the Java binding can hand it to an external player of
their own. The GTK shell itself does not launch an external player; the
`.deb` and `.rpm` packages still recommend one (`mpv`, or `ffmpeg` as the
`.deb` alternative) for opening such URLs by hand.

Streaming sites (YouTube and friends) drive `<video>` through MSE/`blob:`
with no plain file URL; those play inline through the MSE path described
above when libav is present.

## Poster / source metadata (standards, not site scraping)

For pages whose player is JS-driven and has no server-side `<video src>`,
`src/html_lexbor.c` reads **standard** video metadata during HTML parse and
annotates the target element (`data-nd-media-poster` / `data-nd-media-src` /
`data-nd-media-stream`). It reads, in priority order, JSON-LD `VideoObject`
(`contentUrl` / `thumbnailUrl` / `embedUrl`), OpenGraph (`og:video*` /
`og:image*`), and Twitter cards — never a site's private JSON. If a source URL
has an extension the engine decodes it becomes a direct `data-nd-media-src`;
otherwise (e.g. an `og:video` that is a `text/html` embed page, as YouTube
emits) it falls to the MSE stream path — a general rule, not a per-site rule.
The poster comes from `og:image`/`thumbnailUrl`. This covers YouTube, Vimeo and
ordinary `og:video` news articles with the same code. There are no
site-specific media hacks.

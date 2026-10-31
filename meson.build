project(
  'nordstjernen',
  ['c', 'cpp'],
  version: '1.0.40-dev',
  license: 'LicenseRef-NSL-1.0 OR GPL-3.0-or-later',
  default_options: [
    'c_std=gnu2x,c2x,gnu11,c11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=release',
    'optimization=3',
    'b_ndebug=true',
    'b_pie=true',
  ],
  meson_version: '>=1.0',
)

cc = meson.get_compiler('c')

common_warning_flags = [
  '-Wshadow',
  '-Wpointer-arith',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wmissing-declarations',
  '-Wold-style-definition',
  '-Wcast-align',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wformat-security',
  '-Wnull-dereference',
  '-Wlogical-op',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wredundant-decls',
]

suppressed_warnings = [
  '-Wno-constant-logical-operand',
]

fortify_level = '-D_FORTIFY_SOURCE=2'
if cc.has_argument('-D_FORTIFY_SOURCE=3')
  fortify_level = '-D_FORTIFY_SOURCE=3'
endif

hardening_c_flags = [
  '-fstack-protector-strong',
  '-fstack-clash-protection',
  '-fcf-protection=full',
  '-U_FORTIFY_SOURCE',
  fortify_level,
]

hardening_link_flags = [
  '-Wl,-z,relro',
  '-Wl,-z,now',
  '-Wl,-z,noexecstack',
  '-Wl,-z,separate-code',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags + suppressed_warnings +
                             hardening_c_flags),
  language: 'c',
)

add_project_link_arguments(
  cc.get_supported_link_arguments(hardening_link_flags),
  language: 'c',
)

add_project_link_arguments(
  cc.get_supported_link_arguments(hardening_link_flags),
  language: 'cpp',
)

ns_buildtype = get_option('buildtype')
ns_is_release = (ns_buildtype == 'release' or ns_buildtype == 'minsize')
# Every build profile gets -O3 (via default_options optimization=3) plus
# dead-section pruning. Nordstjernen leans on the optimizer to inline the
# lexbor parser walks, QuickJS interpreter dispatch, and our box-layout
# loops; -O2 left ~30% on the table for JS-heavy pages. The -O level is
# left to meson's optimization option so it isn't passed twice (which
# warns); we only add the section-GC flags here. Dev/non-release builds
# keep -g3 + frame pointers so step debugging still works under -O3.
add_project_arguments(
  cc.get_supported_arguments(['-ffunction-sections', '-fdata-sections']),
  language: 'c',
)
add_project_link_arguments(
  cc.get_supported_link_arguments(['-Wl,--gc-sections']),
  language: 'c',
)
if ns_is_release
  add_project_link_arguments(
    cc.get_supported_link_arguments(['-Wl,-s']),
    language: 'c',
  )
else
  add_project_arguments(
    cc.get_supported_arguments(['-g3', '-fno-omit-frame-pointer']),
    language: 'c',
  )
endif
message('nordstjernen build profile: ' +
        (ns_is_release ? 'release (LTO + stripped + hardened)'
                       : 'development (debug symbols + frame pointer + hardened)'))

is_android = host_machine.system() == 'android'
# iOS (set by the ios/ meson cross file) cross-compiles the same GTK-free
# embeddable engine as Android, driven by a UIKit host app instead of GTK 4.
# is_mobile gathers the two: both drop GTK 4, gdk-pixbuf, epoxy/WebGL
# and the desktop-only helper executables, building only libnordstjernen.
is_ios = get_option('ios')
is_mobile = is_android or is_ios
if is_mobile
  add_project_arguments('-DNS_MOBILE=1', language: 'c')
endif

# Android builds only the embeddable engine library, driven by the JNI host app
# in android/. It links the GLib/cairo/pango graphics stack directly instead of
# GTK 4: the engine uses no GTK widgets, and GdkTexture is replaced by the
# ns_texture abstraction (src/texture.c). gtk4 is therefore desktop-only.
# The engine paints with Cairo/Pango and wraps decoded images in the
# ns_texture abstraction (no GdkTexture), so the graphics stack is declared
# directly rather than pulled in transitively through gtk4. The toolkit
# (gtk4/epoxy) stays out of the shared engine deps and is added only to the
# GTK shell, keeping the out-of-process renderer and the embeddable library
# free of GTK. The GTK shell itself is a feature: disabling it yields a
# renderer-only build that needs no GTK 4 at all.
gtk_feature = get_option('gtk')
gtk4_min = host_machine.system() == 'windows' ? '>=4.22.1' : '>=4.14'
use_web_text_lib = not is_mobile and get_option('web-text-lib').allowed()

# web-text-lib enforces the text-stack floors from inside its own build, where a
# too-old library surfaces as a bare version mismatch and only the first of
# several is reported. Checking every hard floor here first names them all.
ns_dependency_floors = [['libcurl', '>=8.5']]
if use_web_text_lib
  ns_dependency_floors += [
    ['glib-2.0', '>=2.80'],
    ['gobject-2.0', '>=2.80'],
    ['gio-2.0', '>=2.80'],
    ['cairo', '>=1.18'],
    ['harfbuzz', '>=8.3'],
    ['fribidi', '>=1.0.6'],
    ['fontconfig', '>=2.15'],
  ]
endif
if not is_mobile and gtk_feature.enabled()
  ns_dependency_floors += [['gtk4', gtk4_min]]
endif
ns_unmet_floors = []
foreach floor : ns_dependency_floors
  if not dependency(floor[0], version: floor[1], required: false).found()
    installed = dependency(floor[0], required: false)
    ns_unmet_floors += '  @0@ @1@ (@2@)'.format(floor[0], floor[1],
      installed.found() ? 'found ' + installed.version() : 'not found')
  endif
endforeach
if ns_unmet_floors.length() > 0
  ns_floor_hint = ''
  if use_web_text_lib
    ns_floor_hint = (' -Dweb-text-lib=disabled drops the text-stack floors by ' +
                     'shaping through the system Pango.')
  endif
  error('Nordstjernen needs newer system libraries than this machine has:\n' +
        '\n'.join(ns_unmet_floors) + '\n' +
        'Ubuntu 24.04, Debian 13 and Fedora 40 are the oldest releases of ' +
        'their distributions that ship all of them.' + ns_floor_hint)
endif

glib       = dependency('glib-2.0')
gobject    = dependency('gobject-2.0')
gio        = dependency('gio-2.0')
cairo      = dependency('cairo')

# Text is shaped through web-text-lib (src/web-text-lib/), the in-tree Pango
# fork that caches finished glyph strings across layouts; every symbol in it is
# renamed, because GTK loads the system Pango into the same process. Android
# and iOS link the system Pango instead -- the fork carries no CoreText backend
# and requires fontconfig -- and src/ns_pango_names.h maps the renamed API back
# for them. Its targets are declared next to lexbor's, after the last project
# argument.
if use_web_text_lib
  add_project_arguments('-DNS_USE_WEB_TEXT_LIB=1', language: 'c')
else
  pango      = dependency('pango')
  pangocairo = dependency('pangocairo')
endif

if not is_mobile
  # libepoxy is the GL/EGL dispatch layer for WebGL. It lives in the shared
  # engine deps (not the GTK shell) because WebGL renders to an offscreen
  # EGL/GLES context in the renderer, with no toolkit involved.
  epoxy      = dependency('epoxy')
  have_egl   = cc.has_header('epoxy/egl.h', dependencies: epoxy)
  if host_machine.system() == 'linux' and not have_egl
    error('WebGL needs libepoxy built with EGL support (epoxy/egl.h)')
  endif
endif

have_gtk = false
if not is_mobile and gtk_feature.allowed()
  gtk4 = dependency('gtk4', version: gtk4_min, required: gtk_feature)
  if gtk4.found()
    have_gtk = true
    gtk4_dep = gtk4
    if host_machine.system() == 'windows'
      message('GTK @0@ — Windows requires the MSYS2 stock GTK (>= 4.22.1).'.format(gtk4.version()))
    elif gtk4.version().version_compare('>=4.22')
      message('GTK @0@ — preferred (>= 4.22).'.format(gtk4.version()))
    else
      warning('GTK @0@ meets the 4.14 minimum, but Nordstjernen prefers >= 4.22 for the newer GSK renderers.'.format(gtk4.version()))
    endif
  endif
endif
gmodule = dependency('gmodule-2.0')
libcurl = dependency('libcurl', version: '>=8.5')
if cc.has_header_symbol('curl/curl.h', 'CURLOPT_COOKIE_SITE', dependencies: libcurl)
  add_project_arguments('-DNS_CURL_HAVE_COOKIE_SITE=1', language: 'c')
  message('libcurl: SameSite and Partitioned cookies decided by libcurl')
endif
if cc.has_header_symbol('curl/curl.h', 'CURLOPT_PRIORITY_URGENCY', dependencies: libcurl)
  add_project_arguments('-DNS_CURL_HAVE_PRIORITY=1', language: 'c')
  message('libcurl: RFC 9218 request priorities')
endif
if libcurl.version().version_compare('<8.11')
  warning('libcurl @0@ predates stable WebSocket support (curl 8.11): WebSocket will be unavailable at runtime unless this libcurl was built with the ws/wss protocols.'.format(libcurl.version()))
endif
libcrypto = dependency('libcrypto')
libm = cc.find_library('m', required: false)

libuchardet = dependency('uchardet')

libpsl = dependency('libpsl')

sqlite3 = dependency('sqlite3')

if not is_mobile
  add_project_arguments('-DNS_ENABLE_WEBGL=1', language: 'c')
  # The offscreen GL context (src/glctx.c) has three backends: WGL on Windows,
  # CGL on macOS (which has no EGL — its epoxy dispatches to the OpenGL
  # framework, and its GL 4.1 core profile compiles GLSL-ES shaders), and EGL
  # everywhere else. Pick CGL on darwin, otherwise EGL when its header is
  # present; without any, ns_gl_context_create() stubs out and WebGL is simply
  # unavailable rather than failing the build.
  if host_machine.system() == 'darwin'
    add_project_arguments('-DNS_HAVE_CGL=1', language: 'c')
  elif have_egl
    add_project_arguments('-DNS_HAVE_EGL=1', language: 'c')
  endif
endif

# WebGPU (navigator.gpu) layers src/webgpu.c over the external wgpu-native
# library; the webgpu.h/wgpu.h headers are vendored in-tree (pinned API). The
# library comes from pkg-config (wgpu_native), an extracted release passed with
# -Dwgpu_native_root, or -- on the desktop platforms wgpu-native publishes
# glibc/macOS/MinGW releases for -- the pinned release fetched by the
# subprojects/wgpu-native-<platform>.wrap and linked statically. On those
# platforms WebGPU, like WebGL, is a required part of the build; elsewhere
# (the BSDs, musl, Android, iOS) it stays optional. -Dwebgpu=disabled always
# drops it. At runtime it is on by default, like WebGL; Settings can turn it
# off, and --enable-webgpu (NS_WEBGPU_ALLOW=1) overrides that.
have_webgpu = false
webgpu_dep = declare_dependency()
webgpu_feature = get_option('webgpu')
wgpu_platform = ''
if host_machine.cpu_family() in ['x86_64', 'aarch64']
  if host_machine.system() == 'linux' and cc.get_define('__GLIBC__',
      prefix: '#include <features.h>') != ''
    wgpu_platform = 'linux-' + host_machine.cpu_family()
  elif host_machine.system() == 'darwin'
    wgpu_platform = 'macos-' + host_machine.cpu_family()
  elif host_machine.system() == 'windows' and host_machine.cpu_family() == 'x86_64'
    wgpu_platform = 'windows-x86_64-gnu'
  endif
endif
webgpu_required = wgpu_platform != '' or webgpu_feature.enabled()
if not webgpu_feature.disabled() and not is_mobile
  wgpu_lib = dependency('wgpu_native', required: false)
  wgpu_root = get_option('wgpu_native_root')
  if not wgpu_lib.found() and wgpu_root != ''
    wgpu_lib = cc.find_library('wgpu_native', dirs: [wgpu_root / 'lib'],
                               required: false)
  endif
  if not wgpu_lib.found() and wgpu_platform != ''
    wgpu_lib = subproject('wgpu-native-' + wgpu_platform).get_variable('wgpu_native_dep')
  endif
  if wgpu_lib.found()
    webgpu_dep = declare_dependency(
      include_directories: include_directories('third_party/wgpu-native/include'),
      dependencies: [
        wgpu_lib,
        cc.find_library('dl', required: false),
        cc.find_library('m', required: false),
        dependency('threads'),
      ],
    )
    add_project_arguments('-DND_HAVE_WEBGPU=1', language: 'c')
    have_webgpu = true
    message('WebGPU: built over wgpu-native -- on by default, switchable in Settings')
  elif webgpu_required
    error('WebGPU needs wgpu-native: install its pkg-config file (wgpu_native), ' +
          'pass -Dwgpu_native_root=/path/to/extracted/release, or ' +
          '-Dwebgpu=disabled to build without it')
  else
    message('WebGPU: not built (wgpu-native has no release for this platform; ' +
            'pass -Dwgpu_native_root or install its pkg-config to enable)')
  endif
endif

libpoppler = dependency('poppler-glib', required: false)
have_poppler = libpoppler.found() and not is_mobile
if have_poppler
  add_project_arguments('-DNS_HAVE_POPPLER=1', language: 'c')
endif

libfontconfig = dependency('fontconfig', required: false)
have_fontconfig = libfontconfig.found()
if have_fontconfig
  add_project_arguments('-DNS_HAVE_FONTCONFIG=1', language: 'c')
endif
if use_web_text_lib
  have_pangoft2 = true
else
  libpangoft2 = dependency('pangoft2', required: false)
  have_pangoft2 = libpangoft2.found()
endif
if have_pangoft2
  add_project_arguments('-DNS_HAVE_PANGOFT2=1', language: 'c')
endif

libfreetype = dependency('freetype2', required: false)
have_freetype = libfreetype.found()
if have_freetype
  add_project_arguments('-DNS_HAVE_FREETYPE=1', language: 'c')
endif

# Brotli: the br Content-Encoding, and WOFF2 web fonts. FreeType 2.10.2 and
# later decode WOFF2 themselves when built with Brotli, which the installed
# ftoption.h records; only a FreeType without it (Homebrew's) needs
# src/woff2.c to decode them over libbrotlidec.
libbrotlidec = dependency('libbrotlidec', required: false)
if libbrotlidec.found()
  add_project_arguments('-DNS_HAVE_BROTLI_DEC=1', language: 'c')
endif
freetype_reads_woff2 = have_freetype and cc.compiles('''
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CONFIG_OPTIONS_H
#if !defined(FT_CONFIG_OPTION_USE_BROTLI) || FREETYPE_MAJOR * 10000 + FREETYPE_MINOR * 100 + FREETYPE_PATCH < 21002
#error FreeType does not decode WOFF2
#endif
''', dependencies: libfreetype, name: 'FreeType decodes WOFF2')
have_woff2 = libbrotlidec.found() and not freetype_reads_woff2
if have_woff2
  add_project_arguments('-DNS_HAVE_WOFF2=1', language: 'c')
endif

# zstd Content-Encoding; without it the browser stops advertising zstd.
libzstd = dependency('libzstd', required: false)
if libzstd.found()
  add_project_arguments('-DNS_HAVE_ZSTD=1', language: 'c')
endif

libwebp = dependency('libwebp')
libwebpdemux = dependency('libwebpdemux')

avif_feature = get_option('avif').disable_auto_if(is_mobile)
libavif = dependency('libavif', required: avif_feature)
have_avif = libavif.found() and not is_mobile
if have_avif
  add_project_arguments('-DNS_HAVE_AVIF=1', language: 'c')
endif

jxl_feature = get_option('jxl').disable_auto_if(is_mobile)
libjxl = dependency('libjxl', required: jxl_feature)
have_jxl = libjxl.found() and not is_mobile
if have_jxl
  add_project_arguments('-DNS_HAVE_JXL=1', language: 'c')
endif

# Inline WebM (VP9/VP8 + Opus/Vorbis) over FFmpeg's libav* (github.com/FFmpeg/
# FFmpeg, not the defunct Libav fork). Required on Linux and Windows, where WebM
# is the dominant format served by YouTube and other sites and the
# external-player fallback is undesirable; auto-detected on macOS; excluded on
# Android, whose dependency sysroot (android/scripts/build-deps.sh) does not
# cross-build FFmpeg, so WebM there stays on the external-player path.
#
# The version floors are the library sonames of FFmpeg 6.0 (2023) — the oldest
# release carrying the new AVChannelLayout API this code uses — so a stock Linux
# build links the system FFmpeg (Ubuntu 24.04 ships 6.1, and every later FFmpeg
# through 8.0 also satisfies these floors) with no source build. The
# macOS/Windows packages bundle a minimal LGPL FFmpeg built by
# scripts/build-ffmpeg-lgpl.sh. Below 6.0 the required build fails on
# Linux/Windows and WebM disables on macOS.
libav_required = host_machine.system() == 'windows' or host_machine.system() == 'linux'
libavformat = dependency('libavformat', version: '>=60', required: libav_required)
libavcodec = dependency('libavcodec', version: '>=60', required: libav_required)
libavutil = dependency('libavutil', version: '>=58', required: libav_required)
libswscale = dependency('libswscale', version: '>=7', required: libav_required)
libswresample = dependency('libswresample', version: '>=4', required: libav_required)
have_libav = libavformat.found() and libavcodec.found() and libavutil.found() \
  and libswscale.found() and libswresample.found() and not is_mobile
if have_libav
  add_project_arguments('-DNS_HAVE_LIBAV=1', language: 'c')
endif

# H.264 video and AAC audio decode through the operating system's own codecs,
# so the binary carries no patent-encumbered decoder and pays no codec licence:
# Media Foundation on Windows, VideoToolbox/AudioToolbox on macOS and iOS,
# MediaCodec on Android, and GStreamer elsewhere (which in turn picks VA-API,
# V4L2 or a software decoder installed on the system). libav still demuxes the
# MP4 container; see src/media_os.h and docs/media.md.
media_os_backend = 'none'
media_os_deps = []
if host_machine.system() == 'windows'
  media_os_backend = 'mf'
  media_os_deps += [cc.find_library('mfuuid'),
                    cc.find_library('ole32')]
elif host_machine.system() == 'darwin'
  media_os_backend = 'vt'
  media_os_deps += [dependency('appleframeworks',
                               modules: ['VideoToolbox', 'AudioToolbox',
                                         'CoreMedia', 'CoreVideo',
                                         'CoreFoundation'])]
elif is_android
  media_os_backend = 'android'
  media_os_deps += [cc.find_library('mediandk')]
else
  gst_required = get_option('gstreamer')
  if host_machine.system() == 'linux' and gst_required.auto()
    gst_required = true
  endif
  gst_core = dependency('gstreamer-1.0', version: '>=1.20',
                        required: gst_required)
  gst_app = dependency('gstreamer-app-1.0', version: '>=1.20',
                       required: gst_required)
  gst_video = dependency('gstreamer-video-1.0', version: '>=1.20',
                         required: gst_required)
  if gst_core.found() and gst_app.found() and gst_video.found()
    media_os_backend = 'gst'
    media_os_deps += [gst_core, gst_app, gst_video]
    add_project_arguments('-DNS_MEDIA_OS_GST=1', language: 'c')
  endif
endif
if media_os_backend == 'none'
  add_project_arguments('-DNS_MEDIA_OS_NONE=1', language: 'c')
endif
ns_ipc_rust = add_languages('rust', native: false,
                            required: get_option('ipc_rust'))
if ns_ipc_rust
  add_project_arguments('-DNS_IPC_RUST=1', language: 'c')
endif
media_os_sources = files('src/media_os.c')
if media_os_backend != 'none'
  media_os_sources += files('src/media_os_' + media_os_backend + '.c')
endif
# Without libav there is no MP4 demuxer, so the in-tree one (src/mp4.c) feeds
# the OS decoder instead: Android, iOS and macOS builds without FFmpeg.
have_os_mp4 = media_os_backend != 'none' and not have_libav
if have_os_mp4
  add_project_arguments('-DNS_HAVE_OS_MP4=1', language: 'c')
endif

# vorbisfile is the ov_* high-level decode API over libvorbis, used by the audio
# helper (src/audio/main.c) to decode Ogg/Vorbis natively where FFmpeg is absent.
libvorbisfile = dependency('vorbisfile', required: false)
if libvorbisfile.found()
  add_project_arguments('-DNS_HAVE_VORBISFILE=1', language: 'c')
endif
# opusfile is the high-level op_* decode API over libopus + libogg, used by the
# audio helper to decode Ogg/Opus natively where FFmpeg is absent.
libopusfile = dependency('opusfile', required: false)
if libopusfile.found()
  add_project_arguments('-DNS_HAVE_OPUSFILE=1', language: 'c')
endif
# canPlayType/mediaCapabilities may honestly advertise native Ogg audio only
# where the decoder that plays it is built: the desktop (SDL2) nordstjernen-audio
# helper. On mobile audio takes a different path, so these are gated off there.
if not is_mobile and get_option('audio').allowed()
  sdl2_probe = dependency('sdl2', required: false)
  if sdl2_probe.found() and libvorbisfile.found()
    add_project_arguments('-DNS_AUDIO_NATIVE_VORBIS=1', language: 'c')
  endif
  if sdl2_probe.found() and libopusfile.found()
    add_project_arguments('-DNS_AUDIO_NATIVE_OPUS=1', language: 'c')
  endif
endif
libenchant = dependency('enchant-2', required: false)
have_enchant = libenchant.found() and not is_mobile
if have_enchant
  add_project_arguments('-DNS_HAVE_ENCHANT=1', language: 'c')
endif

libseccomp = dependency(
  'libseccomp',
  required: host_machine.system() == 'linux',
)
have_seccomp = libseccomp.found() and not is_mobile
if have_seccomp
  add_project_arguments('-DNS_HAVE_SECCOMP=1', language: 'c')
endif

libwuffs = dependency('wuffs', fallback: ['wuffs', 'wuffs_dep'])
libplmpeg = dependency('plmpeg', fallback: ['plmpeg', 'plmpeg_dep'])
zlib = dependency('zlib')

fs = import('fs')
lexbor_version = '0.0.0'
if fs.is_file('src/lexbor/version')
  lexbor_version_raw = fs.read('src/lexbor/version').strip()
  lexbor_version = lexbor_version_raw.split('=')[1]
endif
add_project_arguments('-DNS_LEXBOR_VERSION="' + lexbor_version + '"',
                      language: 'c')

build_date = get_option('build_date')
if build_date == ''
  date_cmd = run_command('date', '-u', '+%Y-%m-%d', check: false)
  if date_cmd.returncode() == 0
    build_date = date_cmd.stdout().strip()
  endif
endif
add_project_arguments('-DNS_BUILD_DATE="' + build_date + '"', language: 'c')
add_project_arguments('-DNS_VERSION="' + meson.project_version() + '"', language: 'c')

if use_web_text_lib
  subdir('src/web-text-lib')
  pango       = web_text_dep
  pangocairo  = web_text_cairo_dep
  libpangoft2 = web_text_fc_dep
endif
subdir('src/lexbor')

if get_option('quickjs') == 'quickjs'
  quickjs_sp = subproject('quickjs',
    default_options: ['warning_level=0', 'werror=false'])
  quickjs_version = quickjs_sp.get_variable('quickjs_version')
  libquickjs = declare_dependency(
    compile_args: ['-DNS_QUICKJS_ORIGINAL',
                   '-DNS_QUICKJS_ORIGINAL_VERSION="' + quickjs_version + '"'],
    dependencies: quickjs_sp.get_variable('quickjs_dep'))
  message('JS engine: quickjs (Bellard\'s original QuickJS ' +
          quickjs_version + ' through src/ns_quickjs.h, full DOM bindings)')
else
  subdir('src/quickjs')
  message('JS engine: quickjs (in-tree quickjs-ng fork, full DOM bindings)')
endif
quickjs_original = get_option('quickjs') == 'quickjs'

wasm_unsupported_cpu = host_machine.cpu_family() == 'x86'
if get_option('wasm').enabled() and wasm_unsupported_cpu
  error('wasm (WAMR) is not supported on 32-bit x86; build with -Dwasm=disabled')
endif
wasm_feature = get_option('wasm').disable_auto_if(wasm_unsupported_cpu)
have_wasm = false
have_wasm_simd = false
simde_dep = declare_dependency()
if wasm_feature.allowed()
  wasm_simd_feature = get_option('wasm_simd')
  if wasm_simd_feature.allowed()
    simde_dep = dependency('simde', required: false)
    if not simde_dep.found() and cc.has_header('simde/wasm/simd128.h')
      simde_dep = declare_dependency()
    endif
    if not simde_dep.found()
      simde_dep = dependency('simde', required: wasm_simd_feature,
                             fallback: ['simde', 'simde_dep'])
    endif
    have_wasm_simd = simde_dep.found()
  endif
  subdir('src/wamr')
  have_wasm = true
else
  libwamr = declare_dependency()
endif

install_data(
  'data/icons/hicolor/scalable/apps/nordstjernen.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen.gif',
  'data/icons/hicolor/scalable/apps/nordstjernen-back-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-bookmarks-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-close-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-downloads-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-forward-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-globe-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-home-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-lock-open-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-lock-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-menu-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-new-tab-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-page-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-private-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-reload-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-star-filled-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-star-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stop-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-warning-symbolic.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-console.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-settings.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-about.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-connect.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-fetch.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-parse.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-script.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-render.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-done.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_data(
  'License.md',
  'COPYING',
  'THIRD-PARTY-LICENSES.md',
  install_dir: get_option('datadir') / 'nordstjernen',
)

install_data(
  'data/safebrowsing.list',
  install_dir: get_option('datadir') / 'nordstjernen',
)

install_data(
  'data/i18n/am.lang',
  'data/i18n/ar.lang',
  'data/i18n/bn.lang',
  'data/i18n/de.lang',
  'data/i18n/el.lang',
  'data/i18n/en.lang',
  'data/i18n/es.lang',
  'data/i18n/fa.lang',
  'data/i18n/fr.lang',
  'data/i18n/gu.lang',
  'data/i18n/ha.lang',
  'data/i18n/hi.lang',
  'data/i18n/hu.lang',
  'data/i18n/id.lang',
  'data/i18n/it.lang',
  'data/i18n/ja.lang',
  'data/i18n/jv.lang',
  'data/i18n/kn.lang',
  'data/i18n/ko.lang',
  'data/i18n/ml.lang',
  'data/i18n/mr.lang',
  'data/i18n/my.lang',
  'data/i18n/nl.lang',
  'data/i18n/pa.lang',
  'data/i18n/pl.lang',
  'data/i18n/pt.lang',
  'data/i18n/ro.lang',
  'data/i18n/ru.lang',
  'data/i18n/su.lang',
  'data/i18n/sw.lang',
  'data/i18n/ta.lang',
  'data/i18n/te.lang',
  'data/i18n/th.lang',
  'data/i18n/tl.lang',
  'data/i18n/tr.lang',
  'data/i18n/uk.lang',
  'data/i18n/ur.lang',
  'data/i18n/vi.lang',
  'data/i18n/yo.lang',
  'data/i18n/zh.lang',
  install_dir: get_option('datadir') / 'nordstjernen' / 'i18n',
)

install_data(
  'data/nordstjernen.desktop',
  install_dir: get_option('datadir') / 'applications',
  rename: 'org.nordstjernen.WebBrowser.desktop',
)

install_data(
  'data/org.nordstjernen.WebBrowser.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

summary({'WebAssembly (WAMR)': have_wasm,
         'WebAssembly SIMD (SIMDe)': have_wasm_simd}, section: 'Features')
summary({'AVIF images (libavif)': have_avif}, section: 'Features')
summary({'H.264/AAC OS decoders': media_os_backend}, section: 'Features')
summary({'JPEG XL images (libjxl)': have_jxl}, section: 'Features')
summary({'GTK 4 shell': have_gtk}, section: 'Shells')

ns_ipc_rust_dep = []
if ns_ipc_rust
  ns_ipc_rust_raw = static_library('ns_ipc_http_rs_raw',
    'src/ipc_http_rs/lib.rs',
    rust_abi: 'c',
    rust_args: ['-C', 'panic=abort'],
    override_options: ['rust_std=2021'],
  )
  ns_ipc_rust_lib = custom_target('ns_ipc_http_rs',
    input: ns_ipc_rust_raw,
    output: 'libns_ipc_http_rs.a',
    command: [find_program('scripts/rust-staticlib-localize.sh'),
              '@INPUT@', '@OUTPUT@',
              find_program('ld'), find_program('objcopy'), find_program('ar'),
              'ns_ipc_rs_*', 'http_write_*', 'json_*'],
  )
  ns_ipc_rust_dep = declare_dependency(link_with: ns_ipc_rust_lib)
endif

subdir('src')

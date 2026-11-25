python = import('python').find_installation()
embed_text = files('../scripts/embed-text.py')

# The engine's own scripts run in every realm and every worker. A native
# build compiles them to bytecode once, here, so a new process reads
# bytecode instead of parsing 670 KB of JavaScript before the page starts.
engine_bytecode = not meson.is_cross_build()
engine_bundles = {}
foreach name, src : {'polyfills': '../data/js/polyfills.js',
                     'streaming': '../data/js/streaming.js',
                     'typedom': '../data/js/typed-om.js'}
  embed_input = src
  embed_args = ['ns_js_' + name + '_src']
  if engine_bytecode
    embed_input = custom_target(
      name + '.bc',
      input: src,
      output: name + '.bc',
      command: [bqjs_bcgen, '<' + name + '>', '@INPUT@', '@OUTPUT@'],
    )
    embed_args = ['ns_js_' + name + '_bc', 'NS_ENGINE_BYTECODE']
  endif
  engine_bundles += {name: custom_target(
    name + '.h',
    input: [embed_text, embed_input],
    output: name + '.h',
    command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@'] + embed_args,
  )}
endforeach
polyfills_h = engine_bundles['polyfills']
streaming_h = engine_bundles['streaming']
typedom_h = engine_bundles['typedom']

sb_key_h = custom_target(
  'ns_sb_key.h',
  input: '../scripts/gen-safebrowsing-key.py',
  output: 'ns_sb_key.h',
  command: [python, '@INPUT@', '@OUTPUT@'],
  build_always_stale: true,
)

embedded_text = {
  'remote_page': ['../data/js/remote-page.js', 'ns_remote_page_src'],
  'webdriver_page': ['../data/js/webdriver-page.js', 'ns_webdriver_page_src'],
  'wpt_hook': ['../data/js/wpt-hook.js', 'ns_wpt_hook_src'],
}
if have_pdfjs
  subdir('pdfjs')
  embedded_text += {
    'pdf_viewer_js': ['../data/js/pdf-viewer.js', 'ns_pdf_viewer_js'],
    'pdf_viewer_css': ['../data/js/pdf-viewer.css', 'ns_pdf_viewer_css'],
  }
endif
embedded_headers = {}
foreach name, spec : embedded_text
  embedded_headers += {name: custom_target(
    name + '.h',
    input: [embed_text, spec[0]],
    output: name + '.h',
    command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@', spec[1]],
  )}
endforeach
remote_page_h = embedded_headers['remote_page']
webdriver_page_h = embedded_headers['webdriver_page']
wpt_hook_h = embedded_headers['wpt_hook']
pdf_viewer_headers = []
if have_pdfjs
  pdf_viewer_headers = [embedded_headers['pdf_viewer_js'],
                        embedded_headers['pdf_viewer_css']]
endif

nordstjernen_engine_sources = files(
  'anim.c',
  'bytecode_cache.c',
  'cache.c',
  'compression_dictionary.c',
  'connection_allowlist.c',
  'cookies.c',
  'csp.c',
  'css.c',
  'css_color.c',
  'css_anchor.c',
  'css_shape.c',
  'css_sizes.c',
  'css_filter.c',
  'css_counter_style.c',
  'css_media.c',
  'css_prop_syntax.c',
  'css_grammar.c',
  'css_syntax.c',
  'datetime.c',
  'dom.c',
  'download.c',
  'engine.c',
  'eventsource.c',
  'forms.c',
  'camera.c',
  'mic.c',
  'font.c',
  'glctx.c',
  'glsl_validate.c',
  'gpu_wire.c',
  'html.c',
  'image.c',
  'smil.c',
  'svg.c',
  'svg_external.c',
  'image_ico.c',
  'image_webp.c',
  'image_wuffs.c',
  'layout.c',
  'mathml.c',
  'net.c',
  'content_decode.c',
  'paint.c',
  'permissions_policy.c',
  'print.c',
  'pdf.c',
  'render.c',
  'safebrowsing.c',
  'safebrowsing_google.c',
  'selection.c',
  'spellcheck.c',
  'texture.c',
  'video.c',
  'video_decode.c',
  'webcrypto.c',
  'ws.c',
  'html_parser.c',
  'xml.c',
)

nordstjernen_engine_sources += files(
  'ext.c',
  'idb.c',
  'js.c',
  'js_brand.c',
  'js_canvas.c',
  'js_canvas_api.c',
  'js_cage.c',
  'js_perf.c',
  'js_webauthn.c',
  'js_realm.c',
  'wasm.c',
  'webaudio.c',
  'webgl.c',
  'ns_quickjs.c',
)

if not is_mobile and have_glslang
  nordstjernen_engine_sources += files('webgl_validate.c')
endif

# GTK-free helpers used by the desktop executable but not the engine UI:
# the crash watchdog. Kept in src/ as common
# code; the GTK frontend itself lives in src/gtk/.
nordstjernen_app_sources = files(
  'watchdog.c',
)

if have_avif
  nordstjernen_engine_sources += files('image_avif.c')
endif

if have_jxl
  nordstjernen_engine_sources += files('image_jxl.c')
endif

if have_woff2
  nordstjernen_engine_sources += files('woff2.c')
endif

if have_webgpu
  nordstjernen_engine_sources += files('webgpu.c')
endif

nordstjernen_engine_sources += media_os_sources
nordstjernen_engine_sources += [sb_key_h]
nordstjernen_engine_sources += pdf_viewer_headers
if have_os_mp4
  nordstjernen_engine_sources += files('mp4.c', 'video_mp4.c')
endif

nordstjernen_platform_sources = files(
  'about_page.c',
  'bookmarks.c',
  'capability.c',
  'config.c',
  'db.c',
  'debuglog.c',
  'history.c',
  'i18n.c',
  'json_string.c',
  'netutil.c',
  'security.c',
  'security_root.c',
)

nordstjernen_platform_deps = [glib, gobject, gio, sqlite3, libpsl]
if have_seccomp
  nordstjernen_platform_deps += [libseccomp]
endif

libns_platform = static_library(
  'ns-platform',
  nordstjernen_platform_sources,
  dependencies: nordstjernen_platform_deps,
  install: false,
)

nordstjernen_win_resources = []
if host_machine.system() == 'windows'
  windows = import('windows')
  nordstjernen_win_resources = windows.compile_resources(
    'nordstjernen.rc',
    depend_files: files('../data/icons/nordstjernen.ico'),
  )
endif

ns_win_system_libs = []
if host_machine.system() == 'windows'
  foreach name : ['bcrypt', 'advapi32', 'shell32', 'user32', 'ws2_32']
    ns_win_system_libs += cc.find_library(name)
  endforeach
endif

nordstjernen_deps = [
  glib,
  gobject,
  gio,
  gmodule,
  cairo,
  pango,
  pangocairo,
  libcurl,
  libcrypto,
  libm,
  libquickjs,
  libwebp,
  libwebpdemux,
  libwuffs,
  libplmpeg,
  libuchardet,
  ns_parsers_rs_dep,
  libwamr,
  sqlite3,
  zlib,
  libpsl,
  libfontconfig,
  libfreetype,
  libbrotlidec,
  libzstd,
  media_os_deps,
  ns_win_system_libs,
]
if have_pangoft2
  nordstjernen_deps += [libpangoft2]
endif
if not is_mobile
  nordstjernen_deps += [epoxy, glslang_deps, libseccomp, libavif, libjxl,
                        libenchant]
  if have_libav
    nordstjernen_deps += [libavformat, libavcodec, libavutil, libswscale]
  endif
endif

# macOS WebGL: src/glctx.c creates the offscreen GL context through CGL, whose
# symbols live in the OpenGL framework. Unused targets drop it via
# -dead_strip_dylibs.
if host_machine.system() == 'darwin'
  nordstjernen_deps += [dependency('appleframeworks', modules: ['OpenGL'])]
endif

sdl3_dep = dependency('sdl3', version: '>=3.2.0', required: get_option('audio'))
if not sdl3_dep.found() and not is_mobile and get_option('audio').auto()
  warning('SDL3 not found: building WITHOUT audio, so pages and videos ' +
          'play silently. Install the SDL3 (3.2 or newer) development ' +
          'package (libsdl3-dev, SDL3-devel, sdl3-dev or sdl3) or pass ' +
          '-Daudio=disabled to silence this warning.')
endif
ns_lib_c_args = []
ns_sdl_deps = []
if sdl3_dep.found()
  ns_sdl_deps += [sdl3_dep.partial_dependency(compile_args: true,
                                              includes: true)]
  ns_lib_c_args += ['-DNS_HAVE_SDL',
                    '-DNS_SDL_LIBDIR="@0@"'.format(
                      sdl3_dep.get_variable(pkgconfig: 'libdir',
                                            default_value: ''))]
endif

nordstjernen_deps += [webgpu_dep]

nordstjernen_link_args = []
if host_machine.system() == 'windows'
  nordstjernen_link_args += cc.get_supported_link_arguments(
    ['-Wl,--stack,8388608'],
  )
endif

# NetBSD pkgsrc libunistring (pulled in transitively through the libcurl /
# libpsl / libidn2 / gnutls stack) carries an undefined reference to libc's
# mbrtoc32, which the hardened --no-undefined link rejects even though the
# dynamic linker resolves it at runtime. Allow undefined symbols originating
# from shared libraries on NetBSD only; our own objects are still checked.
# Every binary that links that stack needs this — the executables below and
# the audio/video helpers, which pull in libunistring via libcurl.
if host_machine.system() == 'netbsd'
  nordstjernen_link_args += cc.get_supported_link_arguments(
    ['-Wl,--allow-shlib-undefined'],
  )
endif

# The gmodule-2.0 dependency injects -Wl,--export-dynamic, which dumps every
# internal engine symbol (~6000: ns_*, quickjs) into the executables'
# .dynsym. Combined with -Wl,-z,now that bloats the dynamic-linker symbol
# resolution done on every process start — and the renderer is forked+exec'd
# afresh, so it pays that cost on each cold start. A version script localises
# the executables' own symbols (it overrides --export-dynamic regardless of
# link order); the only runtime dlsym we need (RAND_bytes for RNG warmup)
# resolves through a dependency library, whose exports this leaves untouched.
# Applies to the executables only; the shared libnordstjernen keeps its API.
nordstjernen_exe_link_args = nordstjernen_link_args + \
  cc.get_supported_link_arguments(
    ['-Wl,--version-script=' + meson.current_source_dir() / 'exe-local.map'])

if host_machine.system() == 'windows'
  executable(
    'nordstjernen-launcher',
    files('win_launcher.c') + nordstjernen_win_resources,
    win_subsystem: 'windows',
    dependencies: [cc.find_library('user32'), cc.find_library('shell32')],
  )
endif

nordstjernen_embed_sources = nordstjernen_engine_sources + files(
  'embed_shim.c',
  'libnordstjernen.c',
) + [polyfills_h, streaming_h, typedom_h, remote_page_h, webdriver_page_h]

if is_android
  nordstjernen_embed_sources += files(
    'ipc_http.c',
    'renderer_serve.c',
    'renderer_tiles.c',
    'rproc_http.c',
  )
endif

# The engine is compiled once, into this static library. The renderer links
# it in (the GTK shell only on GTK < 4.22, for its SVG icon fallback; it
# otherwise links just libns-platform), so each ships as a self-contained
# executable (no
# libnordstjernen.so runtime dependency to bundle in distro packages), and
# the shared libnordstjernen is linked from the same position-independent
# objects instead of compiling every source a second time.
libnordstjernen_static = static_library(
  'nordstjernen-static',
  nordstjernen_embed_sources,
  link_whole: libns_platform,
  dependencies: nordstjernen_deps + ns_sdl_deps,
  c_args: ns_lib_c_args,
  pic: true,
  install: false,
)

libnordstjernen_lib = library(
  'nordstjernen',
  objects: libnordstjernen_static.extract_all_objects(recursive: true),
  dependencies: nordstjernen_deps + ns_sdl_deps,
  link_args: nordstjernen_link_args,
  install: true,
)

install_headers('libnordstjernen.h', subdir: 'nordstjernen')

# Out-of-process renderer (step 1 of the process-per-tab direction): a small
# executable that drives libnordstjernen over an IPC control channel + shared
# memory framebuffer (src/rproc.*), plus a proof-of-concept client. The mobile
# builds ship only the embeddable library — no standalone executables.
if not is_mobile
  rproc_deps = []
  if host_machine.system() != 'windows'
    rproc_deps += [cc.find_library('rt', required: false)]
  endif

  executable(
    'nordstjernen-renderer',
    files('renderer_http.c', 'renderer_serve.c', 'renderer_tiles.c',
          'ipc_http.c', 'threaddump.c', 'headless.c', 'headless_main.c',
          'rproc_http.c', 'rproc_inproc.c') + [wpt_hook_h],
    link_with: libnordstjernen_static,
    include_directories: include_directories('.'),
    dependencies: nordstjernen_deps + rproc_deps,
    c_args: ns_lib_c_args,
    link_args: nordstjernen_exe_link_args,
    win_subsystem: 'windows',
    install: true,
  )

  if sdl3_dep.found()
    libminimp3 = static_library(
      'minimp3',
      files('audio/minimp3_impl.c'),
      include_directories: include_directories('audio'),
      c_args: cc.get_supported_arguments(['-w']),
      install: false,
    )

    audio_deps = [libcurl, sdl3_dep, libplmpeg, libm, ns_win_system_libs]
    if have_libav
      audio_deps += [libavformat, libavcodec, libavutil, libswresample]
    endif

    # On Linux the helper confines itself with the same Landlock + seccomp
    # profile as the renderer once its audio device and network stack are up,
    # so attacker-controlled codec bytes are decoded inside the sandbox.
    audio_sandbox_sources = []
    if host_machine.system() == 'linux'
      audio_sandbox_sources += files('security.c')
      audio_deps += [glib]
      if have_seccomp
        audio_deps += [libseccomp]
      endif
    endif

    executable(
      'nordstjernen-audio',
      files('audio/main.c') + audio_sandbox_sources + media_os_sources,
      include_directories: include_directories('audio', '.'),
      link_with: libminimp3,
      dependencies: audio_deps + media_os_deps,
      link_args: nordstjernen_link_args,
      win_subsystem: 'windows',
      install: true,
    )
  else
    message('SDL3 not found — audio playback helper (nordstjernen-audio) disabled')
  endif
endif

if have_gtk
  subdir('gtk')
elif not is_mobile
  message('GTK 4 shell disabled — building the engine, out-of-process renderer' +
          ' and embeddable library without GTK.')
endif

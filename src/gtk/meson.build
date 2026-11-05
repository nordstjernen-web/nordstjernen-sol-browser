nordstjernen_gtk_sources = files(
  'appmain.c',
  'mcp.c',
  'pagelayers.c',
  'permissions.c',
  'pointerlock.c',
  'sessionstore.c',
  'procview.c',
  'procwindow.c',
  'remotectl.c',
  'rjson.c',
  'titlerow.c',
  'ns_webauthn.c',
  '../rproc_http.c',
  '../ipc_http.c',
  '../threaddump.c',
)

if gtk4_dep.version().version_compare('<4.22')
  nordstjernen_gtk_sources += files('icons.c')
endif

nordstjernen_gtk_deps = nordstjernen_platform_deps + [gtk4_dep, cairo, libcrypto,
                                                    libm, libpsl]
if host_machine.system() == 'windows'
  nordstjernen_gtk_sources += files('webauthn_win.c')
endif
if have_fontconfig
  nordstjernen_gtk_deps += [libfontconfig]
endif
if host_machine.system() != 'windows'
  nordstjernen_gtk_deps += [cc.find_library('rt', required: false)]
endif

nordstjernen_gtk_link = [libns_platform]
if gtk4_dep.version().version_compare('<4.22')
  nordstjernen_gtk_deps += nordstjernen_deps
  nordstjernen_gtk_link = [libnordstjernen_static]
  nordstjernen_gtk_sources += files('../security_root.c')
endif

nordstjernen_gtk_platform_sources = []
nordstjernen_gtk_platform_link_args = []
if host_machine.system() == 'darwin'
  add_languages('objc', native: false)
  nordstjernen_gtk_platform_sources += files('macos_dock.m')
  nordstjernen_gtk_platform_link_args += ['-framework', 'AppKit']
endif

gnome = import('gnome')
nordstjernen_icon_resources = gnome.compile_resources(
  'nordstjernen-icon-resources',
  meson.project_source_root() / 'data' / 'icons' / 'nordstjernen-icons.gresource.xml',
  source_dir: meson.project_source_root() / 'data' / 'icons',
  c_name: 'nordstjernen_icons',
)

executable(
  'nordstjernen',
  nordstjernen_app_sources +
    nordstjernen_gtk_sources + nordstjernen_gtk_platform_sources +
    nordstjernen_icon_resources +
    nordstjernen_win_resources + [polyfills_h, wpt_hook_h, remote_page_h],
  link_with: nordstjernen_gtk_link,
  dependencies: nordstjernen_gtk_deps,
  include_directories: include_directories('..', '.'),
  win_subsystem: nordstjernen_win_subsystem,
  link_args: nordstjernen_exe_link_args + nordstjernen_gtk_platform_link_args,
  install: true,
)

if host_machine.system() != 'windows'
  install_symlink(
    'open-browser-nordstjernen',
    pointing_to: 'nordstjernen',
    install_dir: get_option('bindir'),
  )
endif

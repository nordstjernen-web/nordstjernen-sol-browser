nordstjernen_gtk_sources = files(
  'appmain.c',
  'mcp.c',
  'pagelayers.c',
  'procview.c',
  'procwindow.c',
  'remotectl.c',
  'rjson.c',
  'titlerow.c',
  '../libnordstjernen.c',
  '../renderer_serve.c',
  '../renderer_tiles.c',
  '../rproc_http.c',
  '../rproc_inproc.c',
  '../ipc_http.c',
  '../threaddump.c',
)

if gtk4_dep.version().version_compare('<4.22')
  nordstjernen_gtk_sources += files('icons.c')
endif

nordstjernen_gtk_deps = nordstjernen_deps + [gtk4_dep]
if host_machine.system() != 'windows'
  nordstjernen_gtk_deps += [cc.find_library('rt', required: false)]
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
  nordstjernen_engine_sources + nordstjernen_app_sources +
    nordstjernen_gtk_sources + nordstjernen_gtk_platform_sources +
    nordstjernen_icon_resources +
    nordstjernen_win_resources + [polyfills_h, wpt_hook_h, remote_page_h],
  dependencies: nordstjernen_gtk_deps,
  c_args: ['-DNS_HAVE_GTK=1'],
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

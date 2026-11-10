# Fabrice Bellard's QuickJS, forked in-tree for -Dquickjs=quickjs (the default). Exposes libquickjs and the qjs-bellard host tool.

bqjs_fs = import('fs')
bqjs_version = bqjs_fs.read('VERSION').strip()

bqjs_c_args = cc.get_supported_arguments('-w', '-fwrapv') + [
  '-D_GNU_SOURCE',
  '-DCONFIG_VERSION="' + bqjs_version + '"',
  '-DCONFIG_TEMPORAL_INTL',
]
if host_machine.system() == 'windows'
  bqjs_c_args += '-D__USE_MINGW_ANSI_STDIO'
endif

bqjs_sys_deps = [
  cc.find_library('m', required: false),
  dependency('threads'),
  dependency('dl', required: false),
]

bqjs_srcs = files(
  'quickjs.c',
  'quickjs-cage.c',
  'dtoa.c',
  'libregexp.c',
  'libunicode.c',
  'cutils.c',
  'intl/intl.c',
  'intl/locale.c',
  'intl/locale-data.c',
  'intl/numberformat.c',
  'intl/collator.c',
  'intl/datetimeformat.c',
  'intl/pluralrules.c',
  'intl/listformat.c',
  'intl/relativetimeformat.c',
  'intl/displaynames.c',
  'intl/segmenter.c',
  'intl/durationformat.c',
  'temporal/temporal.c',
  'temporal/instant.c',
  'temporal/duration.c',
  'temporal/plaintime.c',
  'temporal/now.c',
  'temporal/plaindate.c',
  'temporal/plaindatetime.c',
  'temporal/plainyearmonth.c',
  'temporal/plainmonthday.c',
  'temporal/zoneddatetime.c',
  'temporal/calendar.c',
  'temporal/timezone.c',
  'temporal/tzdata_generated.c',
)

bqjs_lib = static_library('quickjs-bellard',
  bqjs_srcs,
  c_args: bqjs_c_args,
  dependencies: bqjs_sys_deps,
  gnu_symbol_visibility: 'hidden',
)

libquickjs = declare_dependency(
  link_with: bqjs_lib,
  compile_args: ['-DNS_QUICKJS_ORIGINAL',
                 '-DCONFIG_TEMPORAL_INTL',
                 '-DNS_QUICKJS_ORIGINAL_VERSION="' + bqjs_version + '"'],
  dependencies: bqjs_sys_deps,
  include_directories: include_directories('.', is_system: true),
)

if not meson.is_cross_build() and host_machine.system() != 'windows'
  bqjs_libc = static_library('quickjs-bellard-libc',
    'quickjs-libc.c',
    c_args: bqjs_c_args,
    dependencies: bqjs_sys_deps,
  )
  bqjsc = executable('qjsc-bellard',
    'qjsc.c',
    c_args: bqjs_c_args + ['-DCONFIG_CC="cc"', '-DCONFIG_PREFIX="/usr/local"'],
    link_with: [bqjs_libc, bqjs_lib],
    dependencies: bqjs_sys_deps,
    install: false,
  )
  bqjs_repl = custom_target('qjs-bellard-repl',
    input: 'repl.js',
    output: 'repl.c',
    command: [bqjsc, '-s', '-c', '-o', '@OUTPUT@', '-m', '@INPUT@'],
  )
  executable('run-test262-bellard',
    'run-test262.c',
    c_args: bqjs_c_args,
    link_with: [bqjs_libc, bqjs_lib],
    dependencies: bqjs_sys_deps,
    install: false,
    build_by_default: false,
  )
  executable('qjs-bellard',
    'qjs.c', bqjs_repl,
    c_args: bqjs_c_args,
    link_with: [bqjs_libc, bqjs_lib],
    dependencies: bqjs_sys_deps,
    install: false,
  )
endif

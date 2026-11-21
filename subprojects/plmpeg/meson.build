project('plmpeg', 'c', license: 'MIT')

cc = meson.get_compiler('c')

plmpeg_lib = static_library(
  'plmpeg',
  'pl_mpeg_impl.c',
  c_args: cc.get_supported_arguments('-w'),
)

plmpeg_dep = declare_dependency(
  link_with: plmpeg_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('plmpeg', plmpeg_dep)

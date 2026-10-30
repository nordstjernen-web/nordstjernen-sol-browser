# WAMR (wasm-micro-runtime) — vendored subset, fast interpreter with
# reference-types + bulk-memory, exposed as the `libwamr` declared dependency.

wamr_system = host_machine.system()

wamr_c_args = cc.get_supported_arguments(['-w']) + [
  '-DBH_MALLOC=wasm_runtime_malloc',
  '-DBH_FREE=wasm_runtime_free',
  '-DWASM_ENABLE_INTERP=1',
  '-DWASM_ENABLE_FAST_INTERP=1',
  '-DWASM_ENABLE_BULK_MEMORY=1',
  '-DWASM_ENABLE_REF_TYPES=1',
  '-DWASM_ENABLE_MODULE_INST_CONTEXT=1',
  '-DWASM_DISABLE_HW_BOUND_CHECK=1',
  '-DWASM_DISABLE_STACK_HW_BOUND_CHECK=1',
]

wamr_invoke_native_suffix = have_wasm_simd ? '_simd.s' : '.s'

wamr_srcs = files(
  'ns_wamr.c',
  'core/iwasm/common/wasm_blocking_op.c',
  'core/iwasm/common/wasm_exec_env.c',
  'core/iwasm/common/wasm_loader_common.c',
  'core/iwasm/common/wasm_memory.c',
  'core/iwasm/common/wasm_native.c',
  'core/iwasm/common/wasm_runtime_common.c',
  'core/iwasm/interpreter/wasm_interp_fast.c',
  'core/iwasm/interpreter/wasm_loader.c',
  'core/iwasm/interpreter/wasm_runtime.c',
  'core/shared/mem-alloc/ems/ems_alloc.c',
  'core/shared/mem-alloc/ems/ems_hmu.c',
  'core/shared/mem-alloc/ems/ems_kfc.c',
  'core/shared/mem-alloc/mem_alloc.c',
  'core/shared/utils/bh_assert.c',
  'core/shared/utils/bh_bitmap.c',
  'core/shared/utils/bh_common.c',
  'core/shared/utils/bh_hashmap.c',
  'core/shared/utils/bh_leb128.c',
  'core/shared/utils/bh_list.c',
  'core/shared/utils/bh_log.c',
  'core/shared/utils/bh_queue.c',
  'core/shared/utils/bh_vector.c',
  'core/shared/utils/runtime_timer.c',
)

if wamr_system == 'windows'
  wamr_c_args += [
    '-DBH_PLATFORM_WINDOWS',
    '-DHAVE_STRUCT_TIMESPEC',
    '-D_WINSOCK_DEPRECATED_NO_WARNINGS',
  ]
  wamr_srcs += files(
    'core/iwasm/common/arch/invokeNative_mingw_x64' + wamr_invoke_native_suffix,
    'core/shared/platform/common/libc-util/libc_errno.c',
    'core/shared/platform/common/memory/mremap.c',
    'core/shared/platform/windows/platform_init.c',
    'core/shared/platform/windows/win_clock.c',
    'core/shared/platform/windows/win_malloc.c',
    'core/shared/platform/windows/win_memmap.c',
    'core/shared/platform/windows/win_thread.c',
    'core/shared/platform/windows/win_time.c',
    'core/shared/platform/windows/win_util.c',
  )
  wamr_platform_inc = 'core/shared/platform/windows'
else
  wamr_srcs += files(
    'core/shared/platform/common/libc-util/libc_errno.c',
    'core/shared/platform/common/memory/mremap.c',
    'core/shared/platform/common/posix/posix_blocking_op.c',
    'core/shared/platform/common/posix/posix_clock.c',
    'core/shared/platform/common/posix/posix_malloc.c',
    'core/shared/platform/common/posix/posix_memmap.c',
    'core/shared/platform/common/posix/posix_sleep.c',
    'core/shared/platform/common/posix/posix_thread.c',
    'core/shared/platform/common/posix/posix_time.c',
  )
  if wamr_system == 'darwin'
    wamr_c_args += ['-DBH_PLATFORM_DARWIN']
    wamr_srcs += files(
      'core/iwasm/common/arch/invokeNative_osx_universal.s',
      'core/shared/platform/darwin/platform_init.c',
    )
    wamr_platform_inc = 'core/shared/platform/darwin'
  else
    wamr_c_args += ['-DBH_PLATFORM_LINUX', '-D_GNU_SOURCE']
    if host_machine.cpu_family() == 'aarch64'
      wamr_srcs += files('core/iwasm/common/arch/invokeNative_aarch64' + wamr_invoke_native_suffix)
    else
      wamr_srcs += files('core/iwasm/common/arch/invokeNative_em64' + wamr_invoke_native_suffix)
    endif
    wamr_srcs += files('core/shared/platform/linux/platform_init.c')
    wamr_platform_inc = 'core/shared/platform/linux'
  endif
endif

wamr_incs = include_directories(
  'core',
  'core/iwasm/include',
  'core/iwasm/common',
  'core/iwasm/interpreter',
  'core/shared/platform/include',
  'core/shared/platform/common/libc-util',
  wamr_platform_inc,
  'core/shared/utils',
  'core/shared/mem-alloc',
)

wamr_sys_deps = [
  cc.find_library('m', required: false),
  dependency('threads', required: false),
]

if have_wasm_simd
  wamr_c_args += ['-DWASM_ENABLE_SIMD=1', '-DWASM_ENABLE_SIMDE=1']
  wamr_sys_deps += simde_dep
endif

wamr_lib = static_library(
  'wamr',
  wamr_srcs,
  c_args: wamr_c_args,
  include_directories: wamr_incs,
  dependencies: wamr_sys_deps,
  gnu_symbol_visibility: 'hidden',
)

libwamr = declare_dependency(
  link_with: wamr_lib,
  dependencies: wamr_sys_deps,
  include_directories: include_directories('.', 'core/iwasm/include'),
  compile_args: ['-DNS_HAVE_WAMR=1'],
)

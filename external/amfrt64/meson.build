# amfrt64.dll: Neutron's AMD AMF runtime, a winelib DLL (ELF amfrt64.dll.so) built with OUR Wine's
# winegcc, like the nvenc bridge. Deployed as <prefix>/drive_c/windows/system32/amfrt64.dll with the
# DLL override amfrt64=native.
#
# The encoder behind it (vkenc.h) has two backends, both loaded with dlopen() at run time: Vulkan
# Video (vkenc.c) and VA-API (vaenc.c), with encoder.c choosing between them. Building needs the
# Vulkan headers and the libva headers (va/va.h, va/va_drm.h), but no link to either library.
#
#   PATH=<our wine tree>/bin:$PATH
#   meson setup build . --cross-file build-wine64.txt --buildtype release \
#       -Dvulkan_headers=$NEUTRON_EXT_WORK/src/dxvk/include/vulkan/include
#   ninja -C build                      -> build/amfrt64.dll.so (strip it when packaging)
#
# -Dtest_stub=true links the fake test backend instead (vkenc_stub.c); never ship that build.
#
# Copyright 2026 the Neutron project. LGPL-2.1-or-later.

project('amfrt64', ['c'], version: '0.1', license: 'LGPL-2.1-or-later',
        default_options: ['c_std=gnu11', 'warning_level=1'])

fs = import('fs')

# winegcc lists its own tree's include dirs after /usr/include, so an installed distro Wine's headers
# would win. Put OUR Wine's first. (-isystem, not -I: GCC drops an -I dir that is also a system dir.)
wine_include = fs.parent(fs.parent(find_program('winegcc').full_path())) / 'include'
add_project_arguments('-isystem', wine_include / 'wine' / 'windows', '-isystem', wine_include, language: 'c')

amfrt64_src = ['amfrt64.c']
amfrt64_deps = [dependency('threads')]

# The backend is plain Linux C, but winegcc compiles everything as Windows code: it defines _WIN32,
# _WIN64 and friends, and vk_platform.h then declares every PFN_vk* __stdcall, which winegcc maps to
# ms_abi. Calls into the host's libvulkan would pass their arguments in the wrong registers (it
# crashed in vkGetInstanceProcAddr, strcmp(NULL)). Build the backend on its own, without them.
backend_args = ['-U_WIN32', '-UWIN32', '-U__WIN32', '-U__WIN32__', '-U_WIN64', '-UWIN64', '-U__WIN64',
                '-U__WIN64__', '-U__WINNT', '-U__WINNT__', '-U__WINESRC__']

if get_option('test_stub')
  warning('TEST-ONLY build: linking the fake vkenc_stub.c backend. Never ship this DLL.')
  backend = static_library('vkenc_stub', 'vkenc_stub.c', c_args: backend_args, pic: true,
                           dependencies: amfrt64_deps)
else
  # The real backends: Vulkan Video (vkenc.c) and VA-API (vaenc.c), chosen per codec by encoder.c.
  # They dlopen() libvulkan.so.1 and libva.so.2 / libva-drm.so.2 at run time, so they need the headers
  # but no link: amfrt64 must load on machines without Vulkan or VA-API.
  cc = meson.get_compiler('c')
  vk_args = []
  if get_option('vulkan_headers') != ''
    vk_args = ['-I' + get_option('vulkan_headers')]
  endif
  cc.has_header('vulkan/vulkan.h', args: vk_args, required: true)
  cc.has_header('va/va.h', required: true)
  cc.has_header('va/va_drm.h', required: true)
  backend = static_library('vkenc', 'vkenc.c', 'vaenc.c', 'encoder.c', c_args: backend_args + vk_args,
                           pic: true, dependencies: amfrt64_deps)
  amfrt64_deps += cc.find_library('dl')
endif

shared_library('amfrt64.dll', amfrt64_src,
  name_prefix  : '',
  link_whole   : backend,
  dependencies : amfrt64_deps,
  objects      : 'amfrt64.spec',
  link_args    : ['-lole32'],
  install      : true)

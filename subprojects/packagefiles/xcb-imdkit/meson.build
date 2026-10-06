# Meson build for xcb-imdkit, transcribed from its CMake build.
#
# Upstream's CMakeLists requires ECM (KDE Extra CMake Modules), which the WSLg
# build container does not provide, so the CMake subproject cannot be
# configured there.  Meson applies this directory on top of the fetched
# sources (see subprojects/xcb-imdkit.wrap), which lets the library be built
# with nothing but xcb and xcb-util.
#
# Upstream sources and dependencies (CMakeLists.txt + src/CMakeLists.txt):
#   add_library(xcb-imdkit SHARED
#       parser.c ximproto.c imdkit.c protocolhandler.c message.c common.c
#       imclient.c clientprotocolhandler.c encoding.c
#       xlibi18n/lcCT.c xlibi18n/lcUTF8.c xlibi18n/lcCharSet.c)
#   target_link_libraries(xcb-imdkit PUBLIC XCB::XCB PRIVATE XCB::UTIL UTHash)
#   add_definitions(-D_GNU_SOURCE)
#   generate_export_header(xcb-imdkit BASE_NAME XCBImdkit)

project('xcb-imdkit', 'c',
	version: '1.0.9',
	license: 'LGPL-2.1-only',
	default_options: ['c_std=c11', 'warning_level=1'])

cc = meson.get_compiler('c')
fs = import('fs')

dep_xcb = dependency('xcb')

# The release tarball has a single top level directory (xcb-imdkit-master/).
# meson normally renames it to the wrap's `directory`, but do not rely on that:
# detect which layout we ended up with.
root = '.'
if not fs.is_dir('src') and fs.is_dir('xcb-imdkit-master')
	root = 'xcb-imdkit-master'
elif not fs.is_dir('src')
	error('xcb-imdkit sources not found: no src/ directory in the subproject')
endif

# Only the server side is needed (the xim-server module talks to X clients).
# That drops imclient.c and clientprotocolhandler.c, and with them upstream's
# private dependency on xcb-util and on the uthash *git submodule*, which is
# absent from the tarball.
srcs = [
	root / 'src/parser.c',
	root / 'src/ximproto.c',
	root / 'src/imdkit.c',
	root / 'src/protocolhandler.c',
	root / 'src/message.c',
	root / 'src/common.c',
	root / 'src/encoding.c',
	root / 'src/xlibi18n/lcCT.c',
	root / 'src/xlibi18n/lcUTF8.c',
	root / 'src/xlibi18n/lcCharSet.c',
]

# Header layout: imdkit.h includes "xcbimdkit_export.h" and "ximcommon.h"
# directly, so both the generated file and src/ must be on the include path.
#
# We deliberately do not build with -fvisibility=hidden: the header's export
# macros are empty, so hiding symbols would make the library unusable.
xcb_imdkit_inc = include_directories('.', root, root / 'src')

inc_dirs = [xcb_imdkit_inc]
if fs.is_dir(root / 'uthash')
	inc_dirs += include_directories(root / 'uthash', is_system: true)
endif

xcb_imdkit_lib = shared_library(
	'xcb-imdkit',
	srcs,
	include_directories: inc_dirs,
	dependencies: [dep_xcb],
	c_args: ['-D_GNU_SOURCE'],
	version: '1.0.9',
	soversion: 1,
	install: true,
)

xcb_imdkit_dep = declare_dependency(
	link_with: xcb_imdkit_lib,
	include_directories: xcb_imdkit_inc,
	dependencies: [dep_xcb],
)

# Meson's dependency('xcb-imdkit', fallback: [...]) hook.
meson.override_dependency('xcb-imdkit', xcb_imdkit_dep)

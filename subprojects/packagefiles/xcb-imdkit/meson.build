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

dep_xcb = dependency('xcb')
dep_xcb_util = dependency('xcb-util', required: false)
if not dep_xcb_util.found()
	dep_xcb_util = cc.find_library('xcb-util', required: false)
endif

# uthash is vendored inside the upstream tree.
inc_uthash = include_directories('uthash', is_system: true)

srcs = [
	'src/parser.c',
	'src/ximproto.c',
	'src/imdkit.c',
	'src/protocolhandler.c',
	'src/message.c',
	'src/common.c',
	'src/imclient.c',
	'src/clientprotocolhandler.c',
	'src/encoding.c',
	'src/xlibi18n/lcCT.c',
	'src/xlibi18n/lcUTF8.c',
	'src/xlibi18n/lcCharSet.c',
]

# Header layout: imdkit.h includes "xcbimdkit_export.h" and "ximcommon.h"
# directly, so both the generated file and src/ must be on the include path.
#
# We deliberately do not build with -fvisibility=hidden: the header's export
# macros are empty, so hiding symbols would make the library unusable.
xcb_imdkit_inc = include_directories('.', 'src')

xcb_imdkit_lib = shared_library(
	'xcb-imdkit',
	srcs,
	include_directories: [xcb_imdkit_inc, inc_uthash],
	dependencies: [dep_xcb, dep_xcb_util],
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

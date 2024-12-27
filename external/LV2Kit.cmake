
add_library(lv2kit STATIC)

set(LV2KIT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/lv2kit/subprojects")

if(WIN32)
	set(lv2kit_COMPILE_OPTIONS_PLAT
			# zix
			-DWIN32_LEAN_AND_MEAN
	)
	set(lv2kit_SOURCES_PLAT
			${LV2KIT_SRC_DIR}/zix/src/win32/environment_win32.c
			${LV2KIT_SRC_DIR}/zix/src/win32/filesystem_win32.c
			${LV2KIT_SRC_DIR}/zix/src/win32/sem_win32.c
			${LV2KIT_SRC_DIR}/zix/src/win32/system_win32.c
			${LV2KIT_SRC_DIR}/zix/src/win32/thread_win32.c
			${LV2KIT_SRC_DIR}/zix/src/win32/win32_util.c
	)
else()
	set(lv2kit_SOURCES_PLAT
			${LV2KIT_SRC_DIR}/zix/src/posix/environment_posix.c
			${LV2KIT_SRC_DIR}/zix/src/posix/filesystem_posix.c
			${LV2KIT_SRC_DIR}/zix/src/posix/system_posix.c
			${LV2KIT_SRC_DIR}/zix/src/posix/thread_posix.c
	)
	if(APPLE)
		set(lv2kit_COMPILE_OPTIONS_PLAT
				# zix
				-D_DARWIN_C_SOURCE
		)
		set(lv2kit_SOURCES_PLAT
				${lv2kit_SOURCES_PLAT}
				${LV2KIT_SRC_DIR}/zix/src/darwin/sem_darwin.c
		)
		set(lv2kit_COMPILE_OPTIONS
				-D_DARWIN_C_SOURCE
		)
	else()
		# FIXME: we will have to fix this when we support non-Linux desktops
		set(lv2kit_COMPILE_OPTIONS_PLAT
				# zix
				-D_GNU_SOURCE
				-D_POSIX_C_SOURCE=200809L
				-D_XOPEN_SOURCE=600
		)
		set(lv2kit_SOURCES_PLAT
				${lv2kit_SOURCES_PLAT}
				${LV2KIT_SRC_DIR}/zix/src/posix/sem_posix.c
		)
	endif()
endif()

target_compile_options(lv2kit PRIVATE
		-DZIX_STATIC
		-DZIX_INTERNAL
		# it seems to violate something in zix and causes assertion failure
		#-DZIX_NO_DEFAULT_CONFIG=1
		# error: call to undeclared function 'posix_fadvise'; ISO C99 and later do not support implicit function declarations [-Wimplicit-function-declaration]
		-DHAVE_POSIX_FADVISE=0
		${lv2kit_COMPILE_OPTIONS_PLAT}
)

set(LV2KIT_INCLUDE_DIRS
		${LV2KIT_SRC_DIR}/lv2/include
		${LV2KIT_SRC_DIR}/serd/include
		${LV2KIT_SRC_DIR}/sord/include
		${LV2KIT_SRC_DIR}/sratom/include
		${LV2KIT_SRC_DIR}/lilv/include
		${LV2KIT_SRC_DIR}/zix/include
)

target_include_directories(lv2kit PRIVATE ${LV2KIT_INCLUDE_DIRS})

target_sources(lv2kit PRIVATE
		${LV2KIT_SRC_DIR}/lilv/src/collections.c
		${LV2KIT_SRC_DIR}/lilv/src/instance.c
		${LV2KIT_SRC_DIR}/lilv/src/dylib.c
		${LV2KIT_SRC_DIR}/lilv/src/lib.c
		${LV2KIT_SRC_DIR}/lilv/src/node.c
		${LV2KIT_SRC_DIR}/lilv/src/plugin.c
		${LV2KIT_SRC_DIR}/lilv/src/pluginclass.c
		${LV2KIT_SRC_DIR}/lilv/src/port.c
		${LV2KIT_SRC_DIR}/lilv/src/query.c
		${LV2KIT_SRC_DIR}/lilv/src/scalepoint.c
		${LV2KIT_SRC_DIR}/lilv/src/state.c
		${LV2KIT_SRC_DIR}/lilv/src/ui.c
		${LV2KIT_SRC_DIR}/lilv/src/util.c
		${LV2KIT_SRC_DIR}/lilv/src/world.c
		${LV2KIT_SRC_DIR}/serd/src/base64.c
		${LV2KIT_SRC_DIR}/serd/src/byte_source.c
		${LV2KIT_SRC_DIR}/serd/src/env.c
		${LV2KIT_SRC_DIR}/serd/src/n3.c
		${LV2KIT_SRC_DIR}/serd/src/node.c
		${LV2KIT_SRC_DIR}/serd/src/reader.c
		#${LV2KIT_SRC_DIR}/serd/src/serdi.c
		${LV2KIT_SRC_DIR}/serd/src/string.c
		${LV2KIT_SRC_DIR}/serd/src/system.c
		${LV2KIT_SRC_DIR}/serd/src/uri.c
		${LV2KIT_SRC_DIR}/serd/src/writer.c
		${LV2KIT_SRC_DIR}/sord/src/sord.c
		#${LV2KIT_SRC_DIR}/sord/src/sord_validate.c
		#${LV2KIT_SRC_DIR}/sord/src/sordi.c
		${LV2KIT_SRC_DIR}/sord/src/syntax.c
		${LV2KIT_SRC_DIR}/sratom/src/sratom.c
		${LV2KIT_SRC_DIR}/zix/src/allocator.c
		${LV2KIT_SRC_DIR}/zix/src/btree.c
		${LV2KIT_SRC_DIR}/zix/src/bump_allocator.c
		${LV2KIT_SRC_DIR}/zix/src/digest.c
		${LV2KIT_SRC_DIR}/zix/src/errno_status.c
		${LV2KIT_SRC_DIR}/zix/src/filesystem.c
		${LV2KIT_SRC_DIR}/zix/src/hash.c
		${LV2KIT_SRC_DIR}/zix/src/path.c
		${LV2KIT_SRC_DIR}/zix/src/ring.c
		${LV2KIT_SRC_DIR}/zix/src/status.c
		${LV2KIT_SRC_DIR}/zix/src/string_view.c
		${LV2KIT_SRC_DIR}/zix/src/system.c
		${LV2KIT_SRC_DIR}/zix/src/tree.c
		${lv2kit_SOURCES_PLAT}
)

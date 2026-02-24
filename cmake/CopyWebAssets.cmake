# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copies the browser shell assets for the wasm build into the build tree,
# skipping the emsdk checkout which lives under tools/uapmd-app/web/.emsdk.

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "CopyWebAssets.cmake requires SRC and DST variables.")
endif()

if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "CopyWebAssets.cmake: '${SRC}' is not a directory.")
endif()

file(MAKE_DIRECTORY "${DST}")

file(GLOB WEB_ENTRIES RELATIVE "${SRC}" "${SRC}/*")
foreach(entry IN LISTS WEB_ENTRIES)
    if(entry MATCHES "^\\.emsdk($|/)")
        continue()
    endif()
    set(src_path "${SRC}/${entry}")
    set(dst_path "${DST}/${entry}")
    if(IS_DIRECTORY "${src_path}")
        file(MAKE_DIRECTORY "${dst_path}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${src_path}" "${dst_path}")
    else()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src_path}" "${dst_path}")
    endif()
endforeach()

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copies only the browser runtime assets for the wasm build into the build tree.
# Everything else under source/tools/uapmd-app/web is treated as non-runtime
# support material and must not be deployed as part of the app build.

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "CopyWebAssets.cmake requires SRC and DST variables.")
endif()

if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "CopyWebAssets.cmake: '${SRC}' is not a directory.")
endif()

set(WEB_RUNTIME_ASSETS
    "index.html"
    "coop-coep-sw.js"
    "audioworklet-env-fix.js"
    "uapmd-webclap-worklet.js"
)

file(MAKE_DIRECTORY "${DST}")

foreach(entry IN LISTS WEB_RUNTIME_ASSETS)
    set(src_path "${SRC}/${entry}")
    set(dst_path "${DST}/${entry}")
    if(NOT EXISTS "${src_path}")
        message(FATAL_ERROR "CopyWebAssets.cmake: missing required runtime asset '${src_path}'.")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src_path}" "${dst_path}")
endforeach()

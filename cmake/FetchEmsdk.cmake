cmake_minimum_required(VERSION 3.21)

set(UAPMD_EMSDK_GIT_TAG "main" CACHE STRING "emsdk git tag (or branch) to checkout")
set(UAPMD_EMSDK_INSTALL_TARGET "latest" CACHE STRING "emsdk install target (e.g. latest or 3.1.62)")
set(UAPMD_EMSDK_INFO_FILE "" CACHE FILEPATH "Optional file to write shell-friendly variables")
set(UAPMD_EMSDK_CACHE_DIR "" CACHE PATH "Location to cache emsdk checkouts; defaults to <repo>/tools/uapmd-app/web/.emsdk")
set(UAPMD_EMSDK_FORCE_REINSTALL OFF CACHE BOOL "Force reinstall even if Emscripten is already present")

if(NOT UAPMD_EMSDK_CACHE_DIR)
    get_filename_component(_uapmd_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(UAPMD_EMSDK_CACHE_DIR "${_uapmd_repo_root}/tools/uapmd-app/web/.emsdk")
endif()

file(MAKE_DIRECTORY "${UAPMD_EMSDK_CACHE_DIR}")
set(ENV{CPM_SOURCE_CACHE} "${UAPMD_EMSDK_CACHE_DIR}")

include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")
CPMAddPackage(
    NAME emsdk
    GITHUB_REPOSITORY emscripten-core/emsdk
    GIT_TAG ${UAPMD_EMSDK_GIT_TAG}
    DOWNLOAD_ONLY YES
)

if(NOT TARGET emsdk)
    # CPMAddPackage populates emsdk_SOURCE_DIR
endif()

set(emsdk_dir "${emsdk_SOURCE_DIR}")
file(TO_CMAKE_PATH "${emsdk_dir}" emsdk_dir_normalized)

if(NOT EXISTS "${emsdk_dir}/emsdk.py")
    message(FATAL_ERROR "emsdk checkout at ${emsdk_dir} is missing emsdk.py")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_emsdk_ready FALSE)
if(EXISTS "${emsdk_dir}/upstream/emscripten/emcc" AND NOT UAPMD_EMSDK_FORCE_REINSTALL)
    set(_emsdk_ready TRUE)
endif()

if(NOT _emsdk_ready)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} emsdk.py install ${UAPMD_EMSDK_INSTALL_TARGET}
        WORKING_DIRECTORY "${emsdk_dir}"
        RESULT_VARIABLE _install_res
    )
    if(NOT _install_res EQUAL 0)
        message(FATAL_ERROR "emsdk install failed with code ${_install_res}")
    endif()

    execute_process(
        COMMAND ${Python3_EXECUTABLE} emsdk.py activate ${UAPMD_EMSDK_INSTALL_TARGET}
        WORKING_DIRECTORY "${emsdk_dir}"
        RESULT_VARIABLE _activate_res
    )
    if(NOT _activate_res EQUAL 0)
        message(FATAL_ERROR "emsdk activate failed with code ${_activate_res}")
    endif()
endif()

set(env_script "${emsdk_dir}/emsdk_env.sh")
if(NOT EXISTS "${env_script}")
    message(FATAL_ERROR "emsdk_env.sh not found under ${emsdk_dir}")
endif()

if(UAPMD_EMSDK_INFO_FILE)
    get_filename_component(_info_dir "${UAPMD_EMSDK_INFO_FILE}" DIRECTORY)
    if(_info_dir)
        file(MAKE_DIRECTORY "${_info_dir}")
    endif()
    file(WRITE "${UAPMD_EMSDK_INFO_FILE}"
        "UAPMD_EMSDK_ROOT=\"${emsdk_dir_normalized}\"\n"
        "UAPMD_EMSDK_ENV_SCRIPT=\"${env_script}\"\n")
endif()

message(STATUS "emsdk ready at ${emsdk_dir_normalized}")

include_guard(GLOBAL)

include(FetchContent)

macro(uapmd_prepare_vcpkg_sdl3)
    if(NOT WIN32)
        return()
    endif()

    if(TARGET SDL3::SDL3 OR TARGET SDL3::SDL3-static OR TARGET SDL3::SDL3-shared)
        return()
    endif()

    if(DEFINED SDL3_DIR AND EXISTS "${SDL3_DIR}/SDL3Config.cmake")
        return()
    endif()

    if(NOT DEFINED UAPMD_VCPKG_TRIPLET OR UAPMD_VCPKG_TRIPLET STREQUAL "")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm].*64$")
            set(UAPMD_VCPKG_TRIPLET "arm64-windows")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(UAPMD_VCPKG_TRIPLET "x86-windows")
        else()
            set(UAPMD_VCPKG_TRIPLET "x64-windows")
        endif()
    endif()
    set(UAPMD_VCPKG_TRIPLET "${UAPMD_VCPKG_TRIPLET}"
        CACHE STRING "vcpkg triplet used when fetching SDL3 automatically" FORCE)

    set(_uapmd_vcpkg_root "")
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/vcpkg.exe")
        set(_uapmd_vcpkg_root "$ENV{VCPKG_ROOT}")
    else()
        set(_uapmd_vcpkg_default_url
            "https://github.com/microsoft/vcpkg/archive/refs/tags/2024.05.24.tar.gz")
        if(NOT DEFINED UAPMD_VCPKG_URL OR UAPMD_VCPKG_URL STREQUAL "")
            set(UAPMD_VCPKG_URL "${_uapmd_vcpkg_default_url}")
        endif()
        set(UAPMD_VCPKG_URL "${UAPMD_VCPKG_URL}"
            CACHE STRING "URL used to download vcpkg for SDL3 on Windows" FORCE)
        if(NOT DEFINED UAPMD_VCPKG_URL_HASH)
            set(UAPMD_VCPKG_URL_HASH "")
        endif()
        set(UAPMD_VCPKG_URL_HASH "${UAPMD_VCPKG_URL_HASH}"
            CACHE STRING "Optional hash to validate the vcpkg download" FORCE)

        set(_uapmd_vcpkg_hash_args)
        if(UAPMD_VCPKG_URL_HASH)
            list(APPEND _uapmd_vcpkg_hash_args URL_HASH ${UAPMD_VCPKG_URL_HASH})
        endif()

        FetchContent_Declare(uapmd_vcpkg
            URL ${UAPMD_VCPKG_URL}
            ${_uapmd_vcpkg_hash_args}
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        )
        FetchContent_GetProperties(uapmd_vcpkg)
        if(NOT uapmd_vcpkg_POPULATED)
            FetchContent_Populate(uapmd_vcpkg)
        endif()
        set(_uapmd_vcpkg_root "${uapmd_vcpkg_SOURCE_DIR}")
    endif()

    if(NOT _uapmd_vcpkg_root)
        message(WARNING "uapmd-app: unable to determine vcpkg root directory, SDL3 auto-download skipped")
        return()
    endif()
    set(UAPMD_VCPKG_ROOT "${_uapmd_vcpkg_root}" CACHE PATH
        "Path to the vcpkg tree used to supply SDL3" FORCE)

    set(_uapmd_vcpkg_exe "${_uapmd_vcpkg_root}/vcpkg.exe")
    set(_uapmd_vcpkg_bootstrap "${_uapmd_vcpkg_root}/bootstrap-vcpkg.bat")

    if(NOT EXISTS "${_uapmd_vcpkg_exe}")
        if(NOT EXISTS "${_uapmd_vcpkg_bootstrap}")
            message(WARNING
                "uapmd-app: bootstrap script missing in ${_uapmd_vcpkg_root}, SDL3 auto-download skipped")
            return()
        endif()
        execute_process(
            COMMAND cmd /c "${_uapmd_vcpkg_bootstrap}" -disableMetrics
            WORKING_DIRECTORY "${_uapmd_vcpkg_root}"
            RESULT_VARIABLE _uapmd_vcpkg_bootstrap_result
        )
        if(NOT _uapmd_vcpkg_bootstrap_result EQUAL 0)
            message(WARNING
                "uapmd-app: vcpkg bootstrap failed (${_uapmd_vcpkg_bootstrap_result}), SDL3 auto-download skipped")
            return()
        endif()
    endif()

    set(_uapmd_vcpkg_triplet_dir
        "${_uapmd_vcpkg_root}/installed/${UAPMD_VCPKG_TRIPLET}")
    set(_uapmd_vcpkg_sdl3_config
        "${_uapmd_vcpkg_triplet_dir}/share/SDL3/SDL3Config.cmake")
    if(NOT EXISTS "${_uapmd_vcpkg_sdl3_config}")
        execute_process(
            COMMAND "${_uapmd_vcpkg_exe}" install "sdl3:${UAPMD_VCPKG_TRIPLET}" --clean-after-build
            WORKING_DIRECTORY "${_uapmd_vcpkg_root}"
            RESULT_VARIABLE _uapmd_vcpkg_install_result
        )
        if(NOT _uapmd_vcpkg_install_result EQUAL 0)
            message(WARNING
                "uapmd-app: failed to install SDL3 via vcpkg (${_uapmd_vcpkg_install_result}), falling back to other backends")
            return()
        endif()
    endif()

    if(NOT EXISTS "${_uapmd_vcpkg_sdl3_config}")
        message(WARNING "uapmd-app: SDL3Config.cmake missing after vcpkg install, falling back to other backends")
        return()
    endif()

    list(APPEND CMAKE_PREFIX_PATH "${_uapmd_vcpkg_triplet_dir}")
    set(SDL3_DIR "${_uapmd_vcpkg_triplet_dir}/share/SDL3"
        CACHE PATH "SDL3 config directory supplied by vcpkg" FORCE)
endmacro()

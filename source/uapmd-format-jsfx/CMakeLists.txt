# JSFX plugin format, hosted by ysfx.
#
# This module is an application-provided AudioPluginFormat: it depends on
# uapmd-plugin-hosting but nothing in uapmd-plugin-hosting depends on it, which keeps ysfx,
# WDL and SWELL out of the core hosting module.

# The JSFX runtime compiles EEL2 to machine code. That is not permitted on iOS and not
# possible on wasm, so those platforms need EEL2's interpreter instead. Android is aarch64
# and should JIT, but that is not verified yet.
if(IOS OR EMSCRIPTEN)
    set(_UAPMD_YSFX_PORTABLE ON)
else()
    set(_UAPMD_YSFX_PORTABLE OFF)
endif()

# LICE's font rendering and its HDC-backed bitmap are the only parts of the graphics stack
# that need SWELL. Desktop keeps them, so that JSFX text renders exactly as it does in
# REAPER. Everywhere else we compile them out and supply our own.
if(ANDROID OR IOS OR EMSCRIPTEN)
    set(_UAPMD_JSFX_OWN_FONTS ON)
else()
    set(_UAPMD_JSFX_OWN_FONTS OFF)
endif()
option(UAPMD_JSFX_OWN_FONTS
        "Use the built-in font backend instead of the platform's, on platforms that have one"
        ${_UAPMD_JSFX_OWN_FONTS})

# The editor draws everything through LICE, which is portable apart from its font
# rendering and its device-context-backed bitmap. Where those cannot be had we supply both
# ourselves, so graphics is on either way.
set(_UAPMD_YSFX_GFX ON)

# Two changes to ysfx. Its `dr_libs`, `stb` and `json` interface targets have names
# generic enough to collide in a build this large -- rtlog-cpp already provides `stb` --
# so the patch guards them with `if(NOT TARGET ...)` and whichever dependency gets there
# first wins. Reusing rtlog-cpp's `stb` is safe because it fetches upstream nothings/stb,
# which carries the headers LICE's image loaders want, and stb_truetype besides.
#
# The patch also adds YSFX_EXTERNAL_FONT, which leaves out LICE's device-context font and
# routes font creation to the host instead.
set(_UAPMD_YSFX_PATCH "${UAPMD_EXTERNAL_DIR}/patches/ysfx-uapmd-integration.patch")
set(_UAPMD_YSFX_PATCH_EPOCH "5")
file(SHA1 "${_UAPMD_YSFX_PATCH}" _UAPMD_YSFX_PATCH_SHA1)

CPMAddPackage(
        NAME            ysfx
        GIT_TAG         b60955593a2917b493b6372cf92a9c9db165d464
        GIT_REPOSITORY  https://github.com/JoepVanlier/ysfx.git
        GIT_SUBMODULES  "thirdparty/dr_libs"
        CUSTOM_CACHE_KEY "b60955593a2917b493b6372cf92a9c9db165d464-v${_UAPMD_YSFX_PATCH_EPOCH}-${_UAPMD_YSFX_PATCH_SHA1}"
        PATCHES         "${_UAPMD_YSFX_PATCH}"
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "YSFX_PLUGIN OFF"
            "YSFX_TESTS OFF"
            "YSFX_TOOLS OFF"
            "YSFX_GFX ${_UAPMD_YSFX_GFX}"
            "YSFX_PORTABLE ${_UAPMD_YSFX_PORTABLE}"
            # ysfx verifies that its GAS port of EEL2's x86-64 assembly still matches
            # upstream WDL, using paths that only resolve when ysfx is the top-level
            # project. We pin a commit, so the check has nothing to tell us.
            "YSFX_SKIP_CHECKSUM ON"
            # Drops LICE_CachedFont, and with it the only reason the graphics stack needs
            # SWELL; font creation is routed to us instead.
            "YSFX_EXTERNAL_FONT ${UAPMD_JSFX_OWN_FONTS}"
)

add_library(uapmd-format-jsfx STATIC)

# ysfx only defines YSFX_NO_GFX on its private target, so the graphics decision is
# restated here for our own sources: without it they would offer an editor that can never
# draw anything.
target_compile_definitions(uapmd-format-jsfx PRIVATE UAPMD_JSFX_HAS_GFX=1)
if(UAPMD_JSFX_OWN_FONTS)
    target_compile_definitions(uapmd-format-jsfx PRIVATE UAPMD_JSFX_OWN_FONTS=1 _LICE_NO_SYSBITMAPS_)
    # stb_truetype comes from the same upstream stb the rest of the build already uses.
    if(TARGET stb)
        target_link_libraries(uapmd-format-jsfx PRIVATE stb)
    endif()
else()
    target_compile_definitions(uapmd-format-jsfx PRIVATE UAPMD_JSFX_OWN_FONTS=0)
endif()

target_include_directories(uapmd-format-jsfx PUBLIC include)
target_include_directories(uapmd-format-jsfx PRIVATE
        ../remidy/include
        ../uapmd-plugin-hosting/include
        ${choc_SOURCE_DIR}
)

target_link_libraries(uapmd-format-jsfx PRIVATE
        uapmd::uapmd-plugin-hosting
        remidy::remidy
        ysfx::ysfx
)

if(UAPMD_JSFX_OWN_FONTS)
    target_sources(uapmd-format-jsfx PRIVATE src/JsfxFontBackend.cpp)
endif()

target_sources(uapmd-format-jsfx PRIVATE
        src/JsfxFramebufferUI.cpp
        src/JsfxGfxScheduler.cpp
        src/JsfxPluginFormat.cpp
        src/JsfxPluginInstance.cpp
        src/JsfxScanning.cpp
        src/JsfxStateCodec.cpp
)

add_library(uapmd::uapmd-format-jsfx ALIAS uapmd-format-jsfx)

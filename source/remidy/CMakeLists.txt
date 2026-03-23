
if(APPLE)
    set(REMIDY_SUPPORT_AU ON)
endif()
if(NOT ANDROID AND NOT EMSCRIPTEN AND NOT IOS)
    set(REMIDY_SUPPORT_VST3 ON)
    set(REMIDY_SUPPORT_LV2 ON)
    set(REMIDY_SUPPORT_CLAP ON)
endif ()
if (ANDROID)
    set(REMIDY_SUPPORT_AAP ON)
endif ()
if (EMSCRIPTEN)
    set(REMIDY_SUPPORT_WEBCLAP ON)
endif ()
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    set (REMIDY_LINUX_DESKTOP ON)
endif ()
if (UNIX AND NOT APPLE AND WAYLAND_CLIENT_FOUND)
    target_compile_definitions(remidy PRIVATE
            HAVE_WAYLAND=1
    )
endif ()


if (${REMIDY_SUPPORT_WEBCLAP})
    set(REMIDY_WEBCLAP_SUPPORT_SOURCES
            src/webclap/PluginFormatWebCLAP.cpp
    )
endif ()

if (${REMIDY_SUPPORT_AAP})
    set(REMIDY_AAP_SUPPORT_SOURCES
            src/aap/PluginFormatAAP.cpp
            src/aap/PluginInstanceAAP.cpp
            src/aap/PluginInstanceAAP.Buses.cpp
            src/aap/PluginInstanceAAP.Parameters.cpp
            src/aap/PluginInstanceAAP.Presets.cpp
            src/aap/PluginInstanceAAP.States.cpp
            src/aap/PluginInstanceAAP.UI.cpp
    )
    set(AAP_LIBRARIES
        androidaudioplugin::androidaudioplugin
    )

    if ("${AAP_DIR}" STREQUAL "")
        error("AAP_DIR is not defined")
    else ()
        message("AAP_DIR:${AAP_DIR}")
    endif ()

    set(androidndk_LIBRARIES android)
    set(aap_LIBRARIES androidaudioplugin)

    find_package(androidaudioplugin REQUIRED)
endif ()

if (${REMIDY_LINUX_DESKTOP})
    find_package(PkgConfig REQUIRED)
    find_package(X11 REQUIRED)

    # Wayland support (optional)
    pkg_check_modules(WAYLAND_CLIENT wayland-client)
    if(WAYLAND_CLIENT_FOUND)
        message(STATUS "Wayland client found - enabling Wayland support")
    else()
        message(WARNING "Wayland client not found - Wayland support disabled")
    endif()
endif()

CPMAddPackage(
        NAME rtlog-cpp
        GIT_REPOSITORY https://github.com/cjappl/rtlog-cpp
        GIT_TAG 36381f9b2d64f7852590dd0c2564dbfdee90e116
        EXCLUDE_FROM_ALL
)

if(${REMIDY_SUPPORT_LV2})
    CPMAddPackage(
            NAME lv2kit
            GIT_REPOSITORY https://github.com/lv2/lv2kit/
            GIT_TAG 0c612fa
            DOWNLOAD_ONLY YES
    )
    set(LV2KIT_SRC_DIR ${lv2kit_SOURCE_DIR}/subprojects) # used by LV2Kit.cmake
    include("../cmake/LV2Kit.cmake")
    set(lv2kit_LIBRARY lv2kit)
    set(REMIDY_LV2_SUPPORT_SOURCES
            src/lv2/LV2Helper.cpp
            src/lv2/symap.cpp
            src/lv2/PluginFormatLV2.cpp
            src/lv2/PluginInstanceLV2.cpp
            src/lv2/PluginInstanceLV2.Buses.cpp
            src/lv2/PluginInstanceLV2.Events.cpp
            src/lv2/PluginInstanceLV2.Parameters.cpp
            src/lv2/PluginInstanceLV2.Presets.cpp
            src/lv2/PluginInstanceLV2.States.cpp
            src/lv2/PluginInstanceLV2.UI.cpp
    )
endif()

if (${REMIDY_SUPPORT_CLAP})
    CPMAddPackage(
            NAME clap
            GIT_REPOSITORY https://github.com/free-audio/clap
            GIT_TAG 1.2.6
            DOWNLOAD_ONLY YES
    )

    CPMAddPackage(
            NAME clap-helpers
            GIT_REPOSITORY https://github.com/free-audio/clap-helpers
            GIT_TAG 58ab81b1dc8219e859529c1306f364bb3aedf7d5
            DOWNLOAD_ONLY YES
    )

    set(REMIDY_CLAP_SUPPORT_INCLUDE_DIRS
            ${clap_SOURCE_DIR}/include
            ${clap-helpers_SOURCE_DIR}/include
    )

    set(REMIDY_CLAP_SUPPORT_SOURCES
            src/clap/CLAPHelper.cpp
            src/clap/HostClasses.cpp
            src/clap/PluginFormatCLAP.cpp
            src/clap/PluginInstanceCLAP.cpp
            src/clap/PluginInstanceCLAP.Buses.cpp
            src/clap/PluginInstanceCLAP.Events.cpp
            src/clap/PluginInstanceCLAP.Parameters.cpp
            src/clap/PluginInstanceCLAP.Presets.cpp
            src/clap/PluginInstanceCLAP.States.cpp
            src/clap/PluginInstanceCLAP.UI.cpp
    )
endif ()

if (REMIDY_SUPPORT_AU)
    # AUv3 sources: available on both macOS and iOS
    set(REMIDY_AU_SUPPORT_SOURCES
            src/au/PluginFormatAU.mm
            src/au/PluginInstanceAUv3.mm
            src/au/PluginInstanceAUv3.Buses.mm
            src/au/PluginInstanceAUv3.Events.mm
            src/au/PluginInstanceAUv3.Parameters.mm
            src/au/PluginInstanceAUv3.States.mm
            src/au/PluginInstanceAUv3.Presets.mm
            src/au/PluginInstanceAUv3.UI.mm
    )
    # AUv2 sources: macOS only (AUv2 does not exist on iOS)
    if(NOT IOS)
        list(APPEND REMIDY_AU_SUPPORT_SOURCES
                src/au/AUv2Helper.cpp
                src/au/PluginInstanceAUv2.mm
                src/au/PluginInstanceAUv2.Buses.mm
                src/au/PluginInstanceAUv2.Events.mm
                src/au/PluginInstanceAUv2.Parameters.mm
                src/au/PluginInstanceAUv2.Presets.mm
                src/au/PluginInstanceAUv2.States.mm
                src/au/PluginInstanceAUv2.UI.mm
        )
    endif()
endif ()

if (${REMIDY_SUPPORT_VST3})
    # it requires gtkmm-3.0 but we don't need it
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
    set(SMTG_ENABLE_WAYLAND_SUPPORT ON)

    CPMAddPackage(
            NAME            vst3sdk
            GIT_REPOSITORY  https://github.com/steinbergmedia/vst3sdk
            GIT_TAG         v3.8.0_build_66
            DOWNLOAD_ONLY   YES
    )

    set(VST3_PUBLIC_SDK_MODULE_INFO_DIR ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/moduleinfo)

    set(REMIDY_VST3_SUPPORT_SOURCES
            ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp
            ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/funknown.cpp
            ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/coreiids.cpp
            ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/ustring.cpp
            ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/conststringtable.cpp
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstinitiids.cpp
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/commoniids.cpp
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/commonstringconvert.cpp
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/utility/stringconvert.cpp

            src/vst3/VST3Helper.cpp
            src/vst3/HostClasses.cpp
            src/vst3/ClassModuleInfo.cpp
            src/vst3/LinuxRunLoopIIDs.cpp
            src/vst3/PluginFormatVST3.cpp
            src/vst3/PluginInstanceVST3.cpp
            src/vst3/PluginInstanceVST3.Buses.cpp
            src/vst3/PluginInstanceVST3.Events.cpp
            src/vst3/PluginInstanceVST3.Parameters.cpp
            src/vst3/PluginInstanceVST3.Presets.cpp
            src/vst3/PluginInstanceVST3.States.cpp
            src/vst3/PluginInstanceVST3.UI.cpp
    )
    set(vst3sdk_LIBRARY sdk) # pretty bad naming from vst3sdk (shrug)
endif ()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy_LIBS_PLAT
            "-framework CoreFoundation"
            "-framework CoreAudio"
            "-framework Foundation"
            "-framework AVFoundation"
    )
elseif(IOS)
    set(remidy_LIBS_PLAT
            "-framework CoreFoundation"
            "-framework CoreAudio"
            "-framework Foundation"
            "-framework AVFoundation"
            "-framework AudioToolbox"
    )
endif ()

if (WIN32)
    set(remidy_LIBS_PLAT ws2_32)
endif()


add_library(remidy STATIC)

target_link_libraries(remidy PUBLIC
        ${X11_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
)

target_include_directories(remidy PRIVATE
        ${AAP_DIR}/include
        ${choc_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${vst3sdk_SOURCE_DIR}
        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}
        ${LV2KIT_INCLUDE_DIRS}
        ${REMIDY_CLAP_SUPPORT_INCLUDE_DIRS}
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/include/remidy
)

target_link_libraries(remidy PRIVATE
        rtlog::rtlog
        ${cpplocate_LIBRARIES}
        midicci_static
        ${remidy_LIBS_PLAT}
        ${androidndk_LIBRARIES}
        ${AAP_LIBRARIES}
        ${lv2kit_LIBRARY}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    target_compile_definitions(remidy PRIVATE
            V3_COM_COMPAT=1
    )
endif ()

target_compile_options(remidy PRIVATE
        $<$<COMPILE_LANGUAGE:ObjCXX>:-framework CoreAudio -framework CoreFoundation>
        $<$<COMPILE_LANGUAGE:Swift>:-cxx-interoperability-mode=default -emit-clang-header-path ${CMAKE_BINARY_DIR}/generated/include/swift-interop/remidy-Swift.h>
        -DLILV_STATIC
        -DZIX_STATIC
)

target_sources(remidy PRIVATE
        src/common.cpp
        src/utils.cpp
        src/AudioProcessContext.cpp
        src/EventLoop.cpp
        src/Logger.cpp
        src/PluginCatalog.cpp
        src/PluginBundlePool.cpp
        src/PluginFormat.cpp
        src/UmpInputDispatcher.cpp

        ${REMIDY_VST3_SUPPORT_SOURCES}
        ${REMIDY_AU_SUPPORT_SOURCES}
        ${REMIDY_LV2_SUPPORT_SOURCES}
        ${REMIDY_CLAP_SUPPORT_SOURCES}
        ${REMIDY_AAP_SUPPORT_SOURCES}
        ${REMIDY_WEBCLAP_SUPPORT_SOURCES}
)

add_library(remidy::remidy ALIAS remidy)

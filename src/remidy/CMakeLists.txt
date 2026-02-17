
if (ANDROID)
    set(AAP_SUPPORT_SOURCES
            aap/PluginFormatAAP.cpp
            aap/PluginInstanceAAP.cpp
    )
    set(AAP_LIBRARIES
        androidaudioplugin::androidaudioplugin
    )

    if ("${AAP_DIR}" STREQUAL "")
        error("AAP_DIR is not defined")
    else ()
        message("AAP_DIR:${AAP_DIR}")
    endif ()
else ()
    include("../../external/LV2Kit.cmake")
    set(lv2kit_LIBRARY lv2kit)
    set(lv2kit_SOURCES
            lv2/LV2Helper.cpp
            lv2/symap.cpp
            lv2/PluginFormatLV2.cpp
            lv2/PluginInstanceLV2.cpp
            lv2/PluginInstanceLV2.Buses.cpp
            lv2/PluginInstanceLV2.Events.cpp
            lv2/PluginInstanceLV2.Parameters.cpp
            lv2/PluginInstanceLV2.Presets.cpp
            lv2/PluginInstanceLV2.States.cpp
            lv2/PluginInstanceLV2.UI.cpp
    )
endif ()

if(ANDROID)
    set(androidndk_LIBRARIES android)
    set(aap_LIBRARIES androidaudioplugin)

    find_package(androidaudioplugin REQUIRED)
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    find_package(PkgConfig REQUIRED)
    find_package(X11 REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

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

if (NOT ANDROID)
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

set(VST3_SUPPORT_SOURCES
        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/funknown.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/coreiids.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/ustring.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/conststringtable.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstinitiids.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/commoniids.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/commonstringconvert.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/utility/stringconvert.cpp

        vst3/VST3Helper.cpp
        vst3/HostClasses.cpp
        vst3/ClassModuleInfo.cpp
        vst3/LinuxRunLoopIIDs.cpp
        vst3/PluginFormatVST3.cpp
        vst3/PluginInstanceVST3.cpp
        vst3/PluginInstanceVST3.Buses.cpp
        vst3/PluginInstanceVST3.Events.cpp
        vst3/PluginInstanceVST3.Parameters.cpp
        vst3/PluginInstanceVST3.Presets.cpp
        vst3/PluginInstanceVST3.States.cpp
        vst3/PluginInstanceVST3.UI.cpp
)
set(vst3sdk_LIBRARY sdk) # pretty bad naming from vst3sdk (shrug)

endif (NOT ANDROID)

add_library(remidy STATIC)

target_link_libraries(remidy PUBLIC
        ${X11_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    target_compile_definitions(remidy PRIVATE
            V3_COM_COMPAT=1
    )
endif ()

if (UNIX AND NOT APPLE AND WAYLAND_CLIENT_FOUND)
    target_compile_definitions(remidy PRIVATE
            HAVE_WAYLAND=1
    )
endif ()

target_include_directories(remidy PRIVATE
        ${AAP_DIR}/include
        ${vst3sdk_SOURCE_DIR}
        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}
        ${choc_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
        ${LV2KIT_INCLUDE_DIRS}
        ${clap_SOURCE_DIR}/include
        ${clap-helpers_SOURCE_DIR}/include
        ${midicci_SOURCE_DIR}/include
        ../../include
        ../../include/remidy # FIXME: this should be removed
        ${CMAKE_BINARY_DIR}/generated/include
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy_LIBS_PLAT
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework Foundation"
        "-framework AVFoundation"
    )
endif ()

if (WIN32)
    target_link_libraries(remidy PRIVATE ws2_32)
endif()

target_link_libraries(remidy PRIVATE
        rtlog::rtlog
        ${cpplocate_LIBRARIES}
        ${lv2kit_LIBRARY}
        midicci_static
        ${remidy_LIBS_PLAT}
        ${androidndk_LIBRARIES}
        ${AAP_LIBRARIES}
)

target_compile_options(remidy PRIVATE
        #-fsanitize=address
        $<$<COMPILE_LANGUAGE:ObjCXX>:-framework CoreAudio -framework CoreFoundation>
        $<$<COMPILE_LANGUAGE:Swift>:-cxx-interoperability-mode=default -emit-clang-header-path ${CMAKE_BINARY_DIR}/generated/include/swift-interop/remidy-Swift.h>
        -DLILV_STATIC
        -DZIX_STATIC
)

if (APPLE)
    set(remidy_OBJC_SOURCES
            au/PluginFormatAU.mm
            au/PluginInstanceAUv2.mm
            au/PluginInstanceAUv2.Buses.mm
            au/PluginInstanceAUv2.Events.mm
            au/PluginInstanceAUv2.Parameters.mm
            au/PluginInstanceAUv2.Presets.mm
            au/PluginInstanceAUv2.States.mm
            au/PluginInstanceAUv2.UI.mm
            au/PluginInstanceAUv3.mm
            au/PluginInstanceAUv3.Buses.mm
            au/PluginInstanceAUv3.Events.mm
            au/PluginInstanceAUv3.Parameters.mm
            au/PluginInstanceAUv3.States.mm
            au/PluginInstanceAUv3.Presets.mm
            au/PluginInstanceAUv3.UI.mm
    )
endif ()

target_sources(remidy PRIVATE

        # swift sources generate C++ header files in use later, so compile them first.
        #src/auv2/AUv2Helper.swift

        utils.cpp
        EventLoop.cpp

        ${VST3_SUPPORT_SOURCES}
        ${AAP_SUPPORT_SOURCES}

        Logger.cpp
        AudioBufferList.cpp
        EventSequence.cpp

        PluginCatalog.cpp
        PluginBundlePool.cpp
        PluginFormat.cpp

        au/AUv2Helper.cpp
        ${remidy_OBJC_SOURCES}

        ${lv2kit_SOURCES}

        clap/CLAPHelper.cpp
        clap/HostClasses.cpp
        clap/PluginFormatCLAP.cpp
        clap/PluginInstanceCLAP.cpp
        clap/PluginInstanceCLAP.Buses.cpp
        clap/PluginInstanceCLAP.Events.cpp
        clap/PluginInstanceCLAP.Parameters.cpp
        clap/PluginInstanceCLAP.Presets.cpp
        clap/PluginInstanceCLAP.States.cpp
        clap/PluginInstanceCLAP.UI.cpp

        AudioProcessContext.cpp
        PluginInstance.cpp
        UmpInputDispatcher.cpp
)

add_library(remidy::remidy ALIAS remidy)


include("../../external/LV2Kit.cmake")

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    find_package(X11 REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
endif()

# FIXME: it would be slightly nicer if we could migrate to CPM (failed to resolve farbot if we actually do so. Also see https://github.com/hogliux/farbot/pull/15 )
# <rtlog>
FetchContent_Declare(rtlog-cpp
        GIT_REPOSITORY https://github.com/cjappl/rtlog-cpp
        GIT_TAG 36381f9b2d64f7852590dd0c2564dbfdee90e116
)
FetchContent_MakeAvailable(rtlog-cpp)
# </rtlog>

FetchContent_Declare(clap
        GIT_REPOSITORY https://github.com/free-audio/clap
        GIT_TAG 1.2.6
)
FetchContent_MakeAvailable(clap)

FetchContent_Declare(clap-helpers
        GIT_REPOSITORY https://github.com/free-audio/clap-helpers
        GIT_TAG 58ab81b1dc8219e859529c1306f364bb3aedf7d5
)
FetchContent_MakeAvailable(clap-helpers)

# it requires gtkmm-3.0 but we don't need it
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
set(SMTG_ENABLE_WAYLAND_SUPPORT ON)

FetchContent_Declare(vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk
        GIT_TAG v3.8.0_build_66
        PATCH_COMMAND git apply ${CMAKE_CURRENT_LIST_DIR}/../../cmake/vst3sdk-mac-terminate.patch
        UPDATE_DISCONNECTED 1
)
FetchContent_MakeAvailable(vst3sdk)

set(VST3_PUBLIC_SDK_MODULE_INFO_DIR ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/moduleinfo)

add_library(remidy STATIC)

target_compile_options(remidy PUBLIC
        ${GTK3_CFLAGS}
)
target_link_options(remidy PUBLIC
        ${GTK3_LDFLAGS}
)
target_link_libraries(remidy PUBLIC
        ${X11_LIBRARIES}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    target_compile_definitions(remidy PRIVATE
            V3_COM_COMPAT=1
    )
endif ()

target_include_directories(remidy PRIVATE
        ${vst3sdk_SOURCE_DIR}
        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}
        ${choc_SOURCE_DIR}
        ${LV2KIT_INCLUDE_DIRS}
        ${clap_SOURCE_DIR}/include
        ${clap-helpers_SOURCE_DIR}/include
        ${cmidi2_SOURCE_DIR}
        ../../include/remidy
        ${CMAKE_BINARY_DIR}/generated/include
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy_LIBS_PLAT
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework Foundation"
    )
endif ()

if (WIN32)
    target_link_libraries(remidy PRIVATE ws2_32)
endif()

target_link_libraries(remidy PRIVATE
        #libremidi::libremidi
        rtlog::rtlog
        cpplocate::cpplocate
        lv2kit
        sdk # pretty bad naming from vst3sdk (shrug)
        ${remidy_LIBS_PLAT}
        ${GTK3_LIBRARIES}
        #-fsanitize=address
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
            au/PluginInstanceAU.mm
            au/PluginInstanceAU.Buses.mm
            au/PluginInstanceAU.Events.mm
            au/PluginInstanceAU.Parameters.mm
            au/PluginInstanceAU.Presets.mm
            au/PluginInstanceAU.States.mm
            au/PluginInstanceAU.UI.mm
    )
endif ()

target_sources(remidy PRIVATE

        # swift sources generate C++ header files in use later, so compile them first.
        #src/auv2/AUv2Helper.swift

        utils.cpp
        EventLoop.cpp

        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/funknown.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/coreiids.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/ustring.cpp
        ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/conststringtable.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstinitiids.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/commoniids.cpp

        Logger.cpp
        AudioBufferList.cpp
        EventSequence.cpp

        PluginCatalog.cpp
        PluginBundlePool.cpp
        PluginFormat.cpp

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

        au/AUv2Helper.cpp
        ${remidy_OBJC_SOURCES}

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

cmake_minimum_required(VERSION 3.18)
if (APPLE)
project(remidy LANGUAGES C CXX OBJC OBJCXX Swift)
else()
project(remidy LANGUAGES C CXX)
endif()

include("../../external/LV2Kit.cmake")

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
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

set(VST3_PUBLIC_SDK_MODULE_INFO_DIR ../../external/vst3-public-sdk-moduleinfo)

add_library(remidy STATIC)

target_compile_options(remidy PUBLIC
        ${GTK3_CFLAGS}
)
target_link_options(remidy PUBLIC
        ${GTK3_LDFLAGS}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    target_compile_definitions(remidy PRIVATE
            V3_COM_COMPAT=1
    )
endif ()

target_include_directories(remidy PRIVATE
        ../../external/travesty
        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}
        ../../external/choc
        ${LV2KIT_INCLUDE_DIRS}
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

target_link_libraries(remidy PRIVATE
        #libremidi::libremidi
        rtlog::rtlog
        cpplocate::cpplocate
        lv2kit
        ${remidy_LIBS_PLAT}
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
    )
endif ()

target_sources(remidy PRIVATE

        # swift sources generate C++ header files in use later, so compile them first.
        #src/auv2/AUv2Helper.swift

        utils.cpp
        EventLoop.cpp

        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp

        Logger.cpp
        AudioBufferList.cpp
        EventSequence.cpp

        PluginCatalog.cpp
        PluginBundlePool.cpp
        PluginFormat.cpp

        vst3/TravestyHelper.cpp
        vst3/HostClasses.cpp
        vst3/ClassModuleInfo.cpp
        vst3/PluginFormatVST3.cpp
        vst3/PluginInstanceVST3.cpp
        vst3/PluginInstanceVST3.Buses.cpp
        vst3/PluginInstanceVST3.Parameters.cpp
        vst3/PluginInstanceVST3.Events.cpp

        au/AUv2Helper.cpp
        ${remidy_OBJC_SOURCES}

        lv2/LV2Helper.cpp
        lv2/symap.cpp
        lv2/PluginFormatLV2.cpp
        lv2/PluginInstanceLV2.cpp
        lv2/PluginInstanceLV2.Buses.cpp
        lv2/PluginInstanceLV2.Events.cpp
        lv2/PluginInstanceLV2.Parameters.cpp

        AudioProcessContext.cpp
        PluginInstance.cpp
        UmpInputDispatcher.cpp
)

add_library(remidy::remidy ALIAS remidy)

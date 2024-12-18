cmake_minimum_required(VERSION 3.29)
project(remidy LANGUAGES C CXX OBJC OBJCXX Swift)

#add_subdirectory("../../external/libremidi" "libremidi")
include("../../external/LV2Kit.cmake")

CPMAddPackage(
        NAME           cpplocate
        GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
        GIT_REPOSITORY https://github.com/cginternals/cpplocate
        # required to mix CPP and Swift in the same project...
        PATCHES        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/cpplocate-remove-pthread.patch
)
CPMAddPackage(
        NAME           cmidi2
        GIT_TAG        0.7
        GIT_REPOSITORY https://github.com/atsushieno/cmidi2
)

# FIXME: it would be slightly nicer if we could migrate to CPM (failed to resolve farbot if we actually do so. Also see https://github.com/hogliux/farbot/pull/15 )
# <rtlog>
FetchContent_Declare(rtlog-cpp
        GIT_REPOSITORY https://github.com/atsushieno/rtlog-cpp
        GIT_TAG mpsc
)
FetchContent_MakeAvailable(rtlog-cpp)
# </rtlog>

# and we need farbot directly as well...
FetchContent_Declare(farbot
        GIT_REPOSITORY https://github.com/hogliux/farbot
        GIT_TAG 0416705394720c12f0d02e55c144e4f69bb06912
)
if (NOT farbot_POPULATED)
    FetchContent_Populate(farbot)
endif ()
add_library(farbot INTERFACE)
add_library(farbot::farbot ALIAS farbot)
target_include_directories(farbot INTERFACE
        $<BUILD_INTERFACE:${farbot_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set(VST3_PUBLIC_SDK_MODULE_INFO_DIR ../../external/vst3-public-sdk-moduleinfo)

add_library(remidy STATIC)

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
)

target_sources(remidy PRIVATE

        # swift sources generate C++ header files in use later, so compile them first.
        #src/auv2/AUv2Helper.swift

        utils.cpp
        EventLoop.cpp

        vst3/TravestyHelper.cpp
        vst3/HostClasses.cpp
        vst3/ClassModuleInfo.cpp

        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp

        lv2/LV2Helper.cpp
        lv2/symap.cpp

        Logger.cpp
        AudioBufferList.cpp
        EventSequence.cpp

        PluginCatalog.cpp
        PluginBundlePool.cpp
        PluginFormat.cpp
        PluginFormatVST3.cpp
        PluginFormatAU.mm
        PluginFormatLV2.cpp

        AudioProcessContext.cpp
        PluginInstance.cpp
        UmpInputDispatcher.cpp
)

add_library(remidy::remidy ALIAS remidy)

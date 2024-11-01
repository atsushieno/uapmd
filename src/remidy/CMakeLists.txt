cmake_minimum_required(VERSION 3.29)
project(remidy LANGUAGES C CXX OBJC OBJCXX Swift)

#add_subdirectory("../../external/libremidi" "libremidi")
add_subdirectory("../../external/json" "json")
include("../../external/LV2Kit.cmake")

include(FetchContent)
# <cpplocate>
FetchContent_Declare(cpplocate
        GIT_REPOSITORY https://github.com/cginternals/cpplocate
        GIT_TAG b796990d8df747758dbd6217b6315b7bdbb0c27e
        # required to mix CPP and Swift in the same project...
        PATCH_COMMAND git reset --hard && git apply ${CMAKE_CURRENT_SOURCE_DIR}/../../external/cpplocate-remove-pthread.patch
)
FetchContent_MakeAvailable(cpplocate)
# </cpplocate>

# <rtlog>
FetchContent_Declare(rtlog-cpp
        GIT_REPOSITORY https://github.com/cjappl/rtlog-cpp
        GIT_TAG 5774ba063ecc25057e0f616d61e50c079e939e82
)
FetchContent_MakeAvailable(rtlog-cpp)
# </rtlog>


set(CMAKE_CXX_STANDARD 20)

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
        ../../external/json/include
        ../../external/choc
        ${LV2KIT_INCLUDE_DIRS}
        #../../external/libremidi/include
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
        #EventLoop.posix.cpp
        #EventLoop.windows.cpp
        #EventLoop.darwin.cpp

        vst3/TravestyHelper.cpp
        vst3/HostClasses.cpp
        vst3/ClassModuleInfo.cpp

        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp

        lv2/LV2Helper.cpp
        lv2/symap.cpp

        Logger.cpp
        AudioBufferList.cpp
        MidiSequence.cpp

        PluginCatalog.cpp
        PluginBundlePool.cpp
        PluginFormat.cpp
        PluginFormatVST3.cpp
        PluginFormatAU.mm
        PluginFormatLV2.cpp

        AudioProcessContext.cpp
        PluginInstance.cpp
)

add_library(remidy::remidy ALIAS remidy)

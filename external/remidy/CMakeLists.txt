cmake_minimum_required(VERSION 3.29)
project(remidy LANGUAGES C CXX OBJC OBJCXX Swift)

set(CMAKE_CXX_STANDARD 23)

#add_subdirectory("../../external/libremidi" "libremidi")
#add_subdirectory("../../external/rtlog-cpp" "rtlog-cpp")
#add_subdirectory("../../external/cpplocate" "cpplocate" EXCLUDE_FROM_ALL)
add_subdirectory("../../external/json" "json")
include("../../external/LV2Kit.cmake")

add_library(remidy STATIC)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
target_compile_definitions(remidy PRIVATE
        V3_COM_COMPAT=1
)
endif ()

set(VST3_PUBLIC_SDK_MODULE_INFO_DIR ../../external/vst3-public-sdk-moduleinfo)

include(FetchContent)
# <cpplocate>
FetchContent_Declare(cpplocate
        GIT_REPOSITORY https://github.com/cginternals/cpplocate
        GIT_TAG b796990d8df747758dbd6217b6315b7bdbb0c27e
        # required to mix CPP and Swift in the same project...
        PATCH_COMMAND git reset --hard && git apply ${CMAKE_CURRENT_SOURCE_DIR}/../cpplocate-remove-pthread.patch
)
FetchContent_MakeAvailable(cpplocate)
target_link_libraries(remidy PRIVATE cpplocate::cpplocate)
# </cpplocate>
# <rtlog>
FetchContent_Declare(rtlog-cpp
        GIT_REPOSITORY https://github.com/cjappl/rtlog-cpp
        GIT_TAG 5774ba063ecc25057e0f616d61e50c079e939e82
)
FetchContent_MakeAvailable(rtlog-cpp)
target_link_libraries(remidy PRIVATE rtlog::rtlog)
# </rtlog>

target_include_directories(remidy PRIVATE
        ../../external/travesty
        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}
        ../../external/json/include
        ${LV2KIT_INCLUDE_DIRS}
        #../../external/libremidi/include
        include/remidy
        ${CMAKE_BINARY_DIR}/generated/include
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy_LIBS_PLAT
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
endif ()

target_link_libraries(remidy PRIVATE
        #libremidi::libremidi
        rtlog::rtlog
        cpplocate::cpplocate
        lv2kit
        ${remidy_LIBS_PLAT}
)

add_compile_options(-fsanitize=address)
add_link_options(-fsanitize=address)

target_compile_options(remidy PRIVATE

        $<$<COMPILE_LANGUAGE:ObjCXX>:-framework CoreAudio -framework CoreFoundation>
        $<$<COMPILE_LANGUAGE:Swift>:-cxx-interoperability-mode=default -emit-clang-header-path ${CMAKE_BINARY_DIR}/generated/include/swift-interop/remidy-Swift.h>
)

target_sources(remidy PRIVATE

        # swift sources generate C++ header files in use later, so compile them first.
        #src/auv2/AUv2Helper.swift

        src/utils.cpp
        src/EventLoop.posix.cpp
        src/EventLoop.windows.cpp
        src/EventLoop.darwin.cpp

        src/vst3/TravestyHelper.cpp
        src/vst3/HostClasses.cpp
        src/vst3/ClassModuleInfo.cpp

        ${VST3_PUBLIC_SDK_MODULE_INFO_DIR}/moduleinfoparser.cpp

        src/lv2/LV2Helper.cpp
        src/lv2/symap.cpp

        src/Logger.cpp
        src/AudioBufferList.cpp
        src/MidiSequence.cpp

        src/PluginCatalog.cpp
        src/AudioPluginLibraryPool.cpp
        src/AudioPluginFormat.cpp
        src/AudioPluginFormatVST3.cpp
        src/AudioPluginFormatAU.mm
        src/AudioPluginFormatLV2.cpp

        src/AudioProcessContext.cpp
        src/AudioPluginInstance.cpp
)

add_library(remidy::remidy ALIAS remidy)

add_subdirectory(examples/remidy-scan)

cmake_minimum_required(VERSION 3.29)
project(remidy LANGUAGES C CXX OBJCXX Swift)

set(CMAKE_CXX_STANDARD 23)

#add_subdirectory("../../external/libremidi" "libremidi")
#add_subdirectory("../../external/rtlog-cpp" "rtlog-cpp")
#add_subdirectory("../../external/cpplocate" "cpplocate" EXCLUDE_FROM_ALL)
add_subdirectory("../../external/json" "json")

add_library(remidy)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
target_compile_definitions(remidy PRIVATE
        V3_COM_COMPAT=1
)
endif ()

# <cpplocate>
include(FetchContent)
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
include(FetchContent)
FetchContent_Declare(rtlog-cpp
        GIT_REPOSITORY https://github.com/cjappl/rtlog-cpp
        GIT_TAG 5774ba063ecc25057e0f616d61e50c079e939e82
)
FetchContent_MakeAvailable(rtlog-cpp)
target_link_libraries(remidy PRIVATE rtlog::rtlog)
# </rtlog>

target_include_directories(remidy PRIVATE
        ../../external/travesty
        ../../external/cpplocate/source/cpplocate/include
        ../../external/json/include
        #../../external/libremidi/include
        include/remidy
        ${CMAKE_BINARY_DIR}/generated/include
)

target_link_libraries(remidy PRIVATE
        #libremidi::libremidi
        rtlog::rtlog
        cpplocate::cpplocate
        "-framework CoreFoundation"
        "-framework AVFoundation"
)

target_compile_options(remidy PRIVATE
        $<$<COMPILE_LANGUAGE:Swift>:-cxx-interoperability-mode=default -emit-clang-header-path ${CMAKE_BINARY_DIR}/generated/include/swift-interop/remidy-Swift.h>
)

target_sources(remidy PRIVATE

        # swift sources generate C++ header files in use later, so compile them first.
        src/auv2/AUv2Helper.swift

        src/vst3/VST3Helper.cpp
        src/vst3/HostApplication.cpp

        src/Logger.cpp
        src/AudioBufferList.cpp
        src/MidiSequence.cpp

        src/PluginCatalog.cpp
        src/AudioPluginLibraryPool.cpp
        src/AudioPluginFormat.cpp
        src/AudioPluginFormatVST3.cpp
        src/AudioPluginFormatAU.cpp

        src/AudioProcessContext.cpp
        src/AudioPluginInstance.cpp
)

add_library(remidy::remidy ALIAS remidy)

add_subdirectory(examples/remidy-scan)

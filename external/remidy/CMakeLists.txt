cmake_minimum_required(VERSION 3.29)
project(remidy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)

#add_subdirectory("../../external/libremidi" "libremidi")

add_library(remidy)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
target_compile_definitions(remidy PRIVATE
        V3_COM_COMPAT=1
)
endif ()

# <rtlog>
include(FetchContent)
FetchContent_Declare(rtlog-cpp
        GIT_REPOSITORY https://github.com/cjappl/rtlog-cpp
        GIT_TAG ab103a63e637b6523251ada9b917fd24ac2f978e
)
FetchContent_MakeAvailable(rtlog-cpp)
target_link_libraries(remidy PRIVATE rtlog::rtlog)
# </rtlog>

target_include_directories(remidy PRIVATE
        ../../external/travesty
        #../../external/libremidi/include
        include/remidy
)

target_link_libraries(remidy PRIVATE
        #libremidi::libremidi
        "-framework CoreFoundation"
)

target_sources(remidy PRIVATE
        src/Logger.cpp
        src/AudioBufferList.cpp
        src/MidiSequence.cpp

        src/HostApplication.cpp
        src/PluginCatalog.cpp
        src/AudioPluginLibraryPool.cpp
        src/AudioPluginFormat.cpp
        src/AudioPluginFormatVST3.cpp

        src/AudioProcessContext.cpp
        src/AudioPluginInstance.cpp
)

add_library(remidy::remidy ALIAS remidy)

add_subdirectory(examples/remidy-scan)

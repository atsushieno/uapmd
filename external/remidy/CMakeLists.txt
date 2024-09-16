cmake_minimum_required(VERSION 3.29)
project(remidy)

set(CMAKE_CXX_STANDARD 23)

#add_subdirectory("../../external/libremidi" "libremidi")

add_library(remidy)

target_include_directories(remidy PRIVATE
        #../../external/libremidi/include
        include/remidy
)

target_link_libraries(remidy PRIVATE
        #libremidi::libremidi
)

target_sources(remidy PRIVATE
        src/AudioPluginFormat.cpp
        src/AudioPluginFormatVST3.cpp
        src/AudioPluginInstance.cpp
        src/AudioPluginLibrary.cpp

        src/AudioBufferList.cpp
        src/MidiSequence.cpp
        src/AudioProcessContext.cpp
)

add_library(remidy::remidy ALIAS remidy)

add_subdirectory(examples/remidy-scan)

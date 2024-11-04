cmake_minimum_required(VERSION 3.18)

project(UniversalAudioPluginMidiDevice)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(src/remidy)

add_subdirectory(src/uapmd)

add_subdirectory(tools/remidy-tooling)

add_subdirectory(tools/remidy-scan)
add_subdirectory(tools/remidy-apply)

add_subdirectory(tools/uapmd-service)

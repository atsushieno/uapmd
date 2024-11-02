cmake_minimum_required(VERSION 3.18)

project(UniversalAudioPluginMidiDevice)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(src/remidy)

add_subdirectory(examples/remidy-tooling)

add_subdirectory(examples/remidy-scan)
add_subdirectory(examples/remidy-apply)

add_subdirectory(src/service)


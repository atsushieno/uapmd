cmake_minimum_required(VERSION 3.29)
project(UniversalAudioPluginMidiDevice)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(src/remidy)

add_subdirectory(examples/remidy-scan)

add_subdirectory(src/service)


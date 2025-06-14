cmake_minimum_required(VERSION 3.18)

project(UniversalAudioPluginMidiDevice)

set(CMAKE_CXX_STANDARD 23)

include(cmake/CPM.cmake)

CPMAddPackage(
        NAME           cmidi2
        GIT_REPOSITORY https://github.com/atsushieno/cmidi2
        GIT_TAG        0.7
        EXCLUDE_FROM_ALL
)

cmake_path(SET CPPLOCATE_PATCH_FILE NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/external/cpplocate-remove-pthread.patch")
FetchContent_Declare(cpplocate
        NAME           cpplocate
        GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
        GIT_REPOSITORY https://github.com/cginternals/cpplocate
        # required to mix CPP and Swift in the same project...
        PATCH_COMMAND        git apply "${CPPLOCATE_PATCH_FILE}"
        UPDATE_DISCONNECTED 1 # https://stackoverflow.com/questions/73718516/
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cpplocate)

#add_compile_options("-fsanitize=address")
#link_libraries("-fsanitize=address")
#add_link_options(-fPIC)

add_subdirectory(src/remidy)

add_subdirectory(src/remidy-tooling)

add_subdirectory(src/uapmd)

add_subdirectory(tools)

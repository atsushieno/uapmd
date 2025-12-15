cmake_minimum_required(VERSION 3.18)

if (APPLE)
    project(UniversalAudioPluginMidiDevice LANGUAGES C CXX OBJC OBJCXX)
else()
    project(UniversalAudioPluginMidiDevice LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 23)

# wow, that's stupid... https://stackoverflow.com/questions/5004858/why-is-stdmin-failing-when-windows-h-is-included
if (MSVC OR WIN32)
add_compile_definitions(NOMINMAX)
endif()

include(cmake/CPM.cmake)

FetchContent_Declare(choc
        NAME            choc
        GIT_TAG         68a63dd
        GIT_REPOSITORY  https://github.com/Tracktion/choc
)
FetchContent_MakeAvailable(choc)

FetchContent_Declare(cmidi2
        GIT_REPOSITORY https://github.com/atsushieno/cmidi2
        GIT_TAG        280bb44
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cmidi2)

cmake_path(SET CPPLOCATE_PATCH_FILE NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/external/cpplocate-remove-pthread.patch")
FetchContent_Declare(cpplocate
        NAME           cpplocate
        GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
        GIT_REPOSITORY https://github.com/cginternals/cpplocate
        # required to mix CPP and Swift in the same project...
        #PATCH_COMMAND        git apply "${CPPLOCATE_PATCH_FILE}"
        #UPDATE_DISCONNECTED 1 # https://stackoverflow.com/questions/73718516/
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cpplocate)

FetchContent_Declare(midicci
        NAME            midicci
        GIT_TAG         64662bf
        GIT_REPOSITORY  https://github.com/atsushieno/midicci
)
FetchContent_MakeAvailable(midicci)

CPMAddPackage(
        NAME portable-file-dialogs
        GIT_TAG c12ea8c
        GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs
        DOWNLOAD_ONLY YES
)

CPMAddPackage(
        NAME           concurrentqueue
        GIT_TAG        2f09da73d22a47dc8a89cdd4fc4c3bfae07f4284
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        EXCLUDE_FROM_ALL
)

#add_compile_options("-fsanitize=address")
#link_libraries("-fsanitize=address")
#add_link_options(-fPIC)

add_subdirectory(src/remidy)

add_subdirectory(src/remidy-gui)

add_subdirectory(src/remidy-tooling)

add_subdirectory(src/uapmd)

add_subdirectory(tools)

add_executable(remidy-plugin-host
        components/AudioPlayerController.cpp
        components/AudioPlayerController.hpp)

set(CMAKE_CXX_STANDARD 23)

# crashes
#set(saucer_backend "Qt6")
set(saucer_modules ON)

include(FetchContent)

FetchContent_Declare(
        saucer
        GIT_REPOSITORY "https://github.com/saucer/saucer"
        GIT_TAG very-experimental
)
FetchContent_MakeAvailable(saucer)

target_include_directories(remidy-plugin-host PRIVATE
        ../../external/choc
        ../../include
)
target_link_libraries(remidy-plugin-host PRIVATE
        saucer::saucer
        remidy::remidy
        remidy::remidy-tooling
        uapmd
)
target_sources(remidy-plugin-host PRIVATE
        AppModel.cpp
        components/AudioDeviceSetup.cpp
        components/AudioPluginSelectors.cpp
        components/AudioPluginInstanceControl.cpp
        main.cpp
)

# Prepare web content files
add_custom_target(build-web-content
        npm install
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/web
)
add_dependencies(remidy-plugin-host build-web-content)
file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}/web
        ${CMAKE_CURRENT_BINARY_DIR}/web
        COPY_ON_ERROR
        SYMBOLIC
)

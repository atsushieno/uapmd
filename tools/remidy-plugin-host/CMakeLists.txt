add_executable(remidy-plugin-host
        AppModel.cpp
        AppModel.hpp)

set(CMAKE_CXX_STANDARD 23)

# crashes
#set(saucer_backend "Qt6")
set(saucer_modules ON)

include(FetchContent)

FetchContent_Declare(
        saucer
        GIT_REPOSITORY "https://github.com/saucer/saucer"
        GIT_TAG v4.0.0
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
)
target_sources(remidy-plugin-host PRIVATE
        AudioDeviceSetup.cpp
        AudioPluginSelectors.cpp
        main.cpp
)
file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}/web
        ${CMAKE_CURRENT_BINARY_DIR}/web
        COPY_ON_ERROR
        SYMBOLIC
)

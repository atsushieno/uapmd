
CPMAddPackage(
        NAME           cxxopts
        GIT_TAG        dbf4c6a66816f6c3872b46cc6af119ad227e04e1
        GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
        EXCLUDE_FROM_ALL
)

if (NOT ANDROID AND NOT EMSCRIPTEN)
    add_subdirectory(remidy-scan)
    add_subdirectory(uapmd-apply)
endif ()
add_subdirectory(uapmd-app)

# Install the CLI tools as executables
if (NOT ANDROID AND NOT EMSCRIPTEN)
install(TARGETS remidy-scan
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT uapmd)
install(TARGETS uapmd-app
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT uapmd)
if(APPLE AND TARGET uapmd-app-bundle)
    install(TARGETS uapmd-app-bundle
            BUNDLE DESTINATION .
            COMPONENT uapmd_app)
endif()
endif ()


CPMAddPackage(
        NAME           cxxopts
        GIT_TAG        dbf4c6a66816f6c3872b46cc6af119ad227e04e1
        GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
        EXCLUDE_FROM_ALL
)

if (NOT ANDROID AND NOT EMSCRIPTEN AND NOT IOS)
    add_subdirectory(uapmd-apply)
    add_subdirectory(uapmd-scan)
endif ()
add_subdirectory(uapmd-app)
if(EMSCRIPTEN)
    add_subdirectory(wclap-host)
    add_dependencies(uapmd-app uapmd-wclap-host)
endif()

# Install targets
if (NOT ANDROID AND NOT EMSCRIPTEN AND NOT IOS)
install(TARGETS uapmd-scan uapmd-apply
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT uapmd)
endif ()
if (NOT ANDROID AND NOT EMSCRIPTEN)
install(TARGETS uapmd-app
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        BUNDLE DESTINATION .
        COMPONENT uapmd)
if(APPLE AND TARGET uapmd-app-bundle)
    install(TARGETS uapmd-app-bundle
            BUNDLE DESTINATION .
            COMPONENT uapmd_app)
endif()
endif ()


CPMAddPackage(
        NAME           cxxopts
        GIT_TAG        dbf4c6a66816f6c3872b46cc6af119ad227e04e1
        GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
        EXCLUDE_FROM_ALL
)

add_subdirectory(remidy-scan)
add_subdirectory(remidy-apply)
add_subdirectory(uapmd-app)

# Install the CLI tools as executables
install(TARGETS remidy-scan
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS uapmd-app
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})


CPMAddPackage(
        NAME           cxxopts
        GIT_TAG        dbf4c6a66816f6c3872b46cc6af119ad227e04e1
        GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
        EXCLUDE_FROM_ALL
)

add_subdirectory(remidy-scan)
add_subdirectory(remidy-apply)
#add_subdirectory(remidy-plugin-host)
#add_subdirectory(uapmd-service)
add_subdirectory(uapmd-app)

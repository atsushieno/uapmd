
add_library(uapmd-project STATIC)

target_include_directories(uapmd-project PRIVATE
        ${choc_SOURCE_DIR}
        ../../include
)

target_link_libraries(uapmd-project
        PUBLIC
        uapmd
)

target_sources(uapmd-project PRIVATE
        audio/ChocAudioFileFactory.cpp
        project/UapmdProjectFileReader.cpp
        project/UapmdProjectFileWriter.cpp
        project/UapmdProjectFile.cpp
)

# Add tests subdirectory
option(UAPMD_BUILD_TESTS "Build uapmd-project tests" ON)
if(UAPMD_BUILD_TESTS)
    add_subdirectory(tests)
endif()

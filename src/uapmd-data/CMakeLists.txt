
add_library(uapmd-data STATIC)

target_include_directories(uapmd-data PRIVATE
        ${choc_SOURCE_DIR}
        ../../include
)

target_link_libraries(uapmd-data
        PUBLIC
        uapmd
)

target_sources(uapmd-data PRIVATE
        audio/ChocAudioFileFactory.cpp
        project/UapmdProjectFileReader.cpp
        project/UapmdProjectFileWriter.cpp
        project/UapmdProjectFile.cpp
        timeline/AudioFileSourceNode.cpp
        timeline/DeviceInputSourceNode.cpp
        timeline/ClipManager.cpp
        timeline/TimelineTrack.cpp
)

# Add tests subdirectory
option(UAPMD_BUILD_TESTS "Build uapmd-data tests" ON)
if(UAPMD_BUILD_TESTS)
    add_subdirectory(tests)
endif()


add_library(uapmd-data STATIC)

target_include_directories(uapmd-data PRIVATE
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
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
        project/SmfConverter.cpp
        project/Smf2ClipReader.cpp
        project/Smf2ClipReaderWriter.cpp
        timeline/AudioFileSourceNode.cpp
        timeline/DeviceInputSourceNode.cpp
        timeline/MidiClipSourceNode.cpp
        timeline/ClipManager.cpp
        timeline/TimelineTrack.cpp
)

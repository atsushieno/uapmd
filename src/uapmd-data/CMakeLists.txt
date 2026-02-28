
CPMAddPackage(
    NAME demucscpp
    GIT_REPOSITORY https://github.com/sevagh/demucs.cpp
    GIT_TAG f1206e9adeea103aef4a636b9e62297cf1f8e34e
    DOWNLOAD_ONLY YES
)

if(NOT TARGET demucs-cpp)
    file(GLOB DEMUCSPP_SOURCES
        ${demucscpp_SOURCE_DIR}/src/*.cpp
    )
    add_library(demucs-cpp STATIC ${DEMUCSPP_SOURCES})
    target_include_directories(demucs-cpp PUBLIC
        ${demucscpp_SOURCE_DIR}/src
        ${demucscpp_SOURCE_DIR}/vendor/eigen
    )
    target_compile_features(demucs-cpp PUBLIC cxx_std_17)
endif()

add_library(uapmd-data STATIC)

target_include_directories(uapmd-data PRIVATE
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${miniaudio_SOURCE_DIR}
        ../../include
)

target_link_libraries(uapmd-data
        PUBLIC
        uapmd
        PRIVATE
        demucs-cpp
)

set(_UAPMD_AUDIO_FACTORY_SRC audio/ChocAudioFileFactory.cpp)
if(EMSCRIPTEN)
    set(_UAPMD_AUDIO_FACTORY_SRC audio/MiniAudioFileFactory.cpp)
endif()

target_sources(uapmd-data PRIVATE
        ${_UAPMD_AUDIO_FACTORY_SRC}
        project/UapmdProjectFileReader.cpp
        project/UapmdProjectFileWriter.cpp
        project/UapmdProjectFile.cpp
        project/SmfConverter.cpp
        project/Smf2ClipReader.cpp
        project/Smf2ClipReaderWriter.cpp
        project/DemucsStemSeparator.cpp
        project/TrackImporter.cpp
        timeline/AudioFileSourceNode.cpp
        timeline/DeviceInputSourceNode.cpp
        timeline/MidiClipSourceNode.cpp
        timeline/ClipManager.cpp
        timeline/TimelineTrack.cpp
)

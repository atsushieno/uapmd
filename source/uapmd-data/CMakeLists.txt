CPMAddPackage(
    NAME signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch
    GIT_TAG 57b93f4e9206a089a45387eaa39bdc9f310d3308
    EXCLUDE_FROM_ALL YES
)

add_library(uapmd-data STATIC)

target_include_directories(uapmd-data PRIVATE
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${miniaudio_SOURCE_DIR}
        ../remidy/include
        ../uapmd-graph/include
        ../uapmd-plugin-hosting/include
        include
)

target_link_libraries(uapmd-data
        PUBLIC
        uapmd-graph
        PRIVATE
        signalsmith-stretch
        zlib-ng
)

set(_UAPMD_AUDIO_FACTORY_SRC src/audio/ChocAudioFileFactory.cpp)
if(EMSCRIPTEN)
    set(_UAPMD_AUDIO_FACTORY_SRC src/audio/MiniAudioFileFactory.cpp)
endif()

target_sources(uapmd-data PRIVATE
        ${_UAPMD_AUDIO_FACTORY_SRC}
        src/command/ProjectCommandManager.cpp
        src/command/ProjectHistory.cpp
        src/command/ProjectUndo.cpp
        src/project/AudioGraphProvider.cpp
        src/project/UapmdProjectFileReader.cpp
        src/project/UapmdProjectFileWriter.cpp
        src/project/UapmdProjectFile.cpp
        src/project/UapmdAudioPluginFullDAGraphData.cpp
        src/project/UapmdPluginGraphBuilder.cpp
        src/project/UapmdAudioPluginFullDAGraphBuilder.cpp
        src/project/SmfConverter.cpp
        src/project/Smf2ClipReader.cpp
        src/project/Smf2ClipReaderWriter.cpp
        src/project/StemSeparator.cpp
        src/project/TrackImporter.cpp
        src/project/ProjectArchive.cpp
        src/timeline/AudioFileSourceNode.cpp
        src/timeline/DeviceInputSourceNode.cpp
        src/timeline/MidiClipSourceNode.cpp
        src/timeline/ClipManager.cpp
        src/timeline/TimeReferenceResolver.cpp
        src/timeline/TimelineTrack.cpp
)

uapmd_optimize_heavy_audio_feature_sources_in_debug(uapmd-data
        src/timeline/AudioFileSourceNode.cpp
)

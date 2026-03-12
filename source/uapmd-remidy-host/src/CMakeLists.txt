add_library(uapmd-remidy-host STATIC
        RemidyAudioPluginHost.cpp
        midi/UapmdNodeUmpMapper.cpp
)

target_include_directories(uapmd-remidy-host
        PUBLIC
        ../include
        PRIVATE
        midi
        ../../remidy/include
        ../../remidy-tooling/include
        ../include
        ../../uapmd/include
        ../../uapmd/src
        ${miniaudio_SOURCE_DIR}
        ${choc_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-remidy-host
        PUBLIC
        uapmd
        PRIVATE
        remidy::remidy
        remidy::remidy-tooling
)

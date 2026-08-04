add_library(uapmd-ara STATIC
        src/AraSupport.cpp
        src/AraSession.cpp
        src/AraHostDocumentController.cpp
        src/AraFormatBinding.cpp
)
add_library(uapmd::ara ALIAS uapmd-ara)

if(NOT DEFINED clap_SOURCE_DIR AND DEFINED CPM_PACKAGE_clap_SOURCE_DIR)
    set(clap_SOURCE_DIR "${CPM_PACKAGE_clap_SOURCE_DIR}")
endif()

if(NOT DEFINED vst3sdk_SOURCE_DIR AND DEFINED CPM_PACKAGE_vst3sdk_SOURCE_DIR)
    set(vst3sdk_SOURCE_DIR "${CPM_PACKAGE_vst3sdk_SOURCE_DIR}")
endif()

if(APPLE)
    target_sources(uapmd-ara PRIVATE src/AraAudioUnit.cpp)
endif()

if(NOT ANDROID AND NOT EMSCRIPTEN)
    target_sources(uapmd-ara PRIVATE
            src/AraVst3.cpp
            src/AraClap.cpp
    )
endif()

target_compile_features(uapmd-ara PUBLIC cxx_std_23)

target_include_directories(uapmd-ara
        PUBLIC
        include
        ${ARA_SDK_SOURCE_DIR}
        ${ARA_SDK_SOURCE_DIR}/ARA_API
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-engine/include
        ${midicci_SOURCE_DIR}/include
        ${vst3sdk_SOURCE_DIR}
        ${clap_SOURCE_DIR}/include
)

target_link_libraries(uapmd-ara
        PUBLIC
        uapmd::ara-api
        uapmd-engine
        uapmd-data
        uapmd-midi-service
        remidy
)

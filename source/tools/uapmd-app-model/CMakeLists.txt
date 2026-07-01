add_library(uapmd-app-model STATIC
        src/AppModel.cpp
        src/UapmdJSRuntime.cpp
        src/Configuration/AudioDeviceConfiguration.cpp
        src/Configuration/VirtualMidiDeviceConfiguration.cpp
)

set(_uapmd_app_model_public_include_dirs
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${libremidi_SOURCE_DIR}/include
        ../../remidy/include
        ../../remidy-tooling/include
        ../../uapmd/include
        ../../uapmd-graph/include
        ../../uapmd-file/include
        ../../uapmd-data/include
        ../../uapmd-engine/include
)

target_include_directories(uapmd-app-model
        PUBLIC
            ${_uapmd_app_model_public_include_dirs}
)

if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(uapmd-app-model PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/liblocate/include
    )
endif()

set(uapmd_app_model_LIBS_PLAT)
if(WIN32 AND NOT UAPMD_BUILDING_WASM)
    list(APPEND uapmd_app_model_LIBS_PLAT ws2_32)
endif()
if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd_app_model_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework Cocoa"
    )
elseif(IOS)
    set(uapmd_app_model_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework UIKit"
            "-framework Metal"
            "-framework MetalKit"
    )
endif()

set(_uapmd_app_model_link_libraries
        remidy
        remidy-tooling
        uapmd
        uapmd-file
        midicci_static
        uapmd-engine
        libremidi::libremidi
        zlib-ng
)

if(TARGET uapmd::ara)
    list(APPEND _uapmd_app_model_link_libraries uapmd::ara)
    target_compile_definitions(uapmd-app-model PUBLIC UAPMD_HAS_ARA=1)
endif()
if(UAPMD_ENABLE_CPPTRACE)
    list(APPEND _uapmd_app_model_link_libraries cpptrace::cpptrace)
endif()
if(NOT UAPMD_BUILDING_WASM)
    list(APPEND _uapmd_app_model_link_libraries ${uapmd_app_model_LIBS_PLAT})
endif()
if(UAPMD_ENABLE_CPPLOCATE)
    list(APPEND _uapmd_app_model_link_libraries ${cpplocate_LIBRARIES})
endif()

target_link_libraries(uapmd-app-model PUBLIC ${_uapmd_app_model_link_libraries})
target_compile_features(uapmd-app-model PUBLIC cxx_std_23)

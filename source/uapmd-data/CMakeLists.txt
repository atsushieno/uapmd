
CPMAddPackage(
    NAME demucscpp
    GIT_REPOSITORY https://github.com/sevagh/demucs.cpp
    GIT_TAG f1206e9adeea103aef4a636b9e62297cf1f8e34e
    DOWNLOAD_ONLY YES
)

set(_UAPMD_DEMUCS_PRIVATE_LIBS)
set(_UAPMD_DEMUCS_PRIVATE_DEFINES)
set(_UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS)

if(UAPMD_DEMUCS_USE_OPENBLAS)
    if(APPLE)
        find_library(_UAPMD_ACCELERATE_FRAMEWORK Accelerate)
        if(_UAPMD_ACCELERATE_FRAMEWORK)
            list(APPEND _UAPMD_DEMUCS_PRIVATE_LIBS ${_UAPMD_ACCELERATE_FRAMEWORK})
            list(APPEND _UAPMD_DEMUCS_PRIVATE_DEFINES EIGEN_USE_BLAS)
        else()
            message(WARNING "Accelerate framework not found on this Apple toolchain; Demucs BLAS acceleration disabled.")
        endif()
    else()
        set(_UAPMD_OPENBLAS_DYNAMIC_ARCH ON)
        set(_UAPMD_OPENBLAS_TARGET "")
        if(CMAKE_CROSSCOMPILING)
            set(_UAPMD_OPENBLAS_DYNAMIC_ARCH OFF)
            string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _uapmd_proc)
            if(_uapmd_proc MATCHES "aarch64" OR _uapmd_proc MATCHES "arm64")
                set(_UAPMD_OPENBLAS_TARGET ARMV8)
            elseif(_uapmd_proc MATCHES "armv7")
                set(_UAPMD_OPENBLAS_TARGET ARMV7)
            elseif(_uapmd_proc MATCHES "x86_64")
                set(_UAPMD_OPENBLAS_TARGET HASWELL)
            elseif(_uapmd_proc MATCHES "i[3-6]86" OR _uapmd_proc MATCHES "x86")
                set(_UAPMD_OPENBLAS_TARGET PENTIUM4)
            else()
                set(_UAPMD_OPENBLAS_TARGET GENERIC)
            endif()
        endif()
        set(_UAPMD_OPENBLAS_OPTIONS
                "BUILD_SHARED_LIBS OFF"
                "BUILD_TESTING OFF"
                "BUILD_WITHOUT_LAPACK ON"
                "DYNAMIC_ARCH ${_UAPMD_OPENBLAS_DYNAMIC_ARCH}"
                "NOFORTRAN ON"
                "CMAKE_POLICY_VERSION 3.5"
                "CMAKE_POLICY_VERSION_MINIMUM 3.5"
        )
        if(_UAPMD_OPENBLAS_TARGET)
            list(APPEND _UAPMD_OPENBLAS_OPTIONS "TARGET ${_UAPMD_OPENBLAS_TARGET}")
        endif()
        CPMAddPackage(
                NAME openblas
                GIT_REPOSITORY https://github.com/OpenMathLib/OpenBLAS
                GIT_TAG v0.3.26
                EXCLUDE_FROM_ALL YES
                OPTIONS ${_UAPMD_OPENBLAS_OPTIONS}
        )
        if(NOT TARGET openblas)
            message(FATAL_ERROR "Requested UAPMD_DEMUCS_USE_OPENBLAS=ON, but the OpenBLAS target was not created.")
        endif()
        list(APPEND _UAPMD_DEMUCS_PRIVATE_LIBS openblas)
        list(APPEND _UAPMD_DEMUCS_PRIVATE_DEFINES EIGEN_USE_BLAS)
        list(APPEND _UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS
                ${openblas_SOURCE_DIR}
                ${openblas_SOURCE_DIR}/include
                ${openblas_SOURCE_DIR}/interface
                ${openblas_BINARY_DIR}
                ${openblas_BINARY_DIR}/generated
        )
    endif()
endif()

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
    if(_UAPMD_DEMUCS_PRIVATE_LIBS)
        target_link_libraries(demucs-cpp PRIVATE ${_UAPMD_DEMUCS_PRIVATE_LIBS})
    endif()
    if(_UAPMD_DEMUCS_PRIVATE_DEFINES)
        target_compile_definitions(demucs-cpp PRIVATE ${_UAPMD_DEMUCS_PRIVATE_DEFINES})
    endif()
    if(_UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS)
        target_include_directories(demucs-cpp PRIVATE ${_UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS})
    endif()
endif()

add_library(uapmd-data STATIC)

target_include_directories(uapmd-data PRIVATE
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${miniaudio_SOURCE_DIR}
        ../remidy/include
        ../remidy-tooling/include
        ../uapmd/include
        include
)

target_link_libraries(uapmd-data
        PUBLIC
        uapmd
        PRIVATE
        demucs-cpp
)

set(_UAPMD_AUDIO_FACTORY_SRC src/audio/ChocAudioFileFactory.cpp)
if(EMSCRIPTEN)
    set(_UAPMD_AUDIO_FACTORY_SRC src/audio/MiniAudioFileFactory.cpp)
endif()

target_sources(uapmd-data PRIVATE
        ${_UAPMD_AUDIO_FACTORY_SRC}
        src/plugin-api/UapmdNodeUmpMapper.cpp
        src/plugin-api/RemidyAudioPluginHost.cpp
        src/project/UapmdProjectFileReader.cpp
        src/project/UapmdProjectFileWriter.cpp
        src/project/UapmdProjectFile.cpp
        src/project/SmfConverter.cpp
        src/project/Smf2ClipReader.cpp
        src/project/Smf2ClipReaderWriter.cpp
        src/project/DemucsStemSeparator.cpp
        src/project/TrackImporter.cpp
        src/project/ProjectArchive.cpp
        src/timeline/AudioFileSourceNode.cpp
        src/timeline/DeviceInputSourceNode.cpp
        src/timeline/MidiClipSourceNode.cpp
        src/timeline/ClipManager.cpp
        src/timeline/TimelineTrack.cpp
)

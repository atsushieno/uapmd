# Shared by the modules that build DSP-heavy third-party code (demucs.cpp,
# signalsmith-stretch): those paths are unusably slow when compiled with full
# Debug settings, so they are optimized even in Debug builds unless the
# builder opts back in with UAPMD_ENABLE_HEAVY_AUDIO_FEATURE_DEBUG.


function(uapmd_optimize_heavy_audio_feature_target_in_debug target_name)
    if(UAPMD_ENABLE_HEAVY_AUDIO_FEATURE_DEBUG)
        return()
    endif()

    target_compile_definitions(${target_name} PRIVATE
            "$<$<CONFIG:Debug>:NDEBUG>"
    )
    target_compile_options(${target_name} PRIVATE
            "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:/O2>"
            "$<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O3>"
    )
endfunction()

function(uapmd_optimize_heavy_audio_feature_sources_in_debug target_name)
    if(UAPMD_ENABLE_HEAVY_AUDIO_FEATURE_DEBUG)
        return()
    endif()

    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_DEFINITIONS
            "$<$<CONFIG:Debug>:NDEBUG>"
    )
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS
            "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:/O2>"
            "$<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O3>"
    )
endfunction()

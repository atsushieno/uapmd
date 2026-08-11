add_library(uapmd-addin-core STATIC
        src/Addin.cpp
)

set_target_properties(uapmd-addin-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(uapmd-addin-core PUBLIC
        include
)

target_compile_features(uapmd-addin-core PUBLIC cxx_std_23)

add_library(uapmd::uapmd-addin-core ALIAS uapmd-addin-core)

function(add_uapmd_addin_library target)
    target_link_libraries(${target} PRIVATE uapmd::uapmd-addin-core)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_definitions(${target} PRIVATE UAPMD_BUILDING_ADDIN=1)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

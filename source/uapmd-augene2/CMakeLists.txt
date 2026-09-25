# Keep the compatibility changes in the build tree, never in a user's checkout
# or the shared CPM cache. They can be dropped once upstream exposes track labels.
CPMAddPackage(
    NAME augene2
    GIT_REPOSITORY https://github.com/atsushieno/augene2
    GIT_TAG 20b3a694fc2df013f8efe58b79363ca816d36ae9
    DOWNLOAD_ONLY YES
)
set(_augene_source "${CMAKE_CURRENT_BINARY_DIR}/compiler")
file(MAKE_DIRECTORY "${_augene_source}")
file(COPY "${augene2_SOURCE_DIR}/include" "${augene2_SOURCE_DIR}/src"
    "${augene2_SOURCE_DIR}/grammar" "${augene2_SOURCE_DIR}/cmake"
    "${augene2_SOURCE_DIR}/resources" "${augene2_SOURCE_DIR}/CMakeLists.txt"
    DESTINATION "${_augene_source}")

file(READ "${augene2_SOURCE_DIR}/include/mugene2/mugene2.hpp" _header)
string(REPLACE "uint32_t track_id{};" "uint32_t track_id{};\n    double source_track_number{};" _header "${_header}")
file(WRITE "${_augene_source}/include/mugene2/mugene2.hpp" "${_header}")
file(READ "${augene2_SOURCE_DIR}/src/mugene2/detail/compiler_internal.cpp" _compiler)
string(REPLACE ".track_id = track_id++," ".track_id = track_id++,\n            .source_track_number = track.number," _compiler "${_compiler}")
file(WRITE "${_augene_source}/src/mugene2/detail/compiler_internal.cpp" "${_compiler}")

# Embed every built-in definition, including files referenced by #include.
file(GLOB _resources "${augene2_SOURCE_DIR}/resources/*.mml")
set(_embedded "#pragma once\n#include <map>\n#include <string>\nnamespace uapmd_augene2 {\ninline const std::map<std::string, std::string> builtin_sources{\n")
foreach(_resource IN LISTS _resources)
    get_filename_component(_name "${_resource}" NAME)
    file(READ "${_resource}" _text)
    string(APPEND _embedded "{\"${_name}\", R\"UAPMD_MML(${_text})UAPMD_MML\"},\n")
endforeach()
string(APPEND _embedded "};\n}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/BuiltinSources.hpp" "${_embedded}")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${_augene_source}" "${CMAKE_CURRENT_BINARY_DIR}/compiler-build" EXCLUDE_FROM_ALL)

add_library(uapmd-augene2 STATIC
    src/Augene2Compiler.cpp
    src/Augene2Integration.cpp
)
target_compile_features(uapmd-augene2 PRIVATE cxx_std_23)
target_compile_definitions(uapmd-augene2 PUBLIC UAPMD_HAS_AUGENE2=1)
target_include_directories(uapmd-augene2 PUBLIC include)
target_include_directories(uapmd-augene2 PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${choc_SOURCE_DIR}")
target_link_libraries(uapmd-augene2 PRIVATE mugene2 uapmd-app-model imgui)

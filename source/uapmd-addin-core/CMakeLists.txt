add_library(uapmd-addin-core STATIC
        src/AddinManager.cpp
)

set_target_properties(uapmd-addin-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(uapmd-addin-core PUBLIC
        include
)

target_compile_features(uapmd-addin-core PUBLIC cxx_std_23)

if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(uapmd-addin-core PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/liblocate/include
    )
    target_link_libraries(uapmd-addin-core PRIVATE ${cpplocate_LIBRARIES})
    target_compile_definitions(uapmd-addin-core PRIVATE UAPMD_HAS_CPPLOCATE=1)
endif()
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(uapmd-addin-core PRIVATE ${CMAKE_DL_LIBS})
endif()

add_library(uapmd::uapmd-addin-core ALIAS uapmd-addin-core)

if(APPLE)
    set(UAPMD_ADDIN_DIRECTORY
            "$ENV{HOME}/Library/Preferences/uapmd/addins"
            CACHE PATH "Directory where the development build installs UAPMD addin libraries")
endif()

function(add_uapmd_addin_library target)
    target_link_libraries(${target} PRIVATE uapmd::uapmd-addin-core)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_definitions(${target} PRIVATE UAPMD_BUILDING_ADDIN=1)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(DEFINED UAPMD_ADDIN_DIRECTORY AND NOT "${UAPMD_ADDIN_DIRECTORY}" STREQUAL "")
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${UAPMD_ADDIN_DIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:${target}>"
                        "${UAPMD_ADDIN_DIRECTORY}/$<TARGET_FILE_NAME:${target}>"
                COMMENT "Installing ${target} into the UAPMD addin directory")
    endif()
endfunction()

function(deploy_uapmd_addin_library target directory)
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${target}>"
                    "${directory}/$<TARGET_FILE_NAME:${target}>"
            COMMENT "Installing ${target} into the UAPMD addin directory")
endfunction()

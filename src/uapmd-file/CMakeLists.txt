add_library(uapmd-file STATIC)

target_include_directories(uapmd-file PUBLIC
        ../../include
)

if(ANDROID)
    target_sources(uapmd-file PRIVATE
            DocumentProviderAndroid.cpp
    )
    target_include_directories(uapmd-file PRIVATE
            ${jmi_SOURCE_DIR}
    )
    target_sources(uapmd-file PRIVATE ${jmi_SOURCE_DIR}/jmi.cpp)
    target_link_libraries(uapmd-file PUBLIC
            android
            log
    )
elseif(EMSCRIPTEN)
    target_sources(uapmd-file PRIVATE
            DocumentProviderEmscripten.cpp
    )
    target_link_options(uapmd-file PUBLIC
            -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$allocateUTF8
            -sEXPORTED_RUNTIME_METHODS=ccall,allocateUTF8
    )
else()
    target_sources(uapmd-file PRIVATE
            DocumentProviderDesktop.cpp
    )
    target_include_directories(uapmd-file PRIVATE
            ${portable-file-dialogs_SOURCE_DIR}
    )
endif()

add_library(uapmd::uapmd-file ALIAS uapmd-file)

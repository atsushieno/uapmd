if(NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "PATCH_FILE must be provided")
endif()

get_filename_component(_patch_file "${PATCH_FILE}" REALPATH)
if(DEFINED PATCH_WORK_DIR)
    get_filename_component(_patch_work_dir "${PATCH_WORK_DIR}" REALPATH)
else()
    set(_patch_work_dir "${CMAKE_BINARY_DIR}")
endif()

if(NOT EXISTS "${_patch_file}")
    message(FATAL_ERROR "Patch file not found: ${_patch_file}")
endif()

execute_process(
    COMMAND git apply --reverse --check --ignore-whitespace "${_patch_file}"
    WORKING_DIRECTORY "${_patch_work_dir}"
    RESULT_VARIABLE _reverse_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(_reverse_result EQUAL 0)
    message(STATUS "Patch already applied: ${_patch_file}")
    return()
endif()

execute_process(
    COMMAND git apply --check --ignore-whitespace "${_patch_file}"
    WORKING_DIRECTORY "${_patch_work_dir}"
    RESULT_VARIABLE _check_result)

if(NOT _check_result EQUAL 0)
    message(FATAL_ERROR "Patch verification failed for ${_patch_file}")
endif()

execute_process(
    COMMAND git apply --ignore-whitespace "${_patch_file}"
    WORKING_DIRECTORY "${_patch_work_dir}"
    RESULT_VARIABLE _apply_result)

if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply patch ${_patch_file}")
endif()

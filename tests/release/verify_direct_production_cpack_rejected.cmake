cmake_minimum_required(VERSION 4.3)

foreach(_input IN ITEMS
        APPELLATE_BUILD_DIR
        APPELLATE_CPACK_EXECUTABLE
        APPELLATE_PACKAGE_FILE_NAME)
    if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
        message(FATAL_ERROR "Direct-production rejection input is missing: ${_input}")
    endif()
endforeach()
if(NOT APPELLATE_PACKAGE_FILE_NAME MATCHES "-production-cpack-disabled$")
    message(FATAL_ERROR "Direct-production rejection requires the disabled package basename")
endif()

get_filename_component(_build_root "${APPELLATE_BUILD_DIR}" ABSOLUTE)
if(_build_root STREQUAL "" OR _build_root STREQUAL "/" OR
   NOT _build_root STREQUAL APPELLATE_BUILD_DIR OR
   IS_SYMLINK "${_build_root}" OR NOT IS_DIRECTORY "${_build_root}")
    message(FATAL_ERROR "Direct-production rejection requires an absolute ordinary build root")
endif()
set(_cpack_config "${_build_root}/CPackConfig.cmake")
if(IS_SYMLINK "${_cpack_config}" OR NOT EXISTS "${_cpack_config}" OR
   IS_DIRECTORY "${_cpack_config}")
    message(FATAL_ERROR "Direct-production rejection lacks an ordinary CPack configuration")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_root "${_build_root}/direct-production-rejection-${_suffix}")
set(_sentinel "${_root}/.appellate-direct-production-rejection-root")
get_filename_component(_root_parent "${_root}" DIRECTORY)
get_filename_component(_root_name "${_root}" NAME)
if(EXISTS "${_root}" OR IS_SYMLINK "${_root}" OR
   NOT _root_parent STREQUAL _build_root OR
   NOT _root_name MATCHES "^direct-production-rejection-[0-9a-f]+$")
    message(FATAL_ERROR "Direct-production rejection scratch root is unsafe")
endif()
file(MAKE_DIRECTORY "${_root}")
file(WRITE "${_sentinel}" "appellate-direct-production-rejection-root\n")
file(REAL_PATH "${_build_root}" _build_root_real)
file(REAL_PATH "${_root}" _root_real)
get_filename_component(_root_real_parent "${_root_real}" DIRECTORY)
if(_build_root_real STREQUAL "" OR _build_root_real STREQUAL "/" OR
   NOT _root_real_parent STREQUAL _build_root_real OR
   NOT _root_real STREQUAL "${_build_root_real}/${_root_name}")
    message(FATAL_ERROR "Direct-production rejection scratch root escaped its build parent")
endif()

set(_artifacts "${_root}/artifacts")
execute_process(
    COMMAND
        "${APPELLATE_CPACK_EXECUTABLE}"
        --config "${_cpack_config}"
        -G TZST
        -B "${_artifacts}"
    WORKING_DIRECTORY "${_build_root}"
    RESULT_VARIABLE _cpack_result
    OUTPUT_VARIABLE _cpack_output
    ERROR_VARIABLE _cpack_error
    TIMEOUT 120
)
set(_cpack_text "${_cpack_output}\n${_cpack_error}")
string(REGEX REPLACE "[ \t\r\n]+" " " _normalized_cpack_text "${_cpack_text}")
string(
    FIND "${_normalized_cpack_text}"
    "Direct production CPack is disabled; use the signed_release_candidate target"
    _expected_rejection_index
)
file(
    GLOB_RECURSE _scratch_entries
    LIST_DIRECTORIES TRUE
    RELATIVE "${_root}"
    "${_root}/*"
)
set(_emitted_artifacts)
foreach(_entry IN LISTS _scratch_entries)
    if(_entry MATCHES "[.]tar[.]zst$" OR _entry MATCHES "[.]sha256$")
        list(APPEND _emitted_artifacts "${_entry}")
    endif()
endforeach()

set(_failure "")
if("${_cpack_result}" STREQUAL "0")
    string(APPEND _failure "Direct production CPack unexpectedly succeeded. ")
endif()
if(_expected_rejection_index LESS 0)
    string(APPEND _failure "Direct production CPack did not report its exact rejection. ")
endif()
if(_emitted_artifacts)
    string(APPEND _failure "Direct production CPack emitted an archive or checksum. ")
endif()

get_filename_component(_final_root_parent "${_root}" DIRECTORY)
get_filename_component(_final_root_name "${_root}" NAME)
if(NOT _final_root_parent STREQUAL _build_root OR
   NOT _final_root_name STREQUAL _root_name OR
   IS_SYMLINK "${_root}" OR NOT IS_DIRECTORY "${_root}" OR
   IS_SYMLINK "${_sentinel}" OR NOT EXISTS "${_sentinel}" OR
   IS_DIRECTORY "${_sentinel}")
    message(FATAL_ERROR "Refusing to clean an unowned direct-production rejection root")
endif()
file(REAL_PATH "${_root}" _final_root_real)
file(READ "${_sentinel}" _sentinel_text)
if(NOT _final_root_real STREQUAL _root_real OR
   NOT _sentinel_text STREQUAL "appellate-direct-production-rejection-root\n")
    message(FATAL_ERROR "Refusing to clean a replaced direct-production rejection root")
endif()
file(REMOVE_RECURSE "${_root}")
if(EXISTS "${_root}" OR IS_SYMLINK "${_root}")
    message(FATAL_ERROR "Direct-production rejection scratch root could not be removed")
endif()
if(NOT _failure STREQUAL "")
    message(FATAL_ERROR "${_failure}\n${_cpack_text}")
endif()

message(STATUS "Direct production CPack rejection left no archive or checksum")

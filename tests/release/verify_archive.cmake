if(NOT DEFINED APPELLATE_BUILD_DIR OR NOT DEFINED APPELLATE_CPACK_EXECUTABLE OR
   NOT DEFINED APPELLATE_PACKAGE_FILE_NAME OR NOT DEFINED APPELLATE_INSTALL_LIBDIR OR
   NOT DEFINED APPELLATE_SOURCE_DIR OR NOT DEFINED APPELLATE_VERIFY_INSTALL_SCRIPT)
    message(FATAL_ERROR "Archive verification inputs are incomplete")
endif()

if(POLICY CMP0009)
    cmake_policy(SET CMP0009 NEW)
endif()

find_program(_appellate_stat NAMES stat REQUIRED)

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_root "${APPELLATE_BUILD_DIR}/archive-smoke-${_suffix}")
set(_artifacts "${_root}/artifacts")
set(_extracted "${_root}/extracted")
if(EXISTS "${_root}")
    message(FATAL_ERROR "Random archive verifier root unexpectedly already exists")
endif()
file(MAKE_DIRECTORY "${_artifacts}" "${_extracted}")
file(WRITE "${_root}/.appellate-archive-verifier-root" "appellate-archive-verifier-root\n")

execute_process(
    COMMAND
        "${APPELLATE_CPACK_EXECUTABLE}"
        --config "${APPELLATE_BUILD_DIR}/CPackConfig.cmake"
        -G TZST
        -B "${_artifacts}"
    WORKING_DIRECTORY "${APPELLATE_BUILD_DIR}"
    RESULT_VARIABLE _cpack_result
    OUTPUT_VARIABLE _cpack_output
    ERROR_VARIABLE _cpack_error
)
if(NOT _cpack_result EQUAL 0)
    message(FATAL_ERROR "CPack failed:\n${_cpack_output}\n${_cpack_error}")
endif()

set(_archive "${_artifacts}/${APPELLATE_PACKAGE_FILE_NAME}.tar.zst")
set(_checksum "${_archive}.sha256")
if(NOT EXISTS "${_archive}" OR NOT EXISTS "${_checksum}")
    message(FATAL_ERROR "CPack did not emit the expected archive and SHA-256 file")
endif()
file(SHA256 "${_archive}" _actual_sha)
file(READ "${_checksum}" _checksum_text)
string(REGEX MATCH "^([0-9a-f]+)[ \t]+" _checksum_match "${_checksum_text}")
if(NOT _checksum_match OR NOT CMAKE_MATCH_1 STREQUAL _actual_sha)
    message(FATAL_ERROR "The emitted archive checksum does not verify")
endif()

file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extracted}")
file(GLOB _top_level LIST_DIRECTORIES TRUE RELATIVE "${_extracted}" "${_extracted}/*")
if(NOT _top_level STREQUAL APPELLATE_PACKAGE_FILE_NAME OR
   NOT IS_DIRECTORY "${_extracted}/${APPELLATE_PACKAGE_FILE_NAME}")
    message(FATAL_ERROR "The emitted archive must contain one exact top-level directory")
endif()
file(WRITE "${_extracted}/.appellate-install-verifier-root" "appellate-install-verifier-root\n")

set(
    _expected_archive_documentation_entries
    "README.md"
    "docs/APPELLATE_EVENT_CATALOG.md"
    "docs/ARCHITECTURE.md"
    "docs/INDEPENDENT_REVIEW.md"
    "docs/PARITY_INVENTORY.md"
    "docs/PRODUCT.md"
    "docs/REALISM_MATRIX.md"
    "docs/RELEASE.md"
    "docs/adr/0001-native-offline-mvp.md"
    "docs/adr/0002-declarative-pack-trust-boundary.md"
    "docs/adr/0003-bench-profile-boundary.md"
    "docs/content/M4_CASE_MATRIX.md"
    "docs/spec/PACKS.md"
)
list(SORT _expected_archive_documentation_entries)
set(
    _archive_documentation_root
    "${_extracted}/${APPELLATE_PACKAGE_FILE_NAME}/share/doc/appellate-workbench"
)
file(
    GLOB_RECURSE _archive_documentation_entries
    LIST_DIRECTORIES FALSE
    RELATIVE "${_archive_documentation_root}"
    "${_archive_documentation_root}/*"
)
list(SORT _archive_documentation_entries)
if(NOT _archive_documentation_entries STREQUAL _expected_archive_documentation_entries)
    message(FATAL_ERROR "Archive documentation does not match the exact nonflattened allowlist")
endif()
foreach(_relative_path IN LISTS _expected_archive_documentation_entries)
    set(_archive_file "${_archive_documentation_root}/${_relative_path}")
    set(_source_file "${APPELLATE_SOURCE_DIR}/${_relative_path}")
    if(IS_SYMLINK "${_archive_file}" OR NOT EXISTS "${_archive_file}" OR
       IS_DIRECTORY "${_archive_file}")
        message(FATAL_ERROR "Archive documentation member is linked, missing, or nonregular: ${_relative_path}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env "LC_ALL=C"
            "${_appellate_stat}" --format=%F -- "${_archive_file}"
        RESULT_VARIABLE _documentation_stat_result
        OUTPUT_VARIABLE _documentation_file_type
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _documentation_stat_result EQUAL 0 OR
       NOT _documentation_file_type STREQUAL "regular file")
        message(FATAL_ERROR "Archive documentation member is not regular: ${_relative_path}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_archive_file}" "${_source_file}"
        RESULT_VARIABLE _documentation_compare_result
    )
    if(NOT _documentation_compare_result EQUAL 0)
        message(FATAL_ERROR "Archive documentation bytes differ from source: ${_relative_path}")
    endif()
endforeach()

file(
    GLOB_RECURSE _archive_pack_entries
    LIST_DIRECTORIES FALSE
    RELATIVE "${_extracted}/${APPELLATE_PACKAGE_FILE_NAME}"
    "${_extracted}/${APPELLATE_PACKAGE_FILE_NAME}/*.awpack"
)
list(SORT _archive_pack_entries)
set(
    _expected_archive_pack_entries
    "share/appellate-workbench/packs/foundation-us-ca4-2026.03.23.awpack"
    "share/appellate-workbench/packs/foundation-us-ca4-fictional-bench-1.0.0.awpack"
    "share/appellate-workbench/packs/foundation-us-federal-2025.12.01.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-arm-agency-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-benton-retaliation-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-blueember-jmol-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-cinderlake-writ-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-ellison-immunity-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-norvale-injunction-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-opengrid-foia-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-m4-serrano-waiver-1.2.0.awpack"
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.1.0.awpack"
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.2.0.awpack"
)
list(SORT _expected_archive_pack_entries)
if(NOT _archive_pack_entries STREQUAL _expected_archive_pack_entries)
    message(
        FATAL_ERROR
        "Archive .awpack allowlist differs; generated starter archives must never ship: "
        "${_archive_pack_entries}"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DAPPELLATE_BUNDLE_PREFIX=${_extracted}/${APPELLATE_PACKAGE_FILE_NAME}"
        "-DAPPELLATE_VERIFIER_ROOT=${_extracted}"
        "-DAPPELLATE_INSTALL_LIBDIR=${APPELLATE_INSTALL_LIBDIR}"
        "-DAPPELLATE_SOURCE_DIR=${APPELLATE_SOURCE_DIR}"
        -P "${APPELLATE_VERIFY_INSTALL_SCRIPT}"
    RESULT_VARIABLE _verify_result
    OUTPUT_VARIABLE _verify_output
    ERROR_VARIABLE _verify_error
)
if(NOT _verify_result EQUAL 0)
    message(FATAL_ERROR "Extracted archive verification failed:\n${_verify_output}\n${_verify_error}")
endif()

file(READ "${_root}/.appellate-archive-verifier-root" _archive_root_sentinel)
if(NOT _archive_root_sentinel STREQUAL "appellate-archive-verifier-root\n")
    message(FATAL_ERROR "Refusing to remove an archive verifier root without its exact sentinel")
endif()
file(REMOVE_RECURSE "${_root}")
if(EXISTS "${_root}")
    message(FATAL_ERROR "Temporary archive verification tree could not be removed")
endif()

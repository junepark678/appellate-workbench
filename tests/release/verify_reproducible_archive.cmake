cmake_minimum_required(VERSION 4.3)

set(
    _appellate_required_inputs
    APPELLATE_BUILD_ROOT
    APPELLATE_CMAKE_GENERATOR
    APPELLATE_CMAKE_MAKE_PROGRAM
    APPELLATE_CPACK_EXECUTABLE
    APPELLATE_CXX_COMPILER
    APPELLATE_GIT_EXECUTABLE
    APPELLATE_GLIBC_FLOOR
    APPELLATE_INSTALL_LIBDIR
    APPELLATE_PACKAGE_FILE_NAME
    APPELLATE_QT6_DIR
    APPELLATE_RELEASE_SOURCE_EPOCH
    APPELLATE_RELEASE_SOURCE_REVISION
    APPELLATE_SOURCE_DIR
)
foreach(_input IN LISTS _appellate_required_inputs)
    if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
        message(FATAL_ERROR "Archive reproducibility input is missing: ${_input}")
    endif()
endforeach()
if(CMAKE_VERSION VERSION_LESS 4.3)
    message(FATAL_ERROR "Archive reproducibility requires CMake 4.3 or newer")
endif()
string(LENGTH "${APPELLATE_RELEASE_SOURCE_REVISION}" _revision_length)
if(NOT _revision_length EQUAL 40 OR
   NOT APPELLATE_RELEASE_SOURCE_REVISION MATCHES "^[0-9a-f]+$" OR
   NOT APPELLATE_RELEASE_SOURCE_EPOCH MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Archive reproducibility revision or epoch is malformed")
endif()
if(NOT APPELLATE_PACKAGE_FILE_NAME MATCHES "-development-unverified$")
    message(FATAL_ERROR "Archive reproducibility may emit only development-unverified packages")
endif()

execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" status --porcelain=v1 --untracked-files=normal
    WORKING_DIRECTORY "${APPELLATE_SOURCE_DIR}"
    RESULT_VARIABLE _source_status_result
    OUTPUT_VARIABLE _source_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _source_status_error
)
if(NOT _source_status_result EQUAL 0 OR NOT _source_status STREQUAL "")
    message(
        FATAL_ERROR
        "Archive reproducibility requires the real source worktree to be clean:\n"
        "${_source_status}\n${_source_status_error}"
    )
endif()
execute_process(
    COMMAND "${APPELLATE_GIT_EXECUTABLE}" rev-parse --verify HEAD^{commit}
    WORKING_DIRECTORY "${APPELLATE_SOURCE_DIR}"
    RESULT_VARIABLE _head_result
    OUTPUT_VARIABLE _head_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _head_error
)
if(NOT _head_result EQUAL 0 OR
   NOT _head_revision STREQUAL APPELLATE_RELEASE_SOURCE_REVISION)
    message(
        FATAL_ERROR
        "Archive reproducibility revision differs from clean HEAD: ${_head_error}"
    )
endif()
execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" show -s --format=%ct
        "${APPELLATE_RELEASE_SOURCE_REVISION}^{commit}"
    WORKING_DIRECTORY "${APPELLATE_SOURCE_DIR}"
    RESULT_VARIABLE _epoch_result
    OUTPUT_VARIABLE _head_epoch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _epoch_error
)
if(NOT _epoch_result EQUAL 0 OR
   NOT _head_epoch STREQUAL APPELLATE_RELEASE_SOURCE_EPOCH)
    message(FATAL_ERROR "Archive reproducibility epoch differs from clean HEAD: ${_epoch_error}")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
get_filename_component(_build_root_absolute "${APPELLATE_BUILD_ROOT}" ABSOLUTE)
if(_build_root_absolute STREQUAL "" OR _build_root_absolute STREQUAL "/" OR
   NOT _build_root_absolute STREQUAL APPELLATE_BUILD_ROOT OR
   IS_SYMLINK "${_build_root_absolute}" OR NOT IS_DIRECTORY "${_build_root_absolute}")
    message(FATAL_ERROR "Archive reproducibility requires an absolute ordinary build root")
endif()
set(_root "${_build_root_absolute}/archive-reproducibility-${_suffix}")
set(_snapshot_source "${_root}/exact-source-snapshot")
set(_first_build "${_root}/first-clean-build")
set(_second_build "${_root}/second-clean-build-with-distinct-path-length")
set(_first_artifacts "${_root}/first-artifacts")
set(_second_artifacts "${_root}/second-artifacts")
get_filename_component(_root_parent "${_root}" DIRECTORY)
get_filename_component(_root_name "${_root}" NAME)
if(EXISTS "${_root}" OR EXISTS "${_first_build}" OR EXISTS "${_second_build}" OR
   IS_SYMLINK "${_root}" OR NOT _root_parent STREQUAL _build_root_absolute OR
   NOT _root_name MATCHES "^archive-reproducibility-[0-9a-f]+$" OR
   _first_build STREQUAL _second_build)
    message(FATAL_ERROR "Archive reproducibility build roots must be distinct and initially absent")
endif()
file(MAKE_DIRECTORY "${_root}")
file(WRITE "${_root}/.appellate-reproducibility-root" "appellate-reproducibility-root\n")
file(REAL_PATH "${_build_root_absolute}" _build_root_real)
file(REAL_PATH "${_root}" _root_real)
get_filename_component(_root_real_parent "${_root_real}" DIRECTORY)
if(_build_root_real STREQUAL "" OR _build_root_real STREQUAL "/" OR
   NOT _root_real_parent STREQUAL _build_root_real OR
   NOT _root_real STREQUAL "${_build_root_real}/${_root_name}")
    message(FATAL_ERROR "Archive reproducibility root escapes its exact build-root parent")
endif()

function(_appellate_cleanup_reproducibility_root)
    set(_root_sentinel_path "${_root}/.appellate-reproducibility-root")
    get_filename_component(_final_root_parent "${_root}" DIRECTORY)
    get_filename_component(_final_root_name "${_root}" NAME)
    if(NOT _final_root_parent STREQUAL _build_root_absolute OR
       NOT _final_root_name STREQUAL _root_name OR IS_SYMLINK "${_root}" OR
       NOT IS_DIRECTORY "${_root}" OR IS_SYMLINK "${_root_sentinel_path}" OR
       NOT EXISTS "${_root_sentinel_path}" OR IS_DIRECTORY "${_root_sentinel_path}")
        message(FATAL_ERROR "Refusing to remove a reproducibility root without its exact sentinel")
    endif()
    file(REAL_PATH "${_root}" _final_root_real)
    get_filename_component(_final_root_real_parent "${_final_root_real}" DIRECTORY)
    if(NOT _final_root_real STREQUAL _root_real OR
       NOT _final_root_real_parent STREQUAL _build_root_real)
        message(FATAL_ERROR "Refusing to remove a replaced reproducibility root")
    endif()
    file(READ "${_root_sentinel_path}" _root_sentinel)
    if(NOT _root_sentinel STREQUAL "appellate-reproducibility-root\n")
        message(FATAL_ERROR "Refusing to remove a reproducibility root with a changed sentinel")
    endif()
    file(REMOVE_RECURSE "${_root}")
    if(EXISTS "${_root}" OR IS_SYMLINK "${_root}")
        message(FATAL_ERROR "Archive reproducibility root could not be removed")
    endif()
endfunction()

function(_appellate_fail_with_cleanup)
    string(CONCAT _failure_message ${ARGV})
    _appellate_cleanup_reproducibility_root()
    message(FATAL_ERROR "${_failure_message}")
endfunction()

execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" clone --local --no-checkout --no-tags --
        "${APPELLATE_SOURCE_DIR}" "${_snapshot_source}"
    RESULT_VARIABLE _snapshot_clone_result
    OUTPUT_VARIABLE _snapshot_clone_output
    ERROR_VARIABLE _snapshot_clone_error
    TIMEOUT 600
)
if(NOT _snapshot_clone_result EQUAL 0)
    _appellate_fail_with_cleanup(
        "Exact source snapshot clone failed (result: ${_snapshot_clone_result}):\n"
        "${_snapshot_clone_output}\n${_snapshot_clone_error}"
    )
endif()
execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" -C "${_snapshot_source}"
        checkout --detach --force "${APPELLATE_RELEASE_SOURCE_REVISION}"
    RESULT_VARIABLE _snapshot_checkout_result
    OUTPUT_VARIABLE _snapshot_checkout_output
    ERROR_VARIABLE _snapshot_checkout_error
    TIMEOUT 600
)
if(NOT _snapshot_checkout_result EQUAL 0)
    _appellate_fail_with_cleanup(
        "Exact source snapshot checkout failed (result: ${_snapshot_checkout_result}):\n"
        "${_snapshot_checkout_output}\n${_snapshot_checkout_error}"
    )
endif()
execute_process(
    COMMAND "${APPELLATE_GIT_EXECUTABLE}" -C "${_snapshot_source}" remote remove origin
    RESULT_VARIABLE _snapshot_remote_result
    OUTPUT_VARIABLE _snapshot_remote_output
    ERROR_VARIABLE _snapshot_remote_error
)
if(NOT _snapshot_remote_result EQUAL 0)
    _appellate_fail_with_cleanup(
        "Exact source snapshot could not be made network-independent:\n"
        "${_snapshot_remote_output}\n${_snapshot_remote_error}"
    )
endif()
execute_process(
    COMMAND "${APPELLATE_GIT_EXECUTABLE}" -C "${_snapshot_source}" rev-parse --verify HEAD^{commit}
    RESULT_VARIABLE _snapshot_head_result
    OUTPUT_VARIABLE _snapshot_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _snapshot_head_error
)
execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" -C "${_snapshot_source}"
        status --porcelain=v1 --untracked-files=normal
    RESULT_VARIABLE _snapshot_status_result
    OUTPUT_VARIABLE _snapshot_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _snapshot_status_error
)
if(NOT _snapshot_head_result EQUAL 0 OR
   NOT _snapshot_head STREQUAL APPELLATE_RELEASE_SOURCE_REVISION OR
   NOT _snapshot_status_result EQUAL 0 OR NOT _snapshot_status STREQUAL "")
    _appellate_fail_with_cleanup(
        "Exact source snapshot does not match the configured clean commit:\n"
        "${_snapshot_head_error}${_snapshot_status}\n${_snapshot_status_error}"
    )
endif()

set(
    _reproducible_environment
    "TZ=UTC"
    "LC_ALL=C.UTF-8"
    "SOURCE_DATE_EPOCH=${APPELLATE_RELEASE_SOURCE_EPOCH}"
)
set(
    _configure_arguments
    -G "${APPELLATE_CMAKE_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_CXX_COMPILER=${APPELLATE_CXX_COMPILER}"
    "-DCMAKE_INSTALL_LIBDIR=${APPELLATE_INSTALL_LIBDIR}"
    "-DCMAKE_MAKE_PROGRAM=${APPELLATE_CMAKE_MAKE_PROGRAM}"
    "-DQt6_DIR=${APPELLATE_QT6_DIR}"
    "-DBUILD_TESTING=OFF"
    "-DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=ON"
    "-DAPPELLATE_ENABLE_ASAN=OFF"
    "-DAPPELLATE_ENABLE_LINUX_PACKAGING=ON"
    "-DAPPELLATE_ENABLE_SYNC=ON"
    "-DAPPELLATE_ENABLE_UBSAN=OFF"
    "-DAPPELLATE_GLIBC_FLOOR=${APPELLATE_GLIBC_FLOOR}"
    "-DAPPELLATE_RELEASE_SIGNING_FINGERPRINT="
    "-DAPPELLATE_RELEASE_SOURCE_EPOCH=${APPELLATE_RELEASE_SOURCE_EPOCH}"
    "-DAPPELLATE_RELEASE_SOURCE_REVISION=${APPELLATE_RELEASE_SOURCE_REVISION}"
)

function(_appellate_configure_reproducible_build build_dir label)
    if(EXISTS "${build_dir}")
        _appellate_fail_with_cleanup("${label} was not initially absent: ${build_dir}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env ${_reproducible_environment}
            "${CMAKE_COMMAND}"
            -S "${_snapshot_source}"
            -B "${build_dir}"
            ${_configure_arguments}
        RESULT_VARIABLE _configure_result
        OUTPUT_VARIABLE _configure_output
        ERROR_VARIABLE _configure_error
        TIMEOUT 600
    )
    if(NOT _configure_result EQUAL 0)
        _appellate_fail_with_cleanup(
            "${label} configure failed (result: ${_configure_result}):\n"
            "${_configure_output}\n${_configure_error}"
        )
    endif()
endfunction()

function(_appellate_build_reproducible_bundle build_dir label)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env ${_reproducible_environment}
            "${CMAKE_COMMAND}" --build "${build_dir}" --parallel 2
        RESULT_VARIABLE _build_result
        OUTPUT_VARIABLE _build_output
        ERROR_VARIABLE _build_error
        TIMEOUT 2400
    )
    if(NOT _build_result EQUAL 0)
        _appellate_fail_with_cleanup(
            "${label} build failed (result: ${_build_result}):\n"
            "${_build_output}\n${_build_error}"
        )
    endif()
endfunction()

function(_appellate_package_reproducible_bundle build_dir artifact_dir label)
    if(EXISTS "${artifact_dir}")
        _appellate_fail_with_cleanup("${label} artifact directory was not initially absent")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env ${_reproducible_environment}
            "${APPELLATE_CPACK_EXECUTABLE}"
            --config "${build_dir}/CPackConfig.cmake"
            -G TZST
            -B "${artifact_dir}"
        WORKING_DIRECTORY "${build_dir}"
        RESULT_VARIABLE _package_result
        OUTPUT_VARIABLE _package_output
        ERROR_VARIABLE _package_error
        TIMEOUT 1800
    )
    if(NOT _package_result EQUAL 0)
        _appellate_fail_with_cleanup(
            "${label} packaging failed (result: ${_package_result}):\n"
            "${_package_output}\n${_package_error}"
        )
    endif()
endfunction()

function(_appellate_verify_checksum archive checksum label output_sha)
    if(NOT EXISTS "${archive}" OR IS_DIRECTORY "${archive}" OR IS_SYMLINK "${archive}" OR
       NOT EXISTS "${checksum}" OR IS_DIRECTORY "${checksum}" OR IS_SYMLINK "${checksum}")
        _appellate_fail_with_cleanup("${label} archive or checksum is missing or nonregular")
    endif()
    file(SHA256 "${archive}" _archive_sha)
    file(READ "${checksum}" _checksum_text)
    string(REGEX MATCH "^([0-9a-f]+)[ \t]+" _checksum_match "${_checksum_text}")
    set(_reported_sha "${CMAKE_MATCH_1}")
    string(LENGTH "${_reported_sha}" _reported_sha_length)
    if(NOT _checksum_match OR NOT _reported_sha_length EQUAL 64 OR
       NOT _reported_sha STREQUAL _archive_sha)
        _appellate_fail_with_cleanup("${label} checksum does not identify its archive")
    endif()
    set("${output_sha}" "${_archive_sha}" PARENT_SCOPE)
endfunction()

_appellate_configure_reproducible_build("${_first_build}" "First clean build")
_appellate_configure_reproducible_build("${_second_build}" "Second clean build")
_appellate_build_reproducible_bundle("${_first_build}" "First clean build")
_appellate_build_reproducible_bundle("${_second_build}" "Second clean build")
_appellate_package_reproducible_bundle(
    "${_first_build}" "${_first_artifacts}" "First clean build"
)
_appellate_package_reproducible_bundle(
    "${_second_build}" "${_second_artifacts}" "Second clean build"
)

set(_archive_name "${APPELLATE_PACKAGE_FILE_NAME}.tar.zst")
set(_first_archive "${_first_artifacts}/${_archive_name}")
set(_second_archive "${_second_artifacts}/${_archive_name}")
set(_first_checksum "${_first_archive}.sha256")
set(_second_checksum "${_second_archive}.sha256")
_appellate_verify_checksum(
    "${_first_archive}" "${_first_checksum}" "First clean build" _first_sha
)
_appellate_verify_checksum(
    "${_second_archive}" "${_second_checksum}" "Second clean build" _second_sha
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_first_archive}" "${_second_archive}"
    RESULT_VARIABLE _archive_compare_result
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_first_checksum}" "${_second_checksum}"
    RESULT_VARIABLE _checksum_compare_result
)
if(NOT _archive_compare_result EQUAL 0 OR NOT _checksum_compare_result EQUAL 0 OR
   NOT _first_sha STREQUAL _second_sha)
    _appellate_fail_with_cleanup(
        "Same-image clean builds did not emit byte-identical archives and checksums: "
        "${_first_sha} != ${_second_sha}"
    )
endif()

execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" status --porcelain=v1 --untracked-files=normal
    WORKING_DIRECTORY "${APPELLATE_SOURCE_DIR}"
    RESULT_VARIABLE _final_status_result
    OUTPUT_VARIABLE _final_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _final_status_error
)
execute_process(
    COMMAND "${APPELLATE_GIT_EXECUTABLE}" rev-parse --verify HEAD^{commit}
    WORKING_DIRECTORY "${APPELLATE_SOURCE_DIR}"
    RESULT_VARIABLE _final_head_result
    OUTPUT_VARIABLE _final_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _final_head_error
)
execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" -C "${_snapshot_source}"
        status --porcelain=v1 --untracked-files=normal
    RESULT_VARIABLE _final_snapshot_status_result
    OUTPUT_VARIABLE _final_snapshot_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _final_snapshot_status_error
)
execute_process(
    COMMAND
        "${APPELLATE_GIT_EXECUTABLE}" -C "${_snapshot_source}"
        rev-parse --verify HEAD^{commit}
    RESULT_VARIABLE _final_snapshot_head_result
    OUTPUT_VARIABLE _final_snapshot_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _final_snapshot_head_error
)
if(NOT _final_status_result EQUAL 0 OR NOT _final_status STREQUAL "" OR
   NOT _final_head_result EQUAL 0 OR
   NOT _final_head STREQUAL APPELLATE_RELEASE_SOURCE_REVISION OR
   NOT _final_snapshot_status_result EQUAL 0 OR NOT _final_snapshot_status STREQUAL "" OR
   NOT _final_snapshot_head_result EQUAL 0 OR
   NOT _final_snapshot_head STREQUAL APPELLATE_RELEASE_SOURCE_REVISION)
    _appellate_fail_with_cleanup(
        "Clean-build source provenance changed during the gate:\n"
        "real status: ${_final_status}\n"
        "snapshot status: ${_final_snapshot_status}\n"
        "${_final_status_error}${_final_head_error}"
        "${_final_snapshot_status_error}${_final_snapshot_head_error}"
    )
endif()

_appellate_cleanup_reproducibility_root()
message(STATUS "Same-image clean-build archive SHA-256: ${_first_sha}")

cmake_minimum_required(VERSION 4.3)

if(NOT DEFINED APPELLATE_ARCHIVE_BUILD_MODE OR
   APPELLATE_ARCHIVE_BUILD_MODE STREQUAL "")
    set(APPELLATE_ARCHIVE_BUILD_MODE "DEVELOPMENT_REPRODUCIBILITY")
endif()
if(NOT APPELLATE_ARCHIVE_BUILD_MODE STREQUAL "DEVELOPMENT_REPRODUCIBILITY" AND
   NOT APPELLATE_ARCHIVE_BUILD_MODE STREQUAL "SIGNED_CANDIDATE")
    message(FATAL_ERROR "Unknown exact archive build mode: ${APPELLATE_ARCHIVE_BUILD_MODE}")
endif()
set(_signed_candidate FALSE)
if(APPELLATE_ARCHIVE_BUILD_MODE STREQUAL "SIGNED_CANDIDATE")
    set(_signed_candidate TRUE)
endif()

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
if(_signed_candidate)
    set(
        _appellate_signed_required_inputs
        APPELLATE_PROJECT_VERSION
        APPELLATE_RELEASE_CANDIDATE_ROOT
        APPELLATE_RELEASE_SIGNING_FINGERPRINT
    )
    foreach(_input IN LISTS _appellate_signed_required_inputs)
        if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
            message(FATAL_ERROR "Signed archive candidate input is missing: ${_input}")
        endif()
    endforeach()
endif()
if(CMAKE_VERSION VERSION_LESS 4.3)
    message(FATAL_ERROR "Archive reproducibility requires CMake 4.3 or newer")
endif()
string(LENGTH "${APPELLATE_RELEASE_SOURCE_REVISION}" _revision_length)
if(NOT _revision_length EQUAL 40 OR
   NOT APPELLATE_RELEASE_SOURCE_REVISION MATCHES "^[0-9a-f]+$" OR
   NOT APPELLATE_RELEASE_SOURCE_EPOCH MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Archive reproducibility revision or epoch is malformed")
endif()
if(_signed_candidate)
    string(LENGTH "${APPELLATE_RELEASE_SIGNING_FINGERPRINT}" _fingerprint_length)
    if(NOT APPELLATE_PROJECT_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$" OR
       NOT _fingerprint_length EQUAL 40 OR
       NOT APPELLATE_RELEASE_SIGNING_FINGERPRINT MATCHES "^[0-9A-Fa-f]+$" OR
       NOT APPELLATE_PACKAGE_FILE_NAME STREQUAL
           "appellate-workbench-${APPELLATE_PROJECT_VERSION}-linux-x86_64")
        message(FATAL_ERROR "Signed archive candidate identity inputs are malformed")
    endif()
    find_program(_appellate_stat_executable NAMES stat REQUIRED)
    string(TOUPPER "${APPELLATE_RELEASE_SIGNING_FINGERPRINT}" _expected_fingerprint)
    set(_expected_tag "v${APPELLATE_PROJECT_VERSION}")
else()
    if(NOT APPELLATE_PACKAGE_FILE_NAME MATCHES "-development-unverified$")
        message(FATAL_ERROR "Archive reproducibility may emit only development-unverified packages")
    endif()
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
file(REAL_PATH "${_build_root_absolute}" _build_root_real)
if(_build_root_real STREQUAL "" OR _build_root_real STREQUAL "/")
    message(FATAL_ERROR "Archive reproducibility build root cannot resolve broadly")
endif()

if(_signed_candidate)
    get_filename_component(
        _candidate_root_absolute "${APPELLATE_RELEASE_CANDIDATE_ROOT}" ABSOLUTE
    )
    get_filename_component(_candidate_root_parent "${_candidate_root_absolute}" DIRECTORY)
    get_filename_component(_candidate_root_name "${_candidate_root_absolute}" NAME)
    if(_candidate_root_absolute STREQUAL "" OR _candidate_root_absolute STREQUAL "/" OR
       NOT _candidate_root_absolute STREQUAL APPELLATE_RELEASE_CANDIDATE_ROOT OR
       NOT _candidate_root_parent STREQUAL _build_root_absolute OR
       NOT _candidate_root_name STREQUAL "release-candidates" OR
       IS_SYMLINK "${_candidate_root_absolute}" OR
       (EXISTS "${_candidate_root_absolute}" AND
        NOT IS_DIRECTORY "${_candidate_root_absolute}"))
        message(
            FATAL_ERROR
            "Signed release candidates require the ordinary release-candidates build child"
        )
    endif()
    set(_candidate_root_created FALSE)
    if(NOT EXISTS "${_candidate_root_absolute}")
        file(MAKE_DIRECTORY "${_candidate_root_absolute}")
        set(_candidate_root_created TRUE)
    endif()
    set(
        _candidate_parent_sentinel
        "${_candidate_root_absolute}/.appellate-release-candidate-root"
    )
    if(_candidate_root_created AND
       NOT EXISTS "${_candidate_parent_sentinel}" AND
       NOT IS_SYMLINK "${_candidate_parent_sentinel}")
        file(
            WRITE "${_candidate_parent_sentinel}"
            "appellate-release-candidate-root\n"
        )
    endif()
    if(IS_SYMLINK "${_candidate_parent_sentinel}" OR
       NOT EXISTS "${_candidate_parent_sentinel}" OR
       IS_DIRECTORY "${_candidate_parent_sentinel}")
        message(FATAL_ERROR "Signed release candidate-root sentinel is missing or replaced")
    endif()
    file(READ "${_candidate_parent_sentinel}" _candidate_parent_sentinel_text)
    execute_process(
        COMMAND
            "${_appellate_stat_executable}" --format=%d|%i|%F|%u|%a --
            "${_candidate_root_absolute}"
        RESULT_VARIABLE _candidate_root_stat_result
        OUTPUT_VARIABLE _candidate_root_identity
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _candidate_root_stat_error
    )
    execute_process(
        COMMAND
            "${_appellate_stat_executable}" --format=%d|%i|%F|%h --
            "${_candidate_parent_sentinel}"
        RESULT_VARIABLE _candidate_sentinel_stat_result
        OUTPUT_VARIABLE _candidate_parent_sentinel_identity
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _candidate_sentinel_stat_error
    )
    if(NOT _candidate_parent_sentinel_text STREQUAL
           "appellate-release-candidate-root\n" OR
       NOT _candidate_root_stat_result EQUAL 0 OR
       NOT _candidate_root_identity MATCHES "^[0-9]+[|][0-9]+[|]directory[|]" OR
       NOT _candidate_sentinel_stat_result EQUAL 0 OR
       NOT _candidate_parent_sentinel_identity MATCHES
           "^[0-9]+[|][0-9]+[|]regular file[|]1$")
        message(
            FATAL_ERROR
            "Signed release candidate root has unsafe identity: "
            "${_candidate_root_stat_error}${_candidate_sentinel_stat_error}"
        )
    endif()
    file(REAL_PATH "${_candidate_root_absolute}" _candidate_root_real)
    get_filename_component(_candidate_root_real_parent "${_candidate_root_real}" DIRECTORY)
    if(NOT _candidate_root_real_parent STREQUAL _build_root_real OR
       NOT _candidate_root_real STREQUAL "${_build_root_real}/release-candidates")
        message(FATAL_ERROR "Signed release candidate root escapes the exact build root")
    endif()
    string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef _first_driver_token)
    string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef _second_driver_token)
    if(_first_driver_token STREQUAL _second_driver_token)
        message(FATAL_ERROR "Signed release build tokens must be distinct")
    endif()
    set(_scratch_parent_absolute "${_candidate_root_absolute}")
    set(_scratch_parent_real "${_candidate_root_real}")
    set(_root_name ".signed-release-staging-${_suffix}")
    set(_root_name_pattern "^[.]signed-release-staging-[0-9a-f]+$")
    set(_root_sentinel_name ".appellate-release-root")
    set(_root_sentinel_value "appellate-signed-release-root\n")
else()
    set(_scratch_parent_absolute "${_build_root_absolute}")
    set(_scratch_parent_real "${_build_root_real}")
    set(_root_name "archive-reproducibility-${_suffix}")
    set(_root_name_pattern "^archive-reproducibility-[0-9a-f]+$")
    set(_root_sentinel_name ".appellate-reproducibility-root")
    set(_root_sentinel_value "appellate-reproducibility-root\n")
endif()

set(_root "${_scratch_parent_absolute}/${_root_name}")
set(_snapshot_source "${_root}/exact-source-snapshot")
set(_first_build "${_root}/first-clean-build")
set(_second_build "${_root}/second-clean-build-with-distinct-path-length")
set(_first_artifacts "${_root}/first-artifacts")
set(_second_artifacts "${_root}/second-artifacts")
get_filename_component(_root_parent "${_root}" DIRECTORY)
if(EXISTS "${_root}" OR EXISTS "${_first_build}" OR EXISTS "${_second_build}" OR
   IS_SYMLINK "${_root}" OR NOT _root_parent STREQUAL _scratch_parent_absolute OR
   NOT _root_name MATCHES "${_root_name_pattern}" OR
   _first_build STREQUAL _second_build)
    message(FATAL_ERROR "Archive reproducibility build roots must be distinct and initially absent")
endif()
file(MAKE_DIRECTORY "${_root}")
set(_root_sentinel_path "${_root}/${_root_sentinel_name}")
file(WRITE "${_root_sentinel_path}" "${_root_sentinel_value}")
file(REAL_PATH "${_root}" _root_real)
get_filename_component(_root_real_parent "${_root_real}" DIRECTORY)
if(NOT _root_real_parent STREQUAL _scratch_parent_real OR
   NOT _root_real STREQUAL "${_scratch_parent_real}/${_root_name}")
    message(FATAL_ERROR "Archive reproducibility root escapes its exact build-root parent")
endif()

function(_appellate_cleanup_reproducibility_root)
    get_filename_component(_final_root_parent "${_root}" DIRECTORY)
    get_filename_component(_final_root_name "${_root}" NAME)
    if(NOT _final_root_parent STREQUAL _scratch_parent_absolute OR
       NOT _final_root_name STREQUAL _root_name OR IS_SYMLINK "${_root}" OR
       NOT IS_DIRECTORY "${_root}" OR IS_SYMLINK "${_root_sentinel_path}" OR
       NOT EXISTS "${_root_sentinel_path}" OR IS_DIRECTORY "${_root_sentinel_path}")
        message(FATAL_ERROR "Refusing to remove a reproducibility root without its exact sentinel")
    endif()
    file(REAL_PATH "${_root}" _final_root_real)
    get_filename_component(_final_root_real_parent "${_final_root_real}" DIRECTORY)
    if(NOT _final_root_real STREQUAL _root_real OR
       NOT _final_root_real_parent STREQUAL _scratch_parent_real)
        message(FATAL_ERROR "Refusing to remove a replaced reproducibility root")
    endif()
    file(READ "${_root_sentinel_path}" _root_sentinel)
    if(NOT _root_sentinel STREQUAL _root_sentinel_value)
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

function(_appellate_assert_signed_git_identity repository label)
    execute_process(
        COMMAND
            "${APPELLATE_GIT_EXECUTABLE}" -C "${repository}"
            rev-parse --verify "refs/tags/${_expected_tag}^{commit}"
        RESULT_VARIABLE _tag_result
        OUTPUT_VARIABLE _tag_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _tag_error
    )
    execute_process(
        COMMAND
            "${APPELLATE_GIT_EXECUTABLE}" -C "${repository}"
            verify-tag --raw "${_expected_tag}"
        RESULT_VARIABLE _verify_result
        OUTPUT_VARIABLE _verify_output
        ERROR_VARIABLE _verify_error
    )
    set(_verify_text "${_verify_output}\n${_verify_error}")
    string(
        REGEX MATCH
        "\\[GNUPG:\\] VALIDSIG ([0-9A-Fa-f]+)"
        _validsig
        "${_verify_text}"
    )
    set(_actual_fingerprint "${CMAKE_MATCH_1}")
    string(TOUPPER "${_actual_fingerprint}" _actual_fingerprint)
    if(NOT _tag_result EQUAL 0 OR
       NOT _tag_revision STREQUAL APPELLATE_RELEASE_SOURCE_REVISION OR
       NOT _verify_result EQUAL 0 OR NOT _validsig OR
       NOT _actual_fingerprint STREQUAL _expected_fingerprint)
        _appellate_fail_with_cleanup(
            "${label} does not carry the exact signed release identity:\n"
            "tag revision: ${_tag_revision}\n"
            "tag fingerprint: ${_actual_fingerprint}\n"
            "${_tag_error}${_verify_text}"
        )
    endif()
endfunction()

if(_signed_candidate)
    _appellate_assert_signed_git_identity("${APPELLATE_SOURCE_DIR}" "Real source worktree")
endif()

set(
    _snapshot_clone_command
    "${APPELLATE_GIT_EXECUTABLE}" clone --local --no-checkout
)
if(_signed_candidate)
    list(APPEND _snapshot_clone_command --no-hardlinks)
else()
    list(APPEND _snapshot_clone_command --no-tags)
endif()
list(APPEND _snapshot_clone_command -- "${APPELLATE_SOURCE_DIR}" "${_snapshot_source}")
execute_process(
    COMMAND ${_snapshot_clone_command}
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
if(_signed_candidate)
    _appellate_assert_signed_git_identity("${_snapshot_source}" "Exact source snapshot")
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
    "-DAPPELLATE_ENABLE_ASAN=OFF"
    "-DAPPELLATE_ENABLE_LINUX_PACKAGING=ON"
    "-DAPPELLATE_ENABLE_SYNC=ON"
    "-DAPPELLATE_ENABLE_UBSAN=OFF"
    "-DAPPELLATE_GLIBC_FLOOR=${APPELLATE_GLIBC_FLOOR}"
    "-DAPPELLATE_RELEASE_SOURCE_EPOCH=${APPELLATE_RELEASE_SOURCE_EPOCH}"
    "-DAPPELLATE_RELEASE_SOURCE_REVISION=${APPELLATE_RELEASE_SOURCE_REVISION}"
)
if(_signed_candidate)
    list(
        APPEND _configure_arguments
        "-DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=OFF"
        "-DAPPELLATE_INTERNAL_SIGNED_RELEASE_BUILD=ON"
        "-DAPPELLATE_SIGNED_RELEASE_ROOT=${_root}"
        "-DAPPELLATE_RELEASE_SIGNING_FINGERPRINT=${_expected_fingerprint}"
    )
else()
    list(
        APPEND _configure_arguments
        "-DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=ON"
        "-DAPPELLATE_RELEASE_SIGNING_FINGERPRINT="
    )
endif()

function(_appellate_configure_reproducible_build build_dir label driver_token)
    if(EXISTS "${build_dir}")
        _appellate_fail_with_cleanup("${label} was not initially absent: ${build_dir}")
    endif()
    set(_build_configure_arguments ${_configure_arguments})
    if(_signed_candidate)
        list(
            APPEND _build_configure_arguments
            "-DAPPELLATE_SIGNED_RELEASE_TOKEN=${driver_token}"
        )
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env ${_reproducible_environment}
            "${CMAKE_COMMAND}"
            -S "${_snapshot_source}"
            -B "${build_dir}"
            ${_build_configure_arguments}
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

function(_appellate_package_reproducible_bundle build_dir artifact_dir label driver_token)
    if(EXISTS "${artifact_dir}")
        _appellate_fail_with_cleanup("${label} artifact directory was not initially absent")
    endif()
    set(
        _cpack_command
        "${APPELLATE_CPACK_EXECUTABLE}"
        --config "${build_dir}/CPackConfig.cmake"
        -G TZST
        -B "${artifact_dir}"
    )
    if(_signed_candidate)
        file(MAKE_DIRECTORY "${artifact_dir}")
        file(
            WRITE "${artifact_dir}/.appellate-signed-artifact-directory"
            "appellate-signed-artifact-directory\n"
        )
        list(
            INSERT _cpack_command 1
            -D "APPELLATE_SIGNED_RELEASE_RUN_TOKEN=${driver_token}"
        )
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env ${_reproducible_environment}
            ${_cpack_command}
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

function(_appellate_assert_candidate_file path label)
    if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}" OR IS_SYMLINK "${path}")
        _appellate_fail_with_cleanup("${label} is missing or not an ordinary file")
    endif()
    execute_process(
        COMMAND "${_appellate_stat_executable}" --format=%F|%h -- "${path}"
        RESULT_VARIABLE _stat_result
        OUTPUT_VARIABLE _stat_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _stat_error
    )
    if(NOT _stat_result EQUAL 0 OR NOT _stat_output STREQUAL "regular file|1")
        _appellate_fail_with_cleanup(
            "${label} must be a single-link regular file: ${_stat_output}${_stat_error}"
        )
    endif()
endfunction()

function(_appellate_assert_candidate_parent_unchanged)
    if(IS_SYMLINK "${_candidate_root_absolute}" OR
       NOT IS_DIRECTORY "${_candidate_root_absolute}" OR
       IS_SYMLINK "${_candidate_parent_sentinel}" OR
       NOT EXISTS "${_candidate_parent_sentinel}" OR
       IS_DIRECTORY "${_candidate_parent_sentinel}")
        message(FATAL_ERROR "Signed candidate parent changed; refusing path-based cleanup or promotion")
    endif()
    file(REAL_PATH "${_candidate_root_absolute}" _current_candidate_root_real)
    get_filename_component(
        _current_candidate_root_real_parent "${_current_candidate_root_real}" DIRECTORY
    )
    file(READ "${_candidate_parent_sentinel}" _current_candidate_parent_sentinel_text)
    execute_process(
        COMMAND
            "${_appellate_stat_executable}" --format=%d|%i|%F|%u|%a --
            "${_candidate_root_absolute}"
        RESULT_VARIABLE _current_candidate_root_stat_result
        OUTPUT_VARIABLE _current_candidate_root_identity
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _current_candidate_root_stat_error
    )
    execute_process(
        COMMAND
            "${_appellate_stat_executable}" --format=%d|%i|%F|%h --
            "${_candidate_parent_sentinel}"
        RESULT_VARIABLE _current_candidate_sentinel_stat_result
        OUTPUT_VARIABLE _current_candidate_parent_sentinel_identity
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _current_candidate_sentinel_stat_error
    )
    if(NOT _current_candidate_root_real STREQUAL _candidate_root_real OR
       NOT _current_candidate_root_real_parent STREQUAL _build_root_real OR
       NOT _current_candidate_parent_sentinel_text STREQUAL
           "appellate-release-candidate-root\n" OR
       NOT _current_candidate_root_stat_result EQUAL 0 OR
       NOT _current_candidate_root_identity STREQUAL _candidate_root_identity OR
       NOT _current_candidate_sentinel_stat_result EQUAL 0 OR
       NOT _current_candidate_parent_sentinel_identity STREQUAL
           _candidate_parent_sentinel_identity)
        message(
            FATAL_ERROR
            "Signed candidate parent identity changed; refusing path-based cleanup or promotion: "
            "${_current_candidate_root_stat_error}${_current_candidate_sentinel_stat_error}"
        )
    endif()
endfunction()

if(NOT _signed_candidate)
    set(_first_driver_token "")
    set(_second_driver_token "")
endif()
_appellate_configure_reproducible_build(
    "${_first_build}" "First clean build" "${_first_driver_token}"
)
_appellate_configure_reproducible_build(
    "${_second_build}" "Second clean build" "${_second_driver_token}"
)
_appellate_build_reproducible_bundle("${_first_build}" "First clean build")
_appellate_build_reproducible_bundle("${_second_build}" "Second clean build")
_appellate_package_reproducible_bundle(
    "${_first_build}" "${_first_artifacts}" "First clean build" "${_first_driver_token}"
)
_appellate_package_reproducible_bundle(
    "${_second_build}" "${_second_artifacts}" "Second clean build" "${_second_driver_token}"
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

if(_signed_candidate)
    _appellate_assert_signed_git_identity("${APPELLATE_SOURCE_DIR}" "Final real source worktree")
    _appellate_assert_signed_git_identity("${_snapshot_source}" "Final exact source snapshot")

    set(_candidate_archive "${_root}/${_archive_name}")
    set(_candidate_checksum "${_candidate_archive}.sha256")
    file(
        RENAME "${_first_archive}" "${_candidate_archive}"
        RESULT _archive_move_result NO_REPLACE
    )
    if(NOT _archive_move_result STREQUAL "0")
        _appellate_fail_with_cleanup(
            "Verified archive could not enter candidate staging: ${_archive_move_result}"
        )
    endif()
    file(
        RENAME "${_first_checksum}" "${_candidate_checksum}"
        RESULT _checksum_move_result NO_REPLACE
    )
    if(NOT _checksum_move_result STREQUAL "0")
        _appellate_fail_with_cleanup(
            "Verified checksum could not enter candidate staging: ${_checksum_move_result}"
        )
    endif()
    _appellate_verify_checksum(
        "${_candidate_archive}" "${_candidate_checksum}"
        "Promoted signed candidate" _candidate_sha
    )
    if(NOT _candidate_sha STREQUAL _first_sha)
        _appellate_fail_with_cleanup("Signed candidate archive changed during staging")
    endif()
    file(READ "${_candidate_checksum}" _candidate_checksum_text)
    string(STRIP "${_candidate_checksum_text}" _candidate_checksum_text)
    string(REGEX MATCH "^[0-9a-f]+[ \t]+(.+)$" _candidate_checksum_match
        "${_candidate_checksum_text}")
    set(_candidate_checksum_reported_path "${CMAKE_MATCH_1}")
    get_filename_component(
        _candidate_checksum_reported_name "${_candidate_checksum_reported_path}" NAME
    )
    if(NOT _candidate_checksum_match OR
       NOT _candidate_checksum_reported_name STREQUAL _archive_name)
        _appellate_fail_with_cleanup("Signed candidate checksum names the wrong archive")
    endif()
    file(SIZE "${_candidate_archive}" _candidate_archive_size)

    set(_candidate_manifest "${_root}/candidate.json")
    set(_candidate_pending_marker "${_root}/.BUILD_COMPLETE.pending")
    file(
        WRITE "${_candidate_manifest}"
        "{\n"
        "  \"schema\": 1,\n"
        "  \"status\": \"signed-tag-exact-build-candidate\",\n"
        "  \"version\": \"${APPELLATE_PROJECT_VERSION}\",\n"
        "  \"source_revision\": \"${APPELLATE_RELEASE_SOURCE_REVISION}\",\n"
        "  \"source_epoch\": ${APPELLATE_RELEASE_SOURCE_EPOCH},\n"
        "  \"tag\": \"${_expected_tag}\",\n"
        "  \"signing_fingerprint\": \"${_expected_fingerprint}\",\n"
        "  \"archive\": \"${_archive_name}\",\n"
        "  \"archive_sha256\": \"${_candidate_sha}\",\n"
        "  \"archive_size\": ${_candidate_archive_size},\n"
        "  \"same_image_builds\": 2\n"
        "}\n"
    )
    _appellate_assert_candidate_file("${_root_sentinel_path}" "Candidate root sentinel")
    _appellate_assert_candidate_file("${_candidate_archive}" "Candidate archive")
    _appellate_assert_candidate_file("${_candidate_checksum}" "Candidate checksum")
    _appellate_assert_candidate_file("${_candidate_manifest}" "Candidate manifest")

    file(
        REMOVE_RECURSE
        "${_snapshot_source}"
        "${_first_build}"
        "${_second_build}"
        "${_first_artifacts}"
        "${_second_artifacts}"
    )
    foreach(_removed_path IN ITEMS
            "${_snapshot_source}"
            "${_first_build}"
            "${_second_build}"
            "${_first_artifacts}"
            "${_second_artifacts}")
        if(EXISTS "${_removed_path}" OR IS_SYMLINK "${_removed_path}")
            _appellate_fail_with_cleanup(
                "Signed candidate scratch child could not be removed: ${_removed_path}"
            )
        endif()
    endforeach()

    file(
        GLOB _candidate_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${_root}"
        "${_root}/*"
        "${_root}/.[!.]*"
        "${_root}/..?*"
    )
    list(REMOVE_DUPLICATES _candidate_entries)
    list(SORT _candidate_entries)
    set(
        _expected_candidate_entries
        .appellate-release-root
        candidate.json
        "${_archive_name}"
        "${_archive_name}.sha256"
    )
    list(SORT _expected_candidate_entries)
    if(NOT _candidate_entries STREQUAL _expected_candidate_entries)
        _appellate_fail_with_cleanup(
            "Signed candidate staging allowlist mismatch: ${_candidate_entries}"
        )
    endif()
    if(EXISTS "${_candidate_pending_marker}" OR
       IS_SYMLINK "${_candidate_pending_marker}")
        _appellate_fail_with_cleanup("Signed candidate pending marker already exists")
    endif()
    file(
        WRITE "${_candidate_pending_marker}"
        "appellate-signed-candidate-build-complete\n"
    )
    _appellate_assert_candidate_file(
        "${_candidate_pending_marker}" "Candidate pending completion marker"
    )

    _appellate_assert_candidate_parent_unchanged()

    string(
        CONCAT _candidate_name
        "appellate-workbench-${APPELLATE_PROJECT_VERSION}-linux-x86_64-"
        "${APPELLATE_RELEASE_SOURCE_REVISION}-${_suffix}"
    )
    set(_candidate_directory "${_candidate_root_absolute}/${_candidate_name}")
    get_filename_component(_candidate_parent "${_candidate_directory}" DIRECTORY)
    if(NOT _candidate_parent STREQUAL _candidate_root_absolute OR
       EXISTS "${_candidate_directory}" OR IS_SYMLINK "${_candidate_directory}")
        _appellate_fail_with_cleanup("Signed candidate destination is not initially absent")
    endif()
    file(
        RENAME "${_root}" "${_candidate_directory}"
        RESULT _candidate_rename_result NO_REPLACE
    )
    if(NOT _candidate_rename_result STREQUAL "0")
        _appellate_assert_candidate_parent_unchanged()
        if(EXISTS "${_root}" AND NOT IS_SYMLINK "${_root}")
            _appellate_fail_with_cleanup(
                "Signed candidate atomic promotion failed: ${_candidate_rename_result}"
            )
        endif()
        message(FATAL_ERROR "Signed candidate promotion lost its sentinel-owned staging root")
    endif()
    _appellate_assert_candidate_parent_unchanged()
    if(IS_SYMLINK "${_candidate_directory}" OR
       NOT IS_DIRECTORY "${_candidate_directory}")
        message(FATAL_ERROR "Promoted signed candidate directory was replaced")
    endif()
    file(REAL_PATH "${_candidate_directory}" _candidate_directory_real)
    get_filename_component(_candidate_directory_real_parent "${_candidate_directory_real}" DIRECTORY)
    if(NOT _candidate_directory_real_parent STREQUAL _candidate_root_real OR
       NOT _candidate_directory_real STREQUAL
           "${_candidate_root_real}/${_candidate_name}")
        message(FATAL_ERROR "Promoted signed candidate directory escaped its exact parent")
    endif()
    set(
        _promoted_pending_marker
        "${_candidate_directory}/.BUILD_COMPLETE.pending"
    )
    set(_promoted_complete_marker "${_candidate_directory}/BUILD_COMPLETE")
    file(
        RENAME "${_promoted_pending_marker}" "${_promoted_complete_marker}"
        RESULT _candidate_marker_rename_result NO_REPLACE
    )
    if(NOT _candidate_marker_rename_result STREQUAL "0")
        message(
            FATAL_ERROR
            "Signed candidate was promoted without BUILD_COMPLETE eligibility: "
            "${_candidate_marker_rename_result}"
        )
    endif()
    message(STATUS "Signed exact-build candidate: ${_candidate_directory}")
    message(STATUS "Same-image signed candidate archive SHA-256: ${_candidate_sha}")
else()
    _appellate_cleanup_reproducibility_root()
    message(STATUS "Same-image clean-build archive SHA-256: ${_first_sha}")
endif()

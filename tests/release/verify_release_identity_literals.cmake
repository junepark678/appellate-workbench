cmake_minimum_required(VERSION 3.30)

foreach(
    _required
    IN ITEMS
        APPELLATE_CMAKE_GENERATOR
        APPELLATE_CPACK_EXECUTABLE
        APPELLATE_CXX_COMPILER
        APPELLATE_GIT_EXECUTABLE
        APPELLATE_PATCHELF_EXECUTABLE
        APPELLATE_QT6_DIR
        APPELLATE_SOURCE_DIR
        APPELLATE_TEST_ROOT
)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

set(
    _configure_arguments
    -G "${APPELLATE_CMAKE_GENERATOR}"
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_CXX_COMPILER=${APPELLATE_CXX_COMPILER}"
    "-DQt6_DIR=${APPELLATE_QT6_DIR}"
    -DBUILD_TESTING=OFF
    -DAPPELLATE_ENABLE_LINUX_PACKAGING=ON
    -DAPPELLATE_ENABLE_SYNC=OFF
)
if(DEFINED APPELLATE_CMAKE_MAKE_PROGRAM AND
   NOT APPELLATE_CMAKE_MAKE_PROGRAM STREQUAL "")
    list(
        APPEND
        _configure_arguments
        "-DCMAKE_MAKE_PROGRAM=${APPELLATE_CMAKE_MAKE_PROGRAM}"
    )
endif()

file(REMOVE_RECURSE "${APPELLATE_TEST_ROOT}")
file(MAKE_DIRECTORY "${APPELLATE_TEST_ROOT}")

include("${APPELLATE_SOURCE_DIR}/cmake/SafeCMakeLiteral.cmake")
set(
    _literal_round_trip
    [======[line one: "quote" and \backslash and $dollar
line two: ]=] then ]==] then ]===] then ]====] then ]=====]]======]
)
string(PREPEND _literal_round_trip "\n;leading newline and semicolon\n")
string(APPEND _literal_round_trip "\nending partial close: ]======")
set(_expected_literal "${_literal_round_trip}")
appellate_select_cmake_bracket_delimiter(
    _test_literal_open
    _test_literal_close
    _literal_round_trip
)
set(_literal_template "${APPELLATE_TEST_ROOT}/literal-round-trip.cmake.in")
set(_literal_script "${APPELLATE_TEST_ROOT}/literal-round-trip.cmake")
file(
    WRITE
    "${_literal_template}"
    [===[set(_encoded_literal @_test_literal_open@x@_literal_round_trip@x@_test_literal_close@)
string(LENGTH "${_encoded_literal}" _encoded_length)
math(EXPR _decoded_length "${_encoded_length} - 2")
string(SUBSTRING "${_encoded_literal}" 1 "${_decoded_length}" _decoded_literal)
]===]
)
configure_file("${_literal_template}" "${_literal_script}" @ONLY)
unset(_decoded_literal)
include("${_literal_script}")
if(NOT _decoded_literal STREQUAL _expected_literal)
    message(FATAL_ERROR "Safe CMake literal did not round-trip arbitrary text")
endif()

set(_injected_fingerprint [===[")
message(FATAL_ERROR "APPELLATE_TEMPLATE_INJECTION")
#]===])
set(_malformed_cache "${APPELLATE_TEST_ROOT}/malformed-fingerprint-cache.cmake")
file(
    WRITE
    "${_malformed_cache}"
    "set(APPELLATE_RELEASE_SIGNING_FINGERPRINT [====[${_injected_fingerprint}]====] "
    "CACHE STRING \"\" FORCE)\n"
)
set(_malformed_build "${APPELLATE_TEST_ROOT}/malformed-fingerprint")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -C "${_malformed_cache}"
        -S "${APPELLATE_SOURCE_DIR}"
        -B "${_malformed_build}"
        ${_configure_arguments}
        -DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=ON
    RESULT_VARIABLE _malformed_result
    OUTPUT_VARIABLE _malformed_output
    ERROR_VARIABLE _malformed_error
)
set(_malformed_log "${_malformed_output}\n${_malformed_error}")
if(_malformed_result EQUAL 0 OR
   NOT _malformed_log MATCHES
       "APPELLATE_RELEASE_SIGNING_FINGERPRINT must be empty or exactly 40")
    message(
        FATAL_ERROR
        "Hostile release fingerprint did not fail at configure-time validation:\n${_malformed_log}"
    )
endif()

set(_malformed_glibc_build "${APPELLATE_TEST_ROOT}/malformed-glibc")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${APPELLATE_SOURCE_DIR}"
        -B "${_malformed_glibc_build}"
        ${_configure_arguments}
        -DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=ON
        "-DAPPELLATE_GLIBC_FLOOR=2.42\"},\"forged\":true,{\"ignored\":\""
    RESULT_VARIABLE _malformed_glibc_result
    OUTPUT_VARIABLE _malformed_glibc_output
    ERROR_VARIABLE _malformed_glibc_error
)
set(_malformed_glibc_log "${_malformed_glibc_output}\n${_malformed_glibc_error}")
if(_malformed_glibc_result EQUAL 0 OR
   NOT _malformed_glibc_log MATCHES "APPELLATE_GLIBC_FLOOR must be")
    message(
        FATAL_ERROR
        "Malformed glibc metadata did not fail at configure-time validation:\n"
        "${_malformed_glibc_log}"
    )
endif()

set(
    _weird_component
    [=====[literal-"quote-$dollar-]=]-]==]-]===]]=====]
)
set(_weird_git "${APPELLATE_TEST_ROOT}/${_weird_component}")
file(CREATE_LINK "${APPELLATE_GIT_EXECUTABLE}" "${_weird_git}" SYMBOLIC)
set(_weird_patchelf "${APPELLATE_TEST_ROOT}/${_weird_component}-patchelf")
file(CREATE_LINK "${APPELLATE_PATCHELF_EXECUTABLE}" "${_weird_patchelf}" SYMBOLIC)
set(_weird_build "${APPELLATE_TEST_ROOT}/quoted-git-path-build")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${APPELLATE_SOURCE_DIR}"
        -B "${_weird_build}"
        ${_configure_arguments}
        -DAPPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE=OFF
        -DAPPELLATE_RELEASE_SIGNING_FINGERPRINT=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
        "-DAPPELLATE_PATCHELF_EXECUTABLE=${_weird_patchelf}"
        "-DGIT_EXECUTABLE=${_weird_git}"
    RESULT_VARIABLE _weird_configure_result
    OUTPUT_VARIABLE _weird_configure_output
    ERROR_VARIABLE _weird_configure_error
)
if(NOT _weird_configure_result EQUAL 0)
    message(
        FATAL_ERROR
        "Quoted release verifier path did not configure:\n"
        "${_weird_configure_output}\n${_weird_configure_error}"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DCMAKE_INSTALL_PREFIX=${APPELLATE_TEST_ROOT}/empty-prefix"
        -P "${_weird_build}/release/linux-bundle-fixup.cmake"
    RESULT_VARIABLE _fixup_result
    OUTPUT_VARIABLE _fixup_output
    ERROR_VARIABLE _fixup_error
)
if(NOT _fixup_result EQUAL 0)
    message(
        FATAL_ERROR
        "Quoted Linux fixup path did not parse:\n${_fixup_output}\n${_fixup_error}"
    )
endif()

set(_package_output "${APPELLATE_TEST_ROOT}/package-output")
file(MAKE_DIRECTORY "${_package_output}")
execute_process(
    COMMAND
        "${APPELLATE_CPACK_EXECUTABLE}"
        --config "${_weird_build}/CPackConfig.cmake"
        -G TZST
        -B "${_package_output}"
    WORKING_DIRECTORY "${_weird_build}"
    RESULT_VARIABLE _cpack_result
    OUTPUT_VARIABLE _cpack_output
    ERROR_VARIABLE _cpack_error
)
set(_cpack_log "${_cpack_output}\n${_cpack_error}")
if(_cpack_result EQUAL 0 OR
   NOT _cpack_log MATCHES
       "Direct production CPack is disabled")
    message(
        FATAL_ERROR
        "Quoted release verifier path did not reach direct-production rejection:\n"
        "${_cpack_log}"
    )
endif()
file(
    GLOB_RECURSE
    _unexpected_packages
    LIST_DIRECTORIES FALSE
    "${_package_output}/*.tar.zst"
    "${_package_output}/*.sha256"
)
if(_unexpected_packages)
    message(FATAL_ERROR "Rejected direct production CPack emitted an artifact")
endif()

file(REMOVE_RECURSE "${APPELLATE_TEST_ROOT}")
message(STATUS "Release verifier literals reject injection and preserve quoted paths")

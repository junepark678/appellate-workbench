include_guard(GLOBAL)

include(GNUInstallDirs)

if(CMAKE_VERSION VERSION_LESS 4.3)
    message(FATAL_ERROR "Linux archive packaging requires CMake 4.3 or newer")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "The MVP binary package is defined only for Linux x86_64")
endif()
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The Linux MVP bundle must be built natively")
endif()
if(NOT QT6_IS_SHARED_LIBS_BUILD)
    message(FATAL_ERROR "The Linux MVP bundle requires a shared Qt build")
endif()
if(Qt6_VERSION VERSION_LESS 6.10)
    message(FATAL_ERROR "Linux deployment requires Qt 6.10 or newer")
endif()
if(CMAKE_CONFIGURATION_TYPES OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "The Linux MVP bundle must use the single-config Release build type")
endif()
if(APPELLATE_ENABLE_ASAN OR APPELLATE_ENABLE_UBSAN)
    message(FATAL_ERROR "Sanitizer runtimes are forbidden in the Linux MVP bundle")
endif()
find_program(APPELLATE_PATCHELF_EXECUTABLE NAMES patchelf REQUIRED)
find_package(Git REQUIRED)

option(
    APPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE
    "Permit a conspicuously named package from a dirty, untagged, or unsigned source tree"
    OFF
)
set(
    APPELLATE_RELEASE_SIGNING_FINGERPRINT
    ""
    CACHE STRING
    "Expected OpenPGP signing-key fingerprint for the exact release tag"
)

set(
    APPELLATE_RELEASE_SOURCE_REVISION
    ""
    CACHE STRING
    "Exact source revision recorded in the release compatibility manifest"
)
if(APPELLATE_RELEASE_SOURCE_REVISION STREQUAL "")
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        RESULT_VARIABLE _appellate_git_result
        OUTPUT_VARIABLE _appellate_git_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_appellate_git_result EQUAL 0)
        set(APPELLATE_RELEASE_SOURCE_REVISION "${_appellate_git_revision}")
    else()
        set(APPELLATE_RELEASE_SOURCE_REVISION "unknown")
    endif()
endif()
string(LENGTH "${APPELLATE_RELEASE_SOURCE_REVISION}" _appellate_revision_length)
if(NOT _appellate_revision_length EQUAL 40 OR
   NOT APPELLATE_RELEASE_SOURCE_REVISION MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "APPELLATE_RELEASE_SOURCE_REVISION must be a lowercase SHA-1 commit ID")
endif()

if(APPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE)
    set(APPELLATE_RELEASE_IDENTITY_STATUS "development-unverified")
else()
    set(APPELLATE_RELEASE_IDENTITY_STATUS "signed-tag-required")
endif()

set(
    APPELLATE_GLIBC_FLOOR
    ""
    CACHE STRING
    "Oldest glibc version supported by this exact Linux bundle"
)
if(APPELLATE_GLIBC_FLOOR STREQUAL "")
    execute_process(
        COMMAND getconf GNU_LIBC_VERSION
        RESULT_VARIABLE _appellate_glibc_result
        OUTPUT_VARIABLE _appellate_glibc_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_appellate_glibc_result EQUAL 0 AND _appellate_glibc_output MATCHES "^glibc ([0-9]+\\.[0-9]+)$")
        set(APPELLATE_GLIBC_FLOOR "${CMAKE_MATCH_1}")
    else()
        set(APPELLATE_GLIBC_FLOOR "unverified")
    endif()
endif()

set(
    _appellate_asterglen_v2_archive
    "${PROJECT_SOURCE_DIR}/content/ca4-rule54b/us-ca4-rule54b-asterglen-0.2.0.awpack"
)
set(
    _appellate_federal_archive
    "${PROJECT_SOURCE_DIR}/content/foundations/us-federal/foundation-us-federal-2025.12.01.awpack"
)
set(
    _appellate_ca4_archive
    "${PROJECT_SOURCE_DIR}/content/foundations/us-ca4/foundation-us-ca4-2026.03.23.awpack"
)
set(
    _appellate_bench_archive
    "${PROJECT_SOURCE_DIR}/content/foundations/us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack"
)
set(
    _appellate_asterglen_v1_archive
    "${PROJECT_SOURCE_DIR}/content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack"
)

set(APPELLATE_ASTERGLEN_V2_ARCHIVE_SHA256
    "10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819")
set(APPELLATE_ASTERGLEN_V2_ARCHIVE_SIZE 3974147)
set(APPELLATE_ASTERGLEN_V2_PACK_REVISION
    "7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728")
set(APPELLATE_FEDERAL_ARCHIVE_SHA256
    "69736648f78376a6d85cde32148337edbf5af2a289de6070734c5454cc6b411b")
set(APPELLATE_FEDERAL_ARCHIVE_SIZE 21002)
set(APPELLATE_FEDERAL_PACK_REVISION
    "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9")
set(APPELLATE_CA4_ARCHIVE_SHA256
    "5c9098d76012891ab2cb1f04c48bdcb3101c64253fdaab1608de789d0f5aa6ef")
set(APPELLATE_CA4_ARCHIVE_SIZE 66512)
set(APPELLATE_CA4_PACK_REVISION
    "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262")
set(APPELLATE_BENCH_ARCHIVE_SHA256
    "e2758217f5ba9b987cc9e9920af65f762263f420e1698b12732d4f02b0121137")
set(APPELLATE_BENCH_ARCHIVE_SIZE 14131)
set(APPELLATE_BENCH_PACK_REVISION
    "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d")
set(APPELLATE_ASTERGLEN_V1_ARCHIVE_SHA256
    "ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227")
set(APPELLATE_ASTERGLEN_V1_ARCHIVE_SIZE 729511)
set(APPELLATE_ASTERGLEN_V1_PACK_REVISION
    "ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424")

function(_appellate_assert_release_archive archive expected_sha256 expected_size)
    if(NOT EXISTS "${archive}")
        message(FATAL_ERROR "Required release archive is missing: ${archive}")
    endif()
    file(SHA256 "${archive}" _actual_sha256)
    file(SIZE "${archive}" _actual_size)
    if(NOT _actual_sha256 STREQUAL expected_sha256 OR NOT _actual_size EQUAL expected_size)
        message(
            FATAL_ERROR
            "Pinned release archive changed: ${archive}\n"
            "expected SHA-256/size: ${expected_sha256}/${expected_size}\n"
            "actual SHA-256/size:   ${_actual_sha256}/${_actual_size}"
        )
    endif()
endfunction()

set(_appellate_release_pack_archives
    "${_appellate_asterglen_v2_archive}"
    "${_appellate_federal_archive}"
    "${_appellate_ca4_archive}"
    "${_appellate_bench_archive}"
    "${_appellate_asterglen_v1_archive}"
)
set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_appellate_release_pack_archives}
)
_appellate_assert_release_archive(
    "${_appellate_asterglen_v2_archive}"
    "${APPELLATE_ASTERGLEN_V2_ARCHIVE_SHA256}"
    "${APPELLATE_ASTERGLEN_V2_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_federal_archive}"
    "${APPELLATE_FEDERAL_ARCHIVE_SHA256}"
    "${APPELLATE_FEDERAL_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_ca4_archive}"
    "${APPELLATE_CA4_ARCHIVE_SHA256}"
    "${APPELLATE_CA4_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_bench_archive}"
    "${APPELLATE_BENCH_ARCHIVE_SHA256}"
    "${APPELLATE_BENCH_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_asterglen_v1_archive}"
    "${APPELLATE_ASTERGLEN_V1_ARCHIVE_SHA256}"
    "${APPELLATE_ASTERGLEN_V1_ARCHIVE_SIZE}"
)

set(_appellate_release_generated "${PROJECT_BINARY_DIR}/release")
file(MAKE_DIRECTORY "${_appellate_release_generated}")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/release-compatibility.json.in"
    "${_appellate_release_generated}/compatibility.json"
    @ONLY
)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/LinuxBundleFixup.cmake.in"
    "${_appellate_release_generated}/linux-bundle-fixup.cmake"
    @ONLY
)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/VerifyReleaseIdentity.cmake.in"
    "${_appellate_release_generated}/verify-release-identity.cmake"
    @ONLY
)

install(
    TARGETS appellate-workbench appellate-pack appellate-render
    BUNDLE DESTINATION .
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    FILES ${_appellate_release_pack_archives}
    DESTINATION "${CMAKE_INSTALL_DATADIR}/appellate-workbench/packs"
)
install(
    FILES "${_appellate_release_generated}/compatibility.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/appellate-workbench"
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/docs/PRODUCT.md"
        "${PROJECT_SOURCE_DIR}/docs/ARCHITECTURE.md"
        "${PROJECT_SOURCE_DIR}/docs/REALISM_MATRIX.md"
        "${PROJECT_SOURCE_DIR}/docs/RELEASE.md"
        "${PROJECT_SOURCE_DIR}/docs/spec/PACKS.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/appellate-workbench"
)

set(QT_DEPLOY_USE_PATCHELF ON)
set(QT_DEPLOY_PATCHELF_EXECUTABLE "${APPELLATE_PATCHELF_EXECUTABLE}")
qt_generate_deploy_app_script(
    TARGET appellate-workbench
    OUTPUT_SCRIPT _appellate_deploy_script
    NO_TRANSLATIONS
    EXCLUDE_PLUGIN_TYPES
        accessiblebridge
        platforms
        platforms_darwin
        xcbglintegrations
        platformthemes
        platforminputcontexts
        generic
        iconengines
        imageformats
        egldeviceintegrations
        wayland_graphics_integration_client
        wayland_decoration_client
        wayland_shell_integration
        wayland-graphics-integration-client
        wayland-decoration-client
        wayland-shell-integration
        networkaccess
        networkinformation
        tls
        sqldrivers
        styles
    INCLUDE_PLUGINS qoffscreen qsqlite qxcb
    PRE_EXCLUDE_REGEXES
        "^ld-linux.*"
        "^libanl[.]so"
        "^libBrokenLocale[.]so"
        "^libc[.]so"
        "^libdl[.]so"
        "^libm[.]so"
        "^libnss_"
        "^libpthread[.]so"
        "^libresolv[.]so"
        "^librt[.]so"
        "^libthread_db[.]so"
        "^libutil[.]so"
)
install(
    CODE
        "if(POLICY CMP0207)\n  set(CMAKE_POLICY_DEFAULT_CMP0207 NEW)\n  cmake_policy(SET CMP0207 NEW)\nendif()"
)
install(SCRIPT "${_appellate_deploy_script}")
install(SCRIPT "${_appellate_release_generated}/linux-bundle-fixup.cmake")

set(CPACK_GENERATOR "TZST")
set(CPACK_PACKAGE_NAME "appellate-workbench")
set(CPACK_PACKAGE_VENDOR "Appellate Workbench")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local-first native appellate practice simulator")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(APPELLATE_ALLOW_UNVERIFIED_DEVELOPMENT_PACKAGE)
    set(
        CPACK_PACKAGE_FILE_NAME
        "appellate-workbench-${PROJECT_VERSION}-linux-x86_64-development-unverified"
    )
else()
    set(CPACK_PACKAGE_FILE_NAME "appellate-workbench-${PROJECT_VERSION}-linux-x86_64")
endif()
set(APPELLATE_BINARY_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_ARCHIVE_THREADS 1)
set(CPACK_ARCHIVE_UID 0)
set(CPACK_ARCHIVE_GID 0)
set(CPACK_VERBATIM_VARIABLES YES)
set(
    CPACK_PROJECT_CONFIG_FILE
    "${_appellate_release_generated}/verify-release-identity.cmake"
)

include(CPack)

if(BUILD_TESTING)
    add_test(
        NAME linux_bundle_smoke
        COMMAND
            "${CMAKE_COMMAND}"
            "-DAPPELLATE_BUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DAPPELLATE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            -P "${PROJECT_SOURCE_DIR}/tests/release/verify_install.cmake"
    )
    set_tests_properties(
        linux_bundle_smoke
        PROPERTIES
            LABELS "e2e;ui;local;packaging"
            TIMEOUT 180
    )

    add_test(
        NAME linux_archive_smoke
        COMMAND
            "${CMAKE_COMMAND}"
            "-DAPPELLATE_BUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DAPPELLATE_CPACK_EXECUTABLE=${CMAKE_CPACK_COMMAND}"
            "-DAPPELLATE_PACKAGE_FILE_NAME=${APPELLATE_BINARY_PACKAGE_FILE_NAME}"
            "-DAPPELLATE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DAPPELLATE_VERIFY_INSTALL_SCRIPT=${PROJECT_SOURCE_DIR}/tests/release/verify_install.cmake"
            -P "${PROJECT_SOURCE_DIR}/tests/release/verify_archive.cmake"
    )
    set_tests_properties(
        linux_archive_smoke
        PROPERTIES
            LABELS "e2e;ui;local;packaging"
            TIMEOUT 300
    )
endif()

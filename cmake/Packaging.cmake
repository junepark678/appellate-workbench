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
    _appellate_gold_archive
    "${PROJECT_SOURCE_DIR}/content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_appellate_gold_archive}")
file(SHA256 "${_appellate_gold_archive}" APPELLATE_GOLD_ARCHIVE_SHA256)
set(
    APPELLATE_GOLD_PACK_REVISION
    "ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424"
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
    FILES "${_appellate_gold_archive}"
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
            "-DAPPELLATE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
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

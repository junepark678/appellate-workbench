include_guard(GLOBAL)

include(GNUInstallDirs)

if(CMAKE_VERSION VERSION_LESS 4.3)
    message(FATAL_ERROR "Linux archive packaging requires CMake 4.3 or newer")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "The pre-MVP binary package is defined only for Linux x86_64")
endif()
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The Linux pre-MVP bundle must be built natively")
endif()
if(NOT QT6_IS_SHARED_LIBS_BUILD)
    message(FATAL_ERROR "The Linux pre-MVP bundle requires a shared Qt build")
endif()
if(Qt6_VERSION VERSION_LESS 6.10)
    message(FATAL_ERROR "Linux deployment requires Qt 6.10 or newer")
endif()
if(CMAKE_CONFIGURATION_TYPES OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "The Linux pre-MVP bundle must use the single-config Release build type")
endif()
if(APPELLATE_ENABLE_ASAN OR APPELLATE_ENABLE_UBSAN)
    message(FATAL_ERROR "Sanitizer runtimes are forbidden in the Linux pre-MVP bundle")
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
set(
    _appellate_cinderlake_writ_archive
    "${PROJECT_SOURCE_DIR}/content/m4/cinderlake-writ/us-ca4-m4-cinderlake-writ-1.2.0.awpack"
)
set(
    _appellate_arm_agency_archive
    "${PROJECT_SOURCE_DIR}/content/m4/arm-agency/us-ca4-m4-arm-agency-1.2.0.awpack"
)
set(
    _appellate_benton_retaliation_archive
    "${PROJECT_SOURCE_DIR}/content/m4/benton-retaliation/us-ca4-m4-benton-retaliation-1.2.0.awpack"
)
set(
    _appellate_norvale_injunction_archive
    "${PROJECT_SOURCE_DIR}/content/m4/norvale-injunction/us-ca4-m4-norvale-injunction-1.2.0.awpack"
)
set(
    _appellate_ellison_immunity_archive
    "${PROJECT_SOURCE_DIR}/content/m4/ellison-immunity/us-ca4-m4-ellison-immunity-1.2.0.awpack"
)
set(
    _appellate_blueember_jmol_archive
    "${PROJECT_SOURCE_DIR}/content/m4/blueember-jmol/us-ca4-m4-blueember-jmol-1.2.0.awpack"
)
set(
    _appellate_opengrid_foia_archive
    "${PROJECT_SOURCE_DIR}/content/m4/opengrid-foia/us-ca4-m4-opengrid-foia-1.2.0.awpack"
)
set(
    _appellate_serrano_waiver_archive
    "${PROJECT_SOURCE_DIR}/content/m4/serrano-waiver/us-ca4-m4-serrano-waiver-1.2.0.awpack"
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
set(APPELLATE_CINDERLAKE_WRIT_ARCHIVE_SHA256
    "eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956")
set(APPELLATE_CINDERLAKE_WRIT_ARCHIVE_SIZE 2519053)
set(APPELLATE_CINDERLAKE_WRIT_PACK_REVISION
    "020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c")
set(APPELLATE_ARM_AGENCY_ARCHIVE_SHA256
    "a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2")
set(APPELLATE_ARM_AGENCY_ARCHIVE_SIZE 3286508)
set(APPELLATE_ARM_AGENCY_PACK_REVISION
    "ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb")
set(APPELLATE_BENTON_RETALIATION_ARCHIVE_SHA256
    "9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05")
set(APPELLATE_BENTON_RETALIATION_ARCHIVE_SIZE 3408701)
set(APPELLATE_BENTON_RETALIATION_PACK_REVISION
    "59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28")
set(APPELLATE_NORVALE_INJUNCTION_ARCHIVE_SHA256
    "a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19")
set(APPELLATE_NORVALE_INJUNCTION_ARCHIVE_SIZE 4744009)
set(APPELLATE_NORVALE_INJUNCTION_PACK_REVISION
    "a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f")
set(APPELLATE_ELLISON_IMMUNITY_ARCHIVE_SHA256
    "59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0")
set(APPELLATE_ELLISON_IMMUNITY_ARCHIVE_SIZE 4230462)
set(APPELLATE_ELLISON_IMMUNITY_PACK_REVISION
    "c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0")
set(APPELLATE_BLUEEMBER_JMOL_ARCHIVE_SHA256
    "c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2")
set(APPELLATE_BLUEEMBER_JMOL_ARCHIVE_SIZE 5326158)
set(APPELLATE_BLUEEMBER_JMOL_PACK_REVISION
    "08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec")
set(APPELLATE_OPENGRID_FOIA_ARCHIVE_SHA256
    "1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09")
set(APPELLATE_OPENGRID_FOIA_ARCHIVE_SIZE 5244039)
set(APPELLATE_OPENGRID_FOIA_PACK_REVISION
    "9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5")
set(APPELLATE_SERRANO_WAIVER_ARCHIVE_SHA256
    "d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243")
set(APPELLATE_SERRANO_WAIVER_ARCHIVE_SIZE 3453568)
set(APPELLATE_SERRANO_WAIVER_PACK_REVISION
    "9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344")

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
    "${_appellate_cinderlake_writ_archive}"
    "${_appellate_arm_agency_archive}"
    "${_appellate_benton_retaliation_archive}"
    "${_appellate_norvale_injunction_archive}"
    "${_appellate_ellison_immunity_archive}"
    "${_appellate_blueember_jmol_archive}"
    "${_appellate_opengrid_foia_archive}"
    "${_appellate_serrano_waiver_archive}"
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
_appellate_assert_release_archive(
    "${_appellate_cinderlake_writ_archive}"
    "${APPELLATE_CINDERLAKE_WRIT_ARCHIVE_SHA256}"
    "${APPELLATE_CINDERLAKE_WRIT_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_arm_agency_archive}"
    "${APPELLATE_ARM_AGENCY_ARCHIVE_SHA256}"
    "${APPELLATE_ARM_AGENCY_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_benton_retaliation_archive}"
    "${APPELLATE_BENTON_RETALIATION_ARCHIVE_SHA256}"
    "${APPELLATE_BENTON_RETALIATION_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_norvale_injunction_archive}"
    "${APPELLATE_NORVALE_INJUNCTION_ARCHIVE_SHA256}"
    "${APPELLATE_NORVALE_INJUNCTION_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_ellison_immunity_archive}"
    "${APPELLATE_ELLISON_IMMUNITY_ARCHIVE_SHA256}"
    "${APPELLATE_ELLISON_IMMUNITY_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_blueember_jmol_archive}"
    "${APPELLATE_BLUEEMBER_JMOL_ARCHIVE_SHA256}"
    "${APPELLATE_BLUEEMBER_JMOL_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_opengrid_foia_archive}"
    "${APPELLATE_OPENGRID_FOIA_ARCHIVE_SHA256}"
    "${APPELLATE_OPENGRID_FOIA_ARCHIVE_SIZE}"
)
_appellate_assert_release_archive(
    "${_appellate_serrano_waiver_archive}"
    "${APPELLATE_SERRANO_WAIVER_ARCHIVE_SHA256}"
    "${APPELLATE_SERRANO_WAIVER_ARCHIVE_SIZE}"
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
            TIMEOUT 600
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
            TIMEOUT 900
    )
endif()

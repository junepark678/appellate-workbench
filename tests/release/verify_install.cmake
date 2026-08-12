if(NOT DEFINED APPELLATE_INSTALL_LIBDIR OR
   (NOT DEFINED APPELLATE_BUILD_DIR AND NOT DEFINED APPELLATE_BUNDLE_PREFIX))
    message(
        FATAL_ERROR
        "APPELLATE_INSTALL_LIBDIR and either APPELLATE_BUILD_DIR or "
        "APPELLATE_BUNDLE_PREFIX are required"
    )
endif()

find_program(_appellate_ldd NAMES ldd REQUIRED)
find_program(_appellate_readelf NAMES readelf REQUIRED)
find_program(_appellate_unshare NAMES unshare REQUIRED)

function(_appellate_assert_dependency_closure binary prefix)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "${_appellate_ldd}" "${binary}"
        RESULT_VARIABLE _ldd_result
        OUTPUT_VARIABLE _ldd_output
        ERROR_VARIABLE _ldd_error
    )
    if(NOT _ldd_result EQUAL 0 OR _ldd_output MATCHES "=>[ \t]+not found")
        message(FATAL_ERROR "Unresolved dependency for ${binary}:\n${_ldd_output}\n${_ldd_error}")
    endif()

    string(REGEX MATCHALL "=>[ \t]+/[^ \t\r\n]+" _resolved_entries "${_ldd_output}")
    foreach(_entry IN LISTS _resolved_entries)
        string(REGEX REPLACE "^=>[ \t]+" "" _resolved "${_entry}")
        string(FIND "${_resolved}" "${prefix}/" _inside_prefix)
        if(_inside_prefix EQUAL 0)
            continue()
        endif()
        get_filename_component(_soname "${_resolved}" NAME)
        if(_soname MATCHES "^lib(c|m|dl|pthread|resolv|rt|util|anl|BrokenLocale)[.]so([.]|$)")
            continue()
        endif()
        message(FATAL_ERROR "${binary} resolves ${_soname} outside the bundle: ${_resolved}")
    endforeach()
endfunction()

function(_appellate_assert_glibc_floor prefix library_root declared_floor)
    if(NOT declared_floor MATCHES "^[0-9]+[.][0-9]+$")
        message(FATAL_ERROR "The declared glibc floor is not a canonical version")
    endif()
    file(GLOB_RECURSE _elf_candidates
        LIST_DIRECTORIES FALSE
        "${prefix}/bin/*"
        "${library_root}/*.so*"
    )
    foreach(_candidate IN LISTS _elf_candidates)
        if(IS_SYMLINK "${_candidate}")
            continue()
        endif()
        execute_process(
            COMMAND "${_appellate_readelf}" --version-info "${_candidate}"
            RESULT_VARIABLE _readelf_result
            OUTPUT_VARIABLE _version_info
            ERROR_QUIET
        )
        if(NOT _readelf_result EQUAL 0)
            continue()
        endif()
        string(REGEX MATCHALL "GLIBC_[0-9]+[.][0-9]+([.][0-9]+)?" _glibc_versions
               "${_version_info}")
        foreach(_glibc_symbol IN LISTS _glibc_versions)
            string(REGEX REPLACE "^GLIBC_" "" _required_version "${_glibc_symbol}")
            if(_required_version VERSION_GREATER declared_floor)
                message(
                    FATAL_ERROR
                    "${_candidate} requires GLIBC_${_required_version}, above declared floor "
                    "${declared_floor}"
                )
            endif()
        endforeach()
    endforeach()
endfunction()

if(DEFINED APPELLATE_BUNDLE_PREFIX)
    get_filename_component(_prefix "${APPELLATE_BUNDLE_PREFIX}" ABSOLUTE)
    get_filename_component(_root "${_prefix}" DIRECTORY)
else()
    string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
    set(_root "${APPELLATE_BUILD_DIR}/install-smoke-${_suffix}")
    set(_prefix "${_root}/installed")
endif()
set(_relocated "${_root}/relocated")
set(_runtime "${_root}/runtime")
set(_catalog "${_root}/catalog")
file(MAKE_DIRECTORY "${_runtime}")
file(
    CHMOD "${_runtime}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
)

if(NOT DEFINED APPELLATE_BUNDLE_PREFIX)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${APPELLATE_BUILD_DIR}" --prefix "${_prefix}"
        RESULT_VARIABLE _install_result
        OUTPUT_VARIABLE _install_output
        ERROR_VARIABLE _install_error
    )
    if(NOT _install_result EQUAL 0)
        message(FATAL_ERROR "Install failed:\n${_install_output}\n${_install_error}")
    endif()
endif()

set(_desktop "${_prefix}/bin/Appellate Workbench")
set(_pack_cli "${_prefix}/bin/appellate-pack")
set(_packs_root "${_prefix}/share/appellate-workbench/packs")
set(_asterglen_v2 "${_packs_root}/us-ca4-rule54b-asterglen-0.2.0.awpack")
set(_federal "${_packs_root}/foundation-us-federal-2025.12.01.awpack")
set(_ca4 "${_packs_root}/foundation-us-ca4-2026.03.23.awpack")
set(_bench "${_packs_root}/foundation-us-ca4-fictional-bench-1.0.0.awpack")
set(_asterglen_v1 "${_packs_root}/us-ca4-rule54b-asterglen-0.1.0.awpack")
set(_compatibility "${_prefix}/share/appellate-workbench/compatibility.json")
foreach(_required IN ITEMS
        "${_desktop}"
        "${_pack_cli}"
        "${_asterglen_v2}"
        "${_federal}"
        "${_ca4}"
        "${_bench}"
        "${_asterglen_v1}"
        "${_compatibility}"
)
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "Installed bundle is missing ${_required}")
    endif()
endforeach()

set(_expected_v2_pack_id "us.ca4.rule54b.asterglen")
set(_expected_v2_version "0.2.0")
set(_expected_v2_revision "7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728")
set(_expected_v2_archive_sha "10739c149a3bf2617d8af6dd131caee7ea6639a9d97e26cdf2974fa176c82819")
set(_expected_v2_archive_size 3974147)
set(_expected_federal_pack_id "foundation.us-federal")
set(_expected_federal_version "2025.12.01")
set(_expected_federal_revision "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9")
set(_expected_federal_archive_sha "69736648f78376a6d85cde32148337edbf5af2a289de6070734c5454cc6b411b")
set(_expected_federal_archive_size 21002)
set(_expected_ca4_pack_id "foundation.us-ca4")
set(_expected_ca4_version "2026.03.23")
set(_expected_ca4_revision "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262")
set(_expected_ca4_archive_sha "5c9098d76012891ab2cb1f04c48bdcb3101c64253fdaab1608de789d0f5aa6ef")
set(_expected_ca4_archive_size 66512)
set(_expected_bench_pack_id "foundation.us-ca4-fictional-bench")
set(_expected_bench_version "1.0.0")
set(_expected_bench_revision "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d")
set(_expected_bench_archive_sha "e2758217f5ba9b987cc9e9920af65f762263f420e1698b12732d4f02b0121137")
set(_expected_bench_archive_size 14131)
set(_expected_v1_pack_id "us.ca4.rule54b.asterglen")
set(_expected_v1_version "0.1.0")
set(_expected_v1_revision "ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424")
set(_expected_v1_archive_sha "ce0ebffb92942e85e02658d11846af70ebb5fdc287f99a4c683a48f381e39227")
set(_expected_v1_archive_size 729511)

file(
    GLOB_RECURSE _installed_pack_archives
    LIST_DIRECTORIES FALSE
    RELATIVE "${_prefix}"
    "${_prefix}/*.awpack"
)
list(SORT _installed_pack_archives)
set(
    _expected_installed_pack_archives
    "share/appellate-workbench/packs/foundation-us-ca4-2026.03.23.awpack"
    "share/appellate-workbench/packs/foundation-us-ca4-fictional-bench-1.0.0.awpack"
    "share/appellate-workbench/packs/foundation-us-federal-2025.12.01.awpack"
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.1.0.awpack"
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.2.0.awpack"
)
list(SORT _expected_installed_pack_archives)
if(NOT _installed_pack_archives STREQUAL _expected_installed_pack_archives)
    message(
        FATAL_ERROR
        "Installed .awpack allowlist differs; generated starter archives must never ship: "
        "${_installed_pack_archives}"
    )
endif()

file(READ "${_compatibility}" _compatibility_json)
string(JSON _compatibility_schema GET "${_compatibility_json}" schema_version)
string(JSON _compatibility_os GET "${_compatibility_json}" platform operating_system)
string(JSON _compatibility_arch GET "${_compatibility_json}" platform architecture)
string(JSON _declared_glibc_floor GET "${_compatibility_json}" platform glibc_floor)
string(JSON _declared_pack_count LENGTH "${_compatibility_json}" bundled_packs)
if(NOT _compatibility_schema EQUAL 1 OR NOT _compatibility_os STREQUAL "linux" OR
   NOT _compatibility_arch STREQUAL "x86_64" OR NOT _declared_pack_count EQUAL 5)
    message(FATAL_ERROR "The installed compatibility manifest does not match the bundle")
endif()

function(_appellate_assert_bundled_pack index archive expected_id expected_version
         expected_revision expected_archive_sha expected_archive_size expected_path)
    string(JSON _declared_id GET "${_compatibility_json}" bundled_packs ${index} pack_id)
    string(JSON _declared_version GET "${_compatibility_json}" bundled_packs ${index} version)
    string(JSON _declared_revision GET "${_compatibility_json}" bundled_packs ${index} revision_sha256)
    string(JSON _declared_archive_sha GET "${_compatibility_json}" bundled_packs ${index} archive_sha256)
    string(JSON _declared_archive_size GET "${_compatibility_json}" bundled_packs ${index} archive_byte_size)
    string(JSON _declared_path GET "${_compatibility_json}" bundled_packs ${index} path)
    file(SHA256 "${archive}" _actual_archive_sha)
    file(SIZE "${archive}" _actual_archive_size)
    if(NOT _declared_id STREQUAL expected_id OR
       NOT _declared_version STREQUAL expected_version OR
       NOT _declared_revision STREQUAL expected_revision OR
       NOT _declared_archive_sha STREQUAL expected_archive_sha OR
       NOT _declared_archive_size EQUAL expected_archive_size OR
       NOT _declared_path STREQUAL expected_path OR
       NOT _actual_archive_sha STREQUAL expected_archive_sha OR
       NOT _actual_archive_size EQUAL expected_archive_size)
        message(FATAL_ERROR "Bundled pack ${index} identity, bytes, or path differs")
    endif()
endfunction()

_appellate_assert_bundled_pack(
    0 "${_asterglen_v2}"
    "${_expected_v2_pack_id}" "${_expected_v2_version}" "${_expected_v2_revision}"
    "${_expected_v2_archive_sha}" "${_expected_v2_archive_size}"
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.2.0.awpack"
)
_appellate_assert_bundled_pack(
    1 "${_federal}"
    "${_expected_federal_pack_id}" "${_expected_federal_version}"
    "${_expected_federal_revision}" "${_expected_federal_archive_sha}"
    "${_expected_federal_archive_size}"
    "share/appellate-workbench/packs/foundation-us-federal-2025.12.01.awpack"
)
_appellate_assert_bundled_pack(
    2 "${_ca4}"
    "${_expected_ca4_pack_id}" "${_expected_ca4_version}" "${_expected_ca4_revision}"
    "${_expected_ca4_archive_sha}" "${_expected_ca4_archive_size}"
    "share/appellate-workbench/packs/foundation-us-ca4-2026.03.23.awpack"
)
_appellate_assert_bundled_pack(
    3 "${_bench}"
    "${_expected_bench_pack_id}" "${_expected_bench_version}" "${_expected_bench_revision}"
    "${_expected_bench_archive_sha}" "${_expected_bench_archive_size}"
    "share/appellate-workbench/packs/foundation-us-ca4-fictional-bench-1.0.0.awpack"
)
_appellate_assert_bundled_pack(
    4 "${_asterglen_v1}"
    "${_expected_v1_pack_id}" "${_expected_v1_version}" "${_expected_v1_revision}"
    "${_expected_v1_archive_sha}" "${_expected_v1_archive_size}"
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.1.0.awpack"
)

execute_process(
    COMMAND "${_pack_cli}" validate "${_asterglen_v1}"
    RESULT_VARIABLE _v1_validate_result
    OUTPUT_VARIABLE _v1_validate_output
    ERROR_VARIABLE _v1_validate_error
)
if(NOT _v1_validate_result EQUAL 0)
    message(
        FATAL_ERROR
        "Immutable Asterglen v0.1 validation failed:\n"
        "${_v1_validate_output}\n${_v1_validate_error}"
    )
endif()
string(JSON _v1_validated_status GET "${_v1_validate_output}" status)
string(JSON _v1_validated_revision GET "${_v1_validate_output}" digest)
string(JSON _v1_validated_pack_id GET "${_v1_validate_output}" pack_id)
string(JSON _v1_validated_pack_version GET "${_v1_validate_output}" version)
if(NOT _v1_validated_status STREQUAL "ok" OR
   NOT _v1_validated_revision STREQUAL _expected_v1_revision OR
   NOT _v1_validated_pack_id STREQUAL _expected_v1_pack_id OR
   NOT _v1_validated_pack_version STREQUAL _expected_v1_version)
    message(FATAL_ERROR "Immutable Asterglen v0.1 identity differs")
endif()

function(_appellate_assert_revision_json json label expected_id expected_version expected_revision)
    string(JSON _actual_id GET "${json}" pack_id)
    string(JSON _actual_version GET "${json}" version)
    string(JSON _actual_revision GET "${json}" digest)
    if(NOT _actual_id STREQUAL expected_id OR
       NOT _actual_version STREQUAL expected_version OR
       NOT _actual_revision STREQUAL expected_revision)
        message(FATAL_ERROR "${label} revision identity differs")
    endif()
endfunction()

function(_appellate_install_exact_archive pack_cli archive catalog installed_at expected_id
         expected_version expected_revision expected_archive_sha)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "${pack_cli}" install "${archive}" "${catalog}"
            --installed-at "${installed_at}"
        RESULT_VARIABLE _install_result
        OUTPUT_VARIABLE _install_output
        ERROR_VARIABLE _install_error
        TIMEOUT 60
    )
    if(NOT _install_result EQUAL 0)
        message(
            FATAL_ERROR
            "Exact bundled pack installation failed for ${archive}:\n"
            "${_install_output}\n${_install_error}"
        )
    endif()
    string(JSON _install_status GET "${_install_output}" status)
    string(JSON _install_command GET "${_install_output}" command)
    string(JSON _install_archive_sha GET "${_install_output}" archive_sha256)
    string(JSON _installed_at GET "${_install_output}" installed_at_utc)
    if(NOT _install_status STREQUAL "ok" OR NOT _install_command STREQUAL "install" OR
       NOT _install_archive_sha STREQUAL expected_archive_sha OR
       NOT _installed_at STREQUAL installed_at)
        message(FATAL_ERROR "Exact bundled pack installation evidence differs for ${archive}")
    endif()
    _appellate_assert_revision_json(
        "${_install_output}" "Installed ${archive}"
        "${expected_id}" "${expected_version}" "${expected_revision}"
    )
endfunction()

function(_appellate_prepare_v2_catalog pack_cli federal ca4 bench asterglen_v2 catalog)
    if(EXISTS "${catalog}")
        message(FATAL_ERROR "Release smoke catalog unexpectedly already exists: ${catalog}")
    endif()
    _appellate_install_exact_archive(
        "${pack_cli}" "${federal}" "${catalog}" "2026-08-12T00:00:00Z"
        "${_expected_federal_pack_id}" "${_expected_federal_version}"
        "${_expected_federal_revision}" "${_expected_federal_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${ca4}" "${catalog}" "2026-08-12T00:00:01Z"
        "${_expected_ca4_pack_id}" "${_expected_ca4_version}"
        "${_expected_ca4_revision}" "${_expected_ca4_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${bench}" "${catalog}" "2026-08-12T00:00:02Z"
        "${_expected_bench_pack_id}" "${_expected_bench_version}"
        "${_expected_bench_revision}" "${_expected_bench_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${asterglen_v2}" "${catalog}" "2026-08-12T00:00:03Z"
        "${_expected_v2_pack_id}" "${_expected_v2_version}"
        "${_expected_v2_revision}" "${_expected_v2_archive_sha}"
    )

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "${pack_cli}" validate-resolved "${catalog}"
            "${_expected_v2_pack_id}" "${_expected_v2_version}" "${_expected_v2_revision}"
        RESULT_VARIABLE _resolved_result
        OUTPUT_VARIABLE _resolved_output
        ERROR_VARIABLE _resolved_error
        TIMEOUT 60
    )
    if(NOT _resolved_result EQUAL 0)
        message(
            FATAL_ERROR
            "Exact bundled Asterglen v0.2 closure validation failed:\n"
            "${_resolved_output}\n${_resolved_error}"
        )
    endif()
    string(JSON _resolved_status GET "${_resolved_output}" status)
    string(JSON _resolved_scope GET "${_resolved_output}" validation_scope)
    string(JSON _resolved_count GET "${_resolved_output}" resolved_revision_count)
    string(JSON _pin_count LENGTH "${_resolved_output}" revision_pins)
    if(NOT _resolved_status STREQUAL "ok" OR
       NOT _resolved_scope STREQUAL "catalog_resolved" OR
       NOT _resolved_count EQUAL 4 OR NOT _pin_count EQUAL 4)
        message(FATAL_ERROR "Asterglen v0.2 resolved-closure evidence is incomplete")
    endif()
    _appellate_assert_revision_json(
        "${_resolved_output}" "Resolved Asterglen v0.2 root"
        "${_expected_v2_pack_id}" "${_expected_v2_version}" "${_expected_v2_revision}"
    )

    set(_expected_pin_ids
        "${_expected_ca4_pack_id}"
        "${_expected_bench_pack_id}"
        "${_expected_federal_pack_id}"
        "${_expected_v2_pack_id}"
    )
    set(_expected_pin_versions
        "${_expected_ca4_version}"
        "${_expected_bench_version}"
        "${_expected_federal_version}"
        "${_expected_v2_version}"
    )
    set(_expected_pin_revisions
        "${_expected_ca4_revision}"
        "${_expected_bench_revision}"
        "${_expected_federal_revision}"
        "${_expected_v2_revision}"
    )
    foreach(_pin_index RANGE 0 3)
        string(JSON _pin GET "${_resolved_output}" revision_pins ${_pin_index})
        list(GET _expected_pin_ids ${_pin_index} _expected_pin_id)
        list(GET _expected_pin_versions ${_pin_index} _expected_pin_version)
        list(GET _expected_pin_revisions ${_pin_index} _expected_pin_revision)
        _appellate_assert_revision_json(
            "${_pin}" "Resolved Asterglen v0.2 pin ${_pin_index}"
            "${_expected_pin_id}" "${_expected_pin_version}" "${_expected_pin_revision}"
        )
    endforeach()
endfunction()

set(_expected_starter_pack_id "example.full.fictional")
set(_expected_starter_pack_version "2.0.0")
set(_expected_starter_pack_digest "023008f685d42634a271a626d5df1eb770ee5a6141a1b199eaa6d9945c4f15ce")
set(_expected_starter_archive_sha "7c11b2ae1d66fc2e722e77a965bfdd07b51108f3cf8c57986e3eb1bbcbae970c")
set(_expected_bundled_workflow_session "workflow.session.5f62a8255168bf9cabfe35af7e09ad86d368dcbd37683cc5206010f170e8db70")
set(_expected_bundled_workflow_digest "20272ca1834c9738a9d40d97b882d18c6115245856a3d9737096e732fc115fbb")
set(_expected_imported_workflow_session "workflow.session.16a9ac9c6f55f8a2390d031e64de8f2deb23f46e34e37a5a5aa87d5e9e3a0df2")
set(_expected_imported_workflow_digest "8f4f7ed230d52c9ca6dee8e8781ca5a587a5a1b59882c02c81f2a16ff3e0189b")
set(_expected_imported_workflow_rows "3fa96066cd77a349a9dce45041c94d3af2d5ee282dfec9aeb57e52473b1b61ed")
set(_expected_imported_asset_digest "b45710d93705fc230515730d26e638636005779238f785bfb51dd80006673d4d")
set(_expected_oral_session "oral.argument.session.00ab90968780fc8513550f093a3fb02e9c681b714aeb220ee23f781167ce8991")
set(_expected_oral_rows "fc30b2025e94ecf1df82116776b1fe16381e82884a742ac4841b283007d29d5a")
set(_expected_oral_transcript "de2bfe06f60197c7584077d05142915b9dfbff815771b1265f220c51661ddb31")

function(_appellate_export_embedded_starter pack_cli state_name output_variable)
    set(_state "${_root}/${state_name}")
    set(_state_runtime "${_state}/runtime")
    set(_starter_source "${_state}/starter-pack")
    set(_starter_archive "${_state}/starter-pack.awpack")
    file(MAKE_DIRECTORY "${_state_runtime}")
    file(
        CHMOD "${_state_runtime}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )
    execute_process(
        COMMAND
            "${_appellate_unshare}" --user --map-root-user --net
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "XDG_RUNTIME_DIR=${_state_runtime}"
            "XDG_CONFIG_HOME=${_state}/config"
            "XDG_DATA_HOME=${_state}/data"
            "XDG_CACHE_HOME=${_state}/cache"
            "${pack_cli}" template "${_starter_source}"
        RESULT_VARIABLE _template_result
        OUTPUT_VARIABLE _template_output
        ERROR_VARIABLE _template_error
        TIMEOUT 60
    )
    if(NOT _template_result EQUAL 0)
        message(
            FATAL_ERROR
            "Network-isolated shipped starter extraction failed:\n"
            "${_template_output}\n${_template_error}"
        )
    endif()
    execute_process(
        COMMAND
            "${_appellate_unshare}" --user --map-root-user --net
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "XDG_RUNTIME_DIR=${_state_runtime}"
            "XDG_CONFIG_HOME=${_state}/config"
            "XDG_DATA_HOME=${_state}/data"
            "XDG_CACHE_HOME=${_state}/cache"
            "${pack_cli}" export "${_starter_source}" "${_starter_archive}"
        RESULT_VARIABLE _export_result
        OUTPUT_VARIABLE _export_output
        ERROR_VARIABLE _export_error
        TIMEOUT 60
    )
    if(NOT _export_result EQUAL 0)
        message(
            FATAL_ERROR
            "Network-isolated shipped starter export failed:\n"
            "${_export_output}\n${_export_error}"
        )
    endif()
    execute_process(
        COMMAND
            "${_appellate_unshare}" --user --map-root-user --net
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "XDG_RUNTIME_DIR=${_state_runtime}"
            "${pack_cli}" validate "${_starter_archive}"
        RESULT_VARIABLE _starter_result
        OUTPUT_VARIABLE _starter_output
        ERROR_VARIABLE _starter_error
        TIMEOUT 60
    )
    if(NOT _starter_result EQUAL 0)
        message(
            FATAL_ERROR
            "Network-isolated shipped starter validation failed:\n"
            "${_starter_output}\n${_starter_error}"
        )
    endif()
    string(JSON _starter_status GET "${_starter_output}" status)
    string(JSON _starter_id GET "${_starter_output}" pack_id)
    string(JSON _starter_version GET "${_starter_output}" version)
    string(JSON _starter_digest GET "${_starter_output}" digest)
    file(SHA256 "${_starter_archive}" _starter_archive_sha)
    if(NOT _starter_status STREQUAL "ok" OR
       NOT _starter_id STREQUAL _expected_starter_pack_id OR
       NOT _starter_version STREQUAL _expected_starter_pack_version OR
       NOT _starter_digest STREQUAL _expected_starter_pack_digest OR
       NOT _starter_archive_sha STREQUAL _expected_starter_archive_sha)
        message(FATAL_ERROR "The shipped embedded starter identity or exported bytes changed")
    endif()
    set("${output_variable}" "${_starter_archive}" PARENT_SCOPE)
endfunction()

function(_appellate_run_offline_e2e desktop workflow_pack grounded_pack state_name)
    set(_state "${_root}/${state_name}")
    set(_state_runtime "${_state}/runtime")
    file(MAKE_DIRECTORY "${_state_runtime}")
    file(
        CHMOD "${_state_runtime}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )
    execute_process(
        COMMAND
            "${_appellate_unshare}" --user --map-root-user --net
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            --unset=DISPLAY
            --unset=WAYLAND_DISPLAY
            "QT_QPA_PLATFORM=offscreen"
            "XDG_RUNTIME_DIR=${_state_runtime}"
            "XDG_CONFIG_HOME=${_state}/config"
            "XDG_DATA_HOME=${_state}/data"
            "XDG_CACHE_HOME=${_state}/cache"
            "${desktop}" --offline-self-test
            --offline-e2e-pack "${grounded_pack}"
            --catalog "${_state}/catalog"
            "${workflow_pack}"
        RESULT_VARIABLE _offline_result
        OUTPUT_VARIABLE _offline_output
        ERROR_VARIABLE _offline_error
        TIMEOUT 120
    )
    if(NOT _offline_result EQUAL 0)
        message(
            FATAL_ERROR
            "Network-isolated installed offline E2E failed:\n"
            "${_offline_output}\n${_offline_error}"
        )
    endif()
    string(JSON _offline_status GET "${_offline_output}" status)
    string(JSON _offline_schema GET "${_offline_output}" schema_version)
    string(JSON _offline_field_count LENGTH "${_offline_output}")
    string(JSON _offline_network GET "${_offline_output}" network_isolation)
    string(JSON _bundled_revision_field_count LENGTH "${_offline_output}" bundled_workflow_pack)
    string(JSON _bundled_revision_id GET "${_offline_output}" bundled_workflow_pack pack_id)
    string(JSON _bundled_revision_version GET "${_offline_output}" bundled_workflow_pack version)
    string(JSON _bundled_revision_digest GET "${_offline_output}" bundled_workflow_pack sha256)
    string(JSON _imported_revision_field_count LENGTH "${_offline_output}" imported_grounded_pack)
    string(JSON _actual_imported_id GET "${_offline_output}" imported_grounded_pack pack_id)
    string(JSON _actual_imported_version GET "${_offline_output}" imported_grounded_pack version)
    string(JSON _actual_imported_digest GET "${_offline_output}" imported_grounded_pack sha256)
    string(JSON _bundled_workflow_session GET "${_offline_output}" bundled_workflow_session_id)
    string(JSON _bundled_workflow_digest GET "${_offline_output}" bundled_workflow_digest)
    string(JSON _bundled_workflow_created_at GET "${_offline_output}" bundled_workflow_created_at_utc)
    string(JSON _bundled_commands GET "${_offline_output}" bundled_workflow_commands)
    string(JSON _bundled_events GET "${_offline_output}" bundled_workflow_events)
    string(JSON _bundled_pins GET "${_offline_output}" bundled_workflow_pins)
    string(JSON _imported_commands GET "${_offline_output}" imported_workflow_commands)
    string(JSON _imported_events GET "${_offline_output}" imported_workflow_events)
    string(JSON _imported_docket GET "${_offline_output}" imported_workflow_docket_entries)
    string(JSON _imported_pins GET "${_offline_output}" imported_workflow_pins)
    string(JSON _imported_assets GET "${_offline_output}" imported_workflow_asset_references)
    string(JSON _workflow_created_at GET "${_offline_output}" imported_workflow_created_at_utc)
    string(JSON _workflow_session GET "${_offline_output}" imported_workflow_session_id)
    string(JSON _workflow_digest GET "${_offline_output}" imported_workflow_digest)
    string(JSON _workflow_rows GET "${_offline_output}" imported_workflow_rows_sha256)
    string(JSON _asset_digest GET "${_offline_output}" imported_asset_sha256)
    string(JSON _oral_session GET "${_offline_output}" oral_session_id)
    string(JSON _oral_entries GET "${_offline_output}" oral_journal_entries)
    string(JSON _oral_created_at GET "${_offline_output}" oral_created_at_utc)
    string(JSON _oral_pins GET "${_offline_output}" oral_pins)
    string(JSON _oral_rows GET "${_offline_output}" oral_rows_sha256)
    string(JSON _oral_transcript GET "${_offline_output}" oral_transcript_sha256)
    if(NOT _offline_schema EQUAL 2 OR NOT _offline_field_count EQUAL 27 OR
       NOT _offline_status STREQUAL "ok" OR
       NOT _offline_network STREQUAL "required-from-caller" OR
       NOT _bundled_revision_field_count EQUAL 3 OR
       NOT _bundled_revision_id STREQUAL _expected_v1_pack_id OR
       NOT _bundled_revision_version STREQUAL _expected_v1_version OR
       NOT _bundled_revision_digest STREQUAL _expected_v1_revision OR
       NOT _imported_revision_field_count EQUAL 3 OR
       NOT _actual_imported_id STREQUAL _expected_starter_pack_id OR
       NOT _actual_imported_version STREQUAL _expected_starter_pack_version OR
       NOT _actual_imported_digest STREQUAL _expected_starter_pack_digest OR
       NOT _bundled_workflow_session STREQUAL _expected_bundled_workflow_session OR
       NOT _bundled_workflow_digest STREQUAL _expected_bundled_workflow_digest OR
       NOT _bundled_workflow_created_at STREQUAL "2026-08-11T10:00:00Z" OR
       NOT _bundled_commands EQUAL 1 OR NOT _bundled_events EQUAL 1 OR
       NOT _bundled_pins EQUAL 1 OR NOT _imported_commands EQUAL 1 OR
       NOT _imported_events EQUAL 2 OR NOT _imported_docket EQUAL 2 OR
       NOT _imported_pins EQUAL 1 OR NOT _imported_assets EQUAL 1 OR
       NOT _workflow_created_at STREQUAL "2026-08-11T10:00:00Z" OR
       NOT _workflow_session STREQUAL _expected_imported_workflow_session OR
       NOT _workflow_digest STREQUAL _expected_imported_workflow_digest OR
       NOT _workflow_rows STREQUAL _expected_imported_workflow_rows OR
       NOT _asset_digest STREQUAL _expected_imported_asset_digest OR
       NOT _oral_session STREQUAL _expected_oral_session OR
       NOT _oral_entries EQUAL 2 OR NOT _oral_pins EQUAL 1 OR
       NOT _oral_created_at STREQUAL "2026-08-11T10:00:00Z" OR
       NOT _oral_rows STREQUAL _expected_oral_rows OR
       NOT _oral_transcript STREQUAL _expected_oral_transcript)
        message(FATAL_ERROR "Installed offline E2E evidence is incomplete or inconsistent")
    endif()
endfunction()

_appellate_prepare_v2_catalog(
    "${_pack_cli}"
    "${_federal}"
    "${_ca4}"
    "${_bench}"
    "${_asterglen_v2}"
    "${_catalog}"
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=LD_LIBRARY_PATH
        --unset=LD_PRELOAD
        --unset=QT_PLUGIN_PATH
        --unset=QT_QPA_PLATFORM_PLUGIN_PATH
        "QT_QPA_PLATFORM=offscreen"
        "XDG_RUNTIME_DIR=${_runtime}"
        "XDG_CONFIG_HOME=${_root}/config"
        "XDG_DATA_HOME=${_root}/data"
        "XDG_CACHE_HOME=${_root}/cache"
        "${_desktop}" --smoke-test --catalog "${_catalog}" "${_asterglen_v2}"
    RESULT_VARIABLE _desktop_result
    OUTPUT_VARIABLE _desktop_output
    ERROR_VARIABLE _desktop_error
)
if(NOT _desktop_result EQUAL 0)
    message(FATAL_ERROR "Installed desktop smoke failed:\n${_desktop_output}\n${_desktop_error}")
endif()

_appellate_export_embedded_starter(
    "${_pack_cli}"
    "installed-starter-export"
    _installed_starter_archive
)
_appellate_run_offline_e2e(
    "${_desktop}"
    "${_asterglen_v1}"
    "${_installed_starter_archive}"
    "installed-offline-e2e"
)

file(RENAME "${_prefix}" "${_relocated}")
set(_relocated_desktop "${_relocated}/bin/Appellate Workbench")
set(_relocated_pack_cli "${_relocated}/bin/appellate-pack")
set(_relocated_renderer "${_relocated}/bin/appellate-render")
set(_relocated_library_root "${_relocated}/${APPELLATE_INSTALL_LIBDIR}")
set(_relocated_packs_root "${_relocated}/share/appellate-workbench/packs")
set(_relocated_asterglen_v2
    "${_relocated_packs_root}/us-ca4-rule54b-asterglen-0.2.0.awpack")
set(_relocated_federal
    "${_relocated_packs_root}/foundation-us-federal-2025.12.01.awpack")
set(_relocated_ca4
    "${_relocated_packs_root}/foundation-us-ca4-2026.03.23.awpack")
set(_relocated_bench
    "${_relocated_packs_root}/foundation-us-ca4-fictional-bench-1.0.0.awpack")
set(_relocated_asterglen_v1
    "${_relocated_packs_root}/us-ca4-rule54b-asterglen-0.1.0.awpack")
foreach(_binary IN ITEMS
        "${_relocated_desktop}"
        "${_relocated_pack_cli}"
        "${_relocated_renderer}"
        "${_relocated_library_root}/qt6/plugins/platforms/libqoffscreen.so"
        "${_relocated_library_root}/qt6/plugins/platforms/libqxcb.so"
        "${_relocated_library_root}/qt6/plugins/sqldrivers/libqsqlite.so"
)
    _appellate_assert_dependency_closure("${_binary}" "${_relocated}")
endforeach()

file(
    GLOB_RECURSE _installed_plugins
    LIST_DIRECTORIES FALSE
    RELATIVE "${_relocated_library_root}/qt6/plugins"
    "${_relocated_library_root}/qt6/plugins/*.so"
)
list(SORT _installed_plugins)
set(
    _expected_plugins
    "platforms/libqoffscreen.so"
    "platforms/libqxcb.so"
    "sqldrivers/libqsqlite.so"
)
if(NOT _installed_plugins STREQUAL _expected_plugins)
    message(FATAL_ERROR "Unexpected deployed plugin set: ${_installed_plugins}")
endif()
_appellate_assert_glibc_floor(
    "${_relocated}"
    "${_relocated_library_root}"
    "${_declared_glibc_floor}"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=LD_LIBRARY_PATH
        --unset=LD_PRELOAD
        --unset=QT_PLUGIN_PATH
        --unset=QT_QPA_PLATFORM_PLUGIN_PATH
        "${_relocated_pack_cli}" validate "${_relocated_asterglen_v1}"
    RESULT_VARIABLE _relocated_validate_result
    OUTPUT_VARIABLE _relocated_validate_output
    ERROR_VARIABLE _relocated_validate_error
)
if(NOT _relocated_validate_result EQUAL 0 OR
   NOT _relocated_validate_output MATCHES "\"status\":\"ok\"")
    message(
        FATAL_ERROR
        "Relocated pack CLI validation failed:\n"
        "${_relocated_validate_output}\n${_relocated_validate_error}"
    )
endif()
_appellate_assert_revision_json(
    "${_relocated_validate_output}" "Relocated immutable Asterglen v0.1"
    "${_expected_v1_pack_id}" "${_expected_v1_version}" "${_expected_v1_revision}"
)

_appellate_prepare_v2_catalog(
    "${_relocated_pack_cli}"
    "${_relocated_federal}"
    "${_relocated_ca4}"
    "${_relocated_bench}"
    "${_relocated_asterglen_v2}"
    "${_root}/relocated-catalog"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=LD_LIBRARY_PATH
        --unset=LD_PRELOAD
        --unset=QT_PLUGIN_PATH
        --unset=QT_QPA_PLATFORM_PLUGIN_PATH
        "QT_QPA_PLATFORM=offscreen"
        "QT_DEBUG_PLUGINS=1"
        "XDG_RUNTIME_DIR=${_runtime}"
        "XDG_CONFIG_HOME=${_root}/relocated-config"
        "XDG_DATA_HOME=${_root}/relocated-data"
        "XDG_CACHE_HOME=${_root}/relocated-cache"
        "${_relocated_desktop}" --smoke-test
        --catalog "${_root}/relocated-catalog"
        "${_relocated_asterglen_v2}"
    RESULT_VARIABLE _relocated_result
    OUTPUT_VARIABLE _relocated_output
    ERROR_VARIABLE _relocated_error
)
if(NOT _relocated_result EQUAL 0)
    message(FATAL_ERROR "Relocated desktop smoke failed:\n${_relocated_output}\n${_relocated_error}")
endif()
_appellate_export_embedded_starter(
    "${_relocated_pack_cli}"
    "relocated-starter-export"
    _relocated_starter_archive
)
_appellate_run_offline_e2e(
    "${_relocated_desktop}"
    "${_relocated_asterglen_v1}"
    "${_relocated_starter_archive}"
    "relocated-offline-e2e"
)
string(FIND
    "${_relocated_error}"
    "${_relocated_library_root}/qt6/plugins/platforms/libqoffscreen.so"
    _offscreen_plugin_index
)
string(
    REGEX MATCHALL
    "/[^ \t\r\n\"]*qt6/plugins[^ \t\r\n\"]*"
    _reported_plugin_paths
    "${_relocated_error}"
)
foreach(_reported_plugin_path IN LISTS _reported_plugin_paths)
    string(FIND "${_reported_plugin_path}" "${_relocated}/" _bundled_plugin_index)
    if(NOT _bundled_plugin_index EQUAL 0)
        message(FATAL_ERROR "Relocated desktop inspected an external Qt plugin path: ${_reported_plugin_path}")
    endif()
endforeach()
if(_offscreen_plugin_index LESS 0)
    message(FATAL_ERROR "Relocated desktop did not load only the bundled offscreen plugin")
endif()

file(REMOVE_RECURSE "${_root}")
if(EXISTS "${_root}")
    message(FATAL_ERROR "Temporary install tree could not be removed")
endif()

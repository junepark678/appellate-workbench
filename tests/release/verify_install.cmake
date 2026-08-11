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
set(_gold "${_prefix}/share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.1.0.awpack")
set(_compatibility "${_prefix}/share/appellate-workbench/compatibility.json")
foreach(_required IN ITEMS "${_desktop}" "${_pack_cli}" "${_gold}" "${_compatibility}")
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "Installed bundle is missing ${_required}")
    endif()
endforeach()

file(
    GLOB_RECURSE _installed_pack_archives
    LIST_DIRECTORIES FALSE
    RELATIVE "${_prefix}"
    "${_prefix}/*.awpack"
)
list(SORT _installed_pack_archives)
set(
    _expected_installed_pack_archives
    "share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.1.0.awpack"
)
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
string(JSON _declared_archive_sha GET "${_compatibility_json}" bundled_packs 0 archive_sha256)
string(JSON _declared_revision_sha GET "${_compatibility_json}" bundled_packs 0 revision_sha256)
string(JSON _declared_pack_id GET "${_compatibility_json}" bundled_packs 0 pack_id)
string(JSON _declared_pack_version GET "${_compatibility_json}" bundled_packs 0 version)
file(SHA256 "${_gold}" _actual_archive_sha)
if(NOT _compatibility_schema EQUAL 1 OR NOT _compatibility_os STREQUAL "linux" OR
   NOT _compatibility_arch STREQUAL "x86_64" OR
   NOT _declared_archive_sha STREQUAL _actual_archive_sha)
    message(FATAL_ERROR "The installed compatibility manifest does not match the bundle")
endif()

execute_process(
    COMMAND "${_pack_cli}" validate "${_gold}"
    RESULT_VARIABLE _validate_result
    OUTPUT_VARIABLE _validate_output
    ERROR_VARIABLE _validate_error
)
if(NOT _validate_result EQUAL 0)
    message(FATAL_ERROR "Bundled pack validation failed:\n${_validate_output}\n${_validate_error}")
endif()
string(JSON _validated_status GET "${_validate_output}" status)
string(JSON _validated_revision GET "${_validate_output}" digest)
string(JSON _validated_pack_id GET "${_validate_output}" pack_id)
string(JSON _validated_pack_version GET "${_validate_output}" version)
if(NOT _validated_status STREQUAL "ok" OR
   NOT _validated_revision STREQUAL _declared_revision_sha OR
   NOT _validated_pack_id STREQUAL _declared_pack_id OR
   NOT _validated_pack_version STREQUAL _declared_pack_version)
    message(FATAL_ERROR "Bundled pack identity differs from the compatibility manifest")
endif()

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
       NOT _bundled_revision_id STREQUAL _declared_pack_id OR
       NOT _bundled_revision_version STREQUAL _declared_pack_version OR
       NOT _bundled_revision_digest STREQUAL _declared_revision_sha OR
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
        "${_desktop}" --smoke-test --catalog "${_catalog}" "${_gold}"
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
    "${_gold}"
    "${_installed_starter_archive}"
    "installed-offline-e2e"
)

file(RENAME "${_prefix}" "${_relocated}")
set(_relocated_desktop "${_relocated}/bin/Appellate Workbench")
set(_relocated_pack_cli "${_relocated}/bin/appellate-pack")
set(_relocated_renderer "${_relocated}/bin/appellate-render")
set(_relocated_library_root "${_relocated}/${APPELLATE_INSTALL_LIBDIR}")
set(_relocated_gold
    "${_relocated}/share/appellate-workbench/packs/us-ca4-rule54b-asterglen-0.1.0.awpack"
)
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
        "${_relocated_pack_cli}" validate "${_relocated_gold}"
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
        "${_relocated_gold}"
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
    "${_relocated_gold}"
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

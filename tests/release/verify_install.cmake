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

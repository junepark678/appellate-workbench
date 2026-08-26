if(NOT DEFINED APPELLATE_INSTALL_LIBDIR OR NOT DEFINED APPELLATE_SOURCE_DIR OR
   (NOT DEFINED APPELLATE_BUILD_DIR AND NOT DEFINED APPELLATE_BUNDLE_PREFIX))
    message(
        FATAL_ERROR
        "APPELLATE_INSTALL_LIBDIR, APPELLATE_SOURCE_DIR, and either APPELLATE_BUILD_DIR or "
        "APPELLATE_BUNDLE_PREFIX are required"
    )
endif()

if(POLICY CMP0009)
    cmake_policy(SET CMP0009 NEW)
endif()

get_filename_component(_appellate_source_root "${APPELLATE_SOURCE_DIR}" ABSOLUTE)
set(
    _appellate_expected_documentation_files
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
list(SORT _appellate_expected_documentation_files)
set(
    _appellate_expected_documentation_directories
    "docs"
    "docs/adr"
    "docs/content"
    "docs/spec"
)
set(
    _appellate_expected_documentation_entries
    ${_appellate_expected_documentation_files}
    ${_appellate_expected_documentation_directories}
)
list(SORT _appellate_expected_documentation_entries)

function(_appellate_assert_documentation_tree prefix label)
    set(_documentation_root "${prefix}/share/doc/appellate-workbench")
    foreach(_directory IN ITEMS
            "${prefix}"
            "${prefix}/share"
            "${prefix}/share/doc"
            "${_documentation_root}"
            "${_documentation_root}/docs"
            "${_documentation_root}/docs/adr"
            "${_documentation_root}/docs/content"
            "${_documentation_root}/docs/spec"
    )
        if(IS_SYMLINK "${_directory}" OR NOT IS_DIRECTORY "${_directory}")
            message(FATAL_ERROR "${label} documentation component is not an ordinary directory: ${_directory}")
        endif()
    endforeach()

    file(
        GLOB_RECURSE _actual_documentation_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${_documentation_root}"
        "${_documentation_root}/*"
    )
    list(SORT _actual_documentation_entries)
    if(NOT _actual_documentation_entries STREQUAL _appellate_expected_documentation_entries)
        message(
            FATAL_ERROR
            "${label} documentation tree differs from the exact nonflattened allowlist: "
            "${_actual_documentation_entries}"
        )
    endif()

    set(_observed_local_link_edges)
    foreach(_relative_path IN LISTS _appellate_expected_documentation_files)
        set(_installed_file "${_documentation_root}/${_relative_path}")
        set(_source_file "${_appellate_source_root}/${_relative_path}")
        if(NOT EXISTS "${_source_file}" OR IS_SYMLINK "${_installed_file}" OR
           NOT EXISTS "${_installed_file}" OR IS_DIRECTORY "${_installed_file}")
            message(FATAL_ERROR "${label} documentation file is missing, linked, or nonregular: ${_relative_path}")
        endif()
        execute_process(
            COMMAND
                "${CMAKE_COMMAND}" -E env "LC_ALL=C"
                "${_appellate_stat}" --format=%F -- "${_installed_file}"
            RESULT_VARIABLE _stat_result
            OUTPUT_VARIABLE _file_type
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT _stat_result EQUAL 0 OR NOT _file_type STREQUAL "regular file")
            message(FATAL_ERROR "${label} documentation member is not a regular file: ${_relative_path}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E compare_files "${_installed_file}" "${_source_file}"
            RESULT_VARIABLE _compare_result
        )
        if(NOT _compare_result EQUAL 0)
            message(FATAL_ERROR "${label} documentation bytes differ from source: ${_relative_path}")
        endif()

        file(READ "${_installed_file}" _markdown)
        string(REGEX REPLACE "[ \t\r\n]+" " " _normalized_markdown "${_markdown}")
        if(_relative_path STREQUAL "docs/INDEPENDENT_REVIEW.md")
            set(
                _required_independent_review_literals
                "appellate-pack prepare-independent-review <catalog> <subject-pack-id> <subject-version> <subject-digest> <case-id> <new-handoff-directory>"
                "appellate-pack finalize-independent-review <handoff-directory> <completed-declaration-json> <catalog> <new-pack-directory>"
                "Its `reviewed_on` date cannot be later than the one current UTC calendar date captured by preparation."
                "Archive, install, and resolve-check the finalized directory as separate operations. The prepare/finalize commands perform none of these steps:"
                "They are not a cryptographic signature or proof of reviewer identity."
                "Neither a prepared handoff nor a finalized review directory is, by itself, evidence that a case is gold, release-ready, or MVP-complete."
                "there is no automatic catalog repair or persistent-lock eviction."
                "It provides neither a cryptographic declaration signature nor a persisted provenance capability."
            )
            foreach(_required_literal IN LISTS _required_independent_review_literals)
                string(FIND "${_normalized_markdown}" "${_required_literal}" _literal_index)
                if(_literal_index EQUAL -1)
                    message(
                        FATAL_ERROR
                        "${label} independent-review documentation omits required text: "
                        "${_required_literal}"
                    )
                endif()
            endforeach()
            set(_publication_step_previous -1)
            foreach(_publication_step IN ITEMS
                    "appellate-pack export-deferred <new-pack-directory> <new-awpack>"
                    "appellate-pack install <new-awpack> <verification-catalog>"
                    "appellate-pack validate-resolved <verification-catalog> <review-pack-id> <review-pack-version> <review-pack-digest>"
            )
                string(FIND "${_normalized_markdown}" "${_publication_step}" _publication_step_index)
                if(_publication_step_index LESS_EQUAL _publication_step_previous)
                    message(
                        FATAL_ERROR
                        "${label} independent-review publication procedure is missing or out of order: "
                        "${_publication_step}"
                    )
                endif()
                set(_publication_step_previous ${_publication_step_index})
            endforeach()
        elseif(_relative_path STREQUAL "docs/spec/PACKS.md")
            set(
                _required_packs_literals
                "appellate-pack prepare-independent-review <catalog> <subject-pack-id> <subject-version> <subject-digest> <case-id> <new-handoff-directory>"
                "appellate-pack finalize-independent-review <handoff-directory> <completed-declaration-json> <catalog> <new-pack-directory>"
                "The source `reviewed_on` cannot be later than the one current UTC calendar date captured by preparation."
                "Reviewer identity and qualification are attributable metadata, not a cryptographic signature or identity proof."
                "Neither command overwrites or repairs an existing path, installs or archives a pack, changes the source root or catalog, performs network access, or establishes gold, release-ready, or MVP-complete status."
                "A finalized directory is separately archived with `export-deferred`, installed with `install` after its exact subject closure, and checked with `validate-resolved`."
                "The application performs no automatic catalog repair and no automatic persistent-lock eviction."
            )
            foreach(_required_literal IN LISTS _required_packs_literals)
                string(FIND "${_normalized_markdown}" "${_required_literal}" _literal_index)
                if(_literal_index EQUAL -1)
                    message(
                        FATAL_ERROR
                        "${label} pack documentation omits required independent-review text: "
                        "${_required_literal}"
                    )
                endif()
            endforeach()
        endif()
        string(REGEX MATCHALL "\\[[^]]*\\]\\([^)]+\\)" _markdown_links "${_markdown}")
        get_filename_component(_document_directory "${_installed_file}" DIRECTORY)
        foreach(_markdown_link IN LISTS _markdown_links)
            string(REGEX REPLACE "^.*\\]\\(([^)]+)\\)$" "\\1" _link_target "${_markdown_link}")
            if(_link_target MATCHES "^<([^>]+)>$")
                set(_link_target "${CMAKE_MATCH_1}")
            endif()
            if(_link_target MATCHES "^[A-Za-z][A-Za-z0-9+.-]*:" OR
               _link_target MATCHES "^//" OR _link_target MATCHES "^#")
                continue()
            endif()
            string(REGEX REPLACE "[#?].*$" "" _link_path "${_link_target}")
            if(_link_path STREQUAL "" OR _link_path MATCHES "^/")
                message(FATAL_ERROR "${label} documentation has an invalid local link: ${_relative_path} -> ${_link_target}")
            endif()
            set(_resolved_target "${_link_path}")
            cmake_path(
                ABSOLUTE_PATH _resolved_target
                BASE_DIRECTORY "${_document_directory}"
                NORMALIZE
            )
            cmake_path(
                RELATIVE_PATH _resolved_target
                BASE_DIRECTORY "${_documentation_root}"
                OUTPUT_VARIABLE _relative_target
            )
            if(_relative_target MATCHES "^([.][.](/|$))" OR
               NOT _relative_target IN_LIST _appellate_expected_documentation_files)
                message(
                    FATAL_ERROR
                    "${label} documentation link escapes or targets a nonallowlisted file: "
                    "${_relative_path} -> ${_link_target}"
                )
            endif()
            list(APPEND _observed_local_link_edges "${_relative_path}|${_relative_target}")
        endforeach()
    endforeach()
    foreach(_required_link_edge IN ITEMS
            "README.md|docs/INDEPENDENT_REVIEW.md"
            "README.md|docs/spec/PACKS.md"
            "docs/INDEPENDENT_REVIEW.md|docs/spec/PACKS.md"
            "docs/spec/PACKS.md|docs/INDEPENDENT_REVIEW.md"
            "docs/RELEASE.md|docs/INDEPENDENT_REVIEW.md"
    )
        if(NOT _required_link_edge IN_LIST _observed_local_link_edges)
            message(
                FATAL_ERROR
                "${label} documentation omits mandatory local link edge: ${_required_link_edge}"
            )
        endif()
    endforeach()
endfunction()

function(_appellate_assert_independent_review_wrong_arity pack_cli label)
    foreach(_command IN ITEMS prepare-independent-review finalize-independent-review)
        execute_process(
            COMMAND "${pack_cli}" "${_command}"
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr
        )
        if(NOT _result EQUAL 2 OR NOT _stdout STREQUAL "")
            message(
                FATAL_ERROR
                "${label} ${_command} wrong-arity gate did not return exit 2 with empty stdout: "
                "stdout=${_stdout}; stderr=${_stderr}"
            )
        endif()
        string(JSON _field_count LENGTH "${_stderr}")
        string(JSON _code_type TYPE "${_stderr}" code)
        string(JSON _code GET "${_stderr}" code)
        string(JSON _command_type TYPE "${_stderr}" command)
        string(JSON _reported_command GET "${_stderr}" command)
        string(JSON _message_type TYPE "${_stderr}" message)
        string(JSON _message GET "${_stderr}" message)
        string(JSON _schema_version_type TYPE "${_stderr}" schema_version)
        string(JSON _schema_version GET "${_stderr}" schema_version)
        string(JSON _status_type TYPE "${_stderr}" status)
        string(JSON _status GET "${_stderr}" status)
        string(REGEX MATCHALL "\n" _stderr_newlines "${_stderr}")
        list(LENGTH _stderr_newlines _newline_count)
        string(
            REGEX MATCH
            "^\\{\"code\":\"invalid_arguments\",\"command\":\"${_command}\",\"message\":\".*\",\"schema_version\":1,\"status\":\"error\"\\}\n$"
            _compact_envelope
            "${_stderr}"
        )
        if(NOT _field_count EQUAL 5 OR NOT _code_type STREQUAL "STRING" OR
           NOT _code STREQUAL "invalid_arguments" OR NOT _command_type STREQUAL "STRING" OR
           NOT _reported_command STREQUAL _command OR NOT _message_type STREQUAL "STRING" OR
           _message STREQUAL "" OR NOT _schema_version_type STREQUAL "NUMBER" OR
           NOT _schema_version EQUAL 1 OR NOT _status_type STREQUAL "STRING" OR
           NOT _status STREQUAL "error" OR NOT _newline_count EQUAL 1 OR
           _compact_envelope STREQUAL "" OR _stderr MATCHES "\r")
            message(FATAL_ERROR "${label} ${_command} wrong-arity JSON envelope differs: ${_stderr}")
        endif()
    endforeach()
endfunction()

find_program(_appellate_ldd NAMES ldd REQUIRED)
find_program(_appellate_readelf NAMES readelf REQUIRED)
find_program(_appellate_id NAMES id REQUIRED)
find_program(_appellate_stat NAMES stat REQUIRED)

execute_process(
    COMMAND "${_appellate_id}" -u
    RESULT_VARIABLE _euid_result
    OUTPUT_VARIABLE _appellate_effective_uid
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT _euid_result EQUAL 0 OR NOT _appellate_effective_uid MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Cannot determine the effective UID for release verification")
endif()

function(_appellate_assert_private_directory path label)
    if(IS_SYMLINK "${path}" OR NOT IS_DIRECTORY "${path}")
        message(FATAL_ERROR "${label} is not an ordinary directory: ${path}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env "LC_ALL=C"
            "${_appellate_stat}" "--format=%F|%u|%a" -- "${path}"
        RESULT_VARIABLE _stat_result
        OUTPUT_VARIABLE _stat_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(_expected "directory|${_appellate_effective_uid}|700")
    if(NOT _stat_result EQUAL 0 OR NOT _stat_output STREQUAL _expected)
        message(FATAL_ERROR "${label} does not have exact directory ownership/mode: ${_stat_output}")
    endif()
endfunction()

function(_appellate_assert_private_file path label)
    if(IS_SYMLINK "${path}" OR NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
        message(FATAL_ERROR "${label} is not an ordinary file: ${path}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env "LC_ALL=C"
            "${_appellate_stat}" "--format=%F|%u|%a|%h" -- "${path}"
        RESULT_VARIABLE _stat_result
        OUTPUT_VARIABLE _stat_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(_expected "regular file|${_appellate_effective_uid}|600|1")
    if(NOT _stat_result EQUAL 0 OR NOT _stat_output STREQUAL _expected)
        message(FATAL_ERROR "${label} does not have exact file ownership/mode/link count: ${_stat_output}")
    endif()
endfunction()

function(_appellate_capture_tree_fingerprint root label output_variable)
    if(IS_SYMLINK "${root}" OR NOT IS_DIRECTORY "${root}")
        message(FATAL_ERROR "${label} fingerprint root is not an ordinary directory: ${root}")
    endif()
    file(
        GLOB_RECURSE _entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${root}"
        "${root}/*"
    )
    list(SORT _entries)
    set(_fingerprint_material "")
    foreach(_entry IN LISTS _entries)
        set(_path "${root}/${_entry}")
        if(IS_SYMLINK "${_path}")
            message(FATAL_ERROR "${label} fingerprint encountered a symlink: ${_entry}")
        elseif(IS_DIRECTORY "${_path}")
            execute_process(
                COMMAND
                    "${CMAKE_COMMAND}" -E env "LC_ALL=C"
                    "${_appellate_stat}" "--format=%F|%u|%a" -- "${_path}"
                RESULT_VARIABLE _stat_result
                OUTPUT_VARIABLE _metadata
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(NOT _stat_result EQUAL 0 OR NOT _metadata MATCHES "^directory[|]")
                message(FATAL_ERROR "${label} cannot fingerprint directory: ${_entry}")
            endif()
            string(APPEND _fingerprint_material "D|${_entry}|${_metadata}\n")
        else()
            execute_process(
                COMMAND
                    "${CMAKE_COMMAND}" -E env "LC_ALL=C"
                    "${_appellate_stat}" "--format=%F|%u|%a|%h" -- "${_path}"
                RESULT_VARIABLE _stat_result
                OUTPUT_VARIABLE _metadata
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(NOT _stat_result EQUAL 0 OR NOT _metadata MATCHES "^regular file[|]")
                message(FATAL_ERROR "${label} cannot fingerprint regular file: ${_entry}")
            endif()
            file(SIZE "${_path}" _size)
            file(SHA256 "${_path}" _sha256)
            string(APPEND _fingerprint_material "F|${_entry}|${_metadata}|${_size}|${_sha256}\n")
        endif()
    endforeach()
    string(SHA256 _fingerprint "${_fingerprint_material}")
    set("${output_variable}" "${_fingerprint}" PARENT_SCOPE)
endfunction()

function(_appellate_assert_no_transient_residue root label)
    if(NOT IS_DIRECTORY "${root}")
        message(FATAL_ERROR "${label} residue root is missing: ${root}")
    endif()
    file(
        GLOB_RECURSE _entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${root}"
        "${root}/*"
    )
    foreach(_entry IN LISTS _entries)
        if(_entry MATCHES "(^|/)[.]install[.]lock($|[.]rmlock$)" OR
           _entry MATCHES "(^|/)[.]awpack-" OR _entry MATCHES "(^|/)[.]blob-" OR
           _entry MATCHES "(^|/)[.][^/]+[.]appellate-independent-review-")
            message(FATAL_ERROR "${label} retained transient residue: ${_entry}")
        endif()
    endforeach()
endfunction()

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
    if(NOT DEFINED APPELLATE_VERIFIER_ROOT)
        message(FATAL_ERROR "APPELLATE_VERIFIER_ROOT is required with APPELLATE_BUNDLE_PREFIX")
    endif()
    get_filename_component(_root "${APPELLATE_VERIFIER_ROOT}" ABSOLUTE)
    get_filename_component(_prefix_parent "${_prefix}" DIRECTORY)
    if(NOT _prefix_parent STREQUAL _root)
        message(FATAL_ERROR "The bundle prefix must be an immediate child of the verifier root")
    endif()
else()
    string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
    set(_root "${APPELLATE_BUILD_DIR}/install-smoke-${_suffix}")
    set(_prefix "${_root}/installed")
    if(EXISTS "${_root}")
        message(FATAL_ERROR "Random install verifier root unexpectedly already exists")
    endif()
    file(MAKE_DIRECTORY "${_root}")
    file(WRITE "${_root}/.appellate-install-verifier-root" "appellate-install-verifier-root\n")
endif()
if(_root STREQUAL "/" OR _root STREQUAL "" OR
   NOT EXISTS "${_root}/.appellate-install-verifier-root")
    message(FATAL_ERROR "The install verifier root is not sentinel-controlled")
endif()
file(READ "${_root}/.appellate-install-verifier-root" _verifier_root_sentinel)
if(NOT _verifier_root_sentinel STREQUAL "appellate-install-verifier-root\n")
    message(FATAL_ERROR "The install verifier root sentinel differs")
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
set(_cinder "${_packs_root}/us-ca4-m4-cinderlake-writ-1.2.0.awpack")
set(_arm_agency "${_packs_root}/us-ca4-m4-arm-agency-1.2.0.awpack")
set(_benton_retaliation "${_packs_root}/us-ca4-m4-benton-retaliation-1.2.0.awpack")
set(_norvale_injunction "${_packs_root}/us-ca4-m4-norvale-injunction-1.2.0.awpack")
set(_ellison_immunity "${_packs_root}/us-ca4-m4-ellison-immunity-1.2.0.awpack")
set(_blueember_jmol "${_packs_root}/us-ca4-m4-blueember-jmol-1.2.0.awpack")
set(_opengrid_foia "${_packs_root}/us-ca4-m4-opengrid-foia-1.2.0.awpack")
set(_serrano_waiver "${_packs_root}/us-ca4-m4-serrano-waiver-1.2.0.awpack")
set(_compatibility "${_prefix}/share/appellate-workbench/compatibility.json")
_appellate_assert_documentation_tree("${_prefix}" "Installed")
_appellate_assert_independent_review_wrong_arity("${_pack_cli}" "Installed")
foreach(_required IN ITEMS
        "${_desktop}"
        "${_pack_cli}"
        "${_asterglen_v2}"
        "${_federal}"
        "${_ca4}"
        "${_bench}"
        "${_asterglen_v1}"
        "${_cinder}"
        "${_arm_agency}"
        "${_benton_retaliation}"
        "${_norvale_injunction}"
        "${_ellison_immunity}"
        "${_blueember_jmol}"
        "${_opengrid_foia}"
        "${_serrano_waiver}"
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
set(_expected_cinder_pack_id "us.ca4.m4.cinderlake-writ")
set(_expected_cinder_version "1.2.0")
set(_expected_cinder_revision "020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c")
set(_expected_cinder_archive_sha "eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956")
set(_expected_cinder_archive_size 2519053)
set(_expected_arm_agency_pack_id "us.ca4.m4.arm-agency")
set(_expected_arm_agency_version "1.2.0")
set(_expected_arm_agency_revision "ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb")
set(_expected_arm_agency_archive_sha "a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2")
set(_expected_arm_agency_archive_size 3286508)
set(_expected_benton_retaliation_pack_id "us.ca4.m4.benton-retaliation")
set(_expected_benton_retaliation_version "1.2.0")
set(_expected_benton_retaliation_revision "59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28")
set(_expected_benton_retaliation_archive_sha "9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05")
set(_expected_benton_retaliation_archive_size 3408701)
set(_expected_norvale_injunction_pack_id "us.ca4.m4.norvale-injunction")
set(_expected_norvale_injunction_version "1.2.0")
set(_expected_norvale_injunction_revision "a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f")
set(_expected_norvale_injunction_archive_sha "a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19")
set(_expected_norvale_injunction_archive_size 4744009)
set(_expected_ellison_immunity_pack_id "us.ca4.m4.ellison-immunity")
set(_expected_ellison_immunity_version "1.2.0")
set(_expected_ellison_immunity_revision "c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0")
set(_expected_ellison_immunity_archive_sha "59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0")
set(_expected_ellison_immunity_archive_size 4230462)
set(_expected_blueember_jmol_pack_id "us.ca4.m4.blueember-jmol")
set(_expected_blueember_jmol_version "1.2.0")
set(_expected_blueember_jmol_revision "08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec")
set(_expected_blueember_jmol_archive_sha "c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2")
set(_expected_blueember_jmol_archive_size 5326158)
set(_expected_opengrid_foia_pack_id "us.ca4.m4.opengrid-foia")
set(_expected_opengrid_foia_version "1.2.0")
set(_expected_opengrid_foia_revision "9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5")
set(_expected_opengrid_foia_archive_sha "1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09")
set(_expected_opengrid_foia_archive_size 5244039)
set(_expected_serrano_waiver_pack_id "us.ca4.m4.serrano-waiver")
set(_expected_serrano_waiver_version "1.2.0")
set(_expected_serrano_waiver_revision "9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344")
set(_expected_serrano_waiver_archive_sha "d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243")
set(_expected_serrano_waiver_archive_size 3453568)

set(_additional_case_keys
    "cinderlake-writ"
    "arm-agency"
    "benton-retaliation"
    "norvale-injunction"
    "ellison-immunity"
    "blueember-jmol"
    "opengrid-foia"
    "serrano-waiver"
)
set(_additional_case_labels
    "Cinder Lake writ"
    "A.R.M. agency review"
    "Benton retaliation"
    "Norvale injunction"
    "Ellison immunity"
    "Blue Ember JMOL"
    "Open Grid FOIA"
    "Serrano waiver"
)
set(_additional_case_archive_names
    "us-ca4-m4-cinderlake-writ-1.2.0.awpack"
    "us-ca4-m4-arm-agency-1.2.0.awpack"
    "us-ca4-m4-benton-retaliation-1.2.0.awpack"
    "us-ca4-m4-norvale-injunction-1.2.0.awpack"
    "us-ca4-m4-ellison-immunity-1.2.0.awpack"
    "us-ca4-m4-blueember-jmol-1.2.0.awpack"
    "us-ca4-m4-opengrid-foia-1.2.0.awpack"
    "us-ca4-m4-serrano-waiver-1.2.0.awpack"
)
set(_additional_case_pack_ids
    "us.ca4.m4.cinderlake-writ"
    "us.ca4.m4.arm-agency"
    "us.ca4.m4.benton-retaliation"
    "us.ca4.m4.norvale-injunction"
    "us.ca4.m4.ellison-immunity"
    "us.ca4.m4.blueember-jmol"
    "us.ca4.m4.opengrid-foia"
    "us.ca4.m4.serrano-waiver"
)
set(_additional_case_revisions
    "020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c"
    "ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb"
    "59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28"
    "a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f"
    "c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0"
    "08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec"
    "9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5"
    "9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344"
)
set(_additional_case_archive_shas
    "eeefbbbe84cf4addbf91a68447281217226c6a08c7e0e3e1294947d5e5dc8956"
    "a150903c6c3332d8de582a8ef46e7fd1dd17cee0ac52c93c0ebaf51313cf54d2"
    "9515bdde1e3405e6e82488abd73314a31c33a2062f9e34b4cecdaaff8b634a05"
    "a4b993aa3cc6582d1d0f6ca9a7203109378f4f1c1b2e6ce32efbfe82b6a48e19"
    "59f32f521644bac61865cf1e59444fc98dbb9007461a1709272ffe261cbad1d0"
    "c6332ae33e351ccb27ed17b5576b147a47f9f5f0b44583365212b1781a288ed2"
    "1efa067767f3c729bbd67c40b3faa239673025f421133bddf32ec6b090231b09"
    "d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243"
)
set(_additional_case_ids
    "ca4m4.case.cinderlake-writ"
    "ca4m4.case.arm-agency"
    "ca4m4.case.benton-retaliation"
    "ca4m4.case.norvale-injunction"
    "ca4m4.case.ellison-immunity"
    "ca4m4.case.blueember-jmol"
    "ca4m4.case.opengrid-foia"
    "ca4m4.case.serrano-waiver"
)
foreach(_metadata_list IN ITEMS
        _additional_case_keys
        _additional_case_labels
        _additional_case_archive_names
        _additional_case_pack_ids
        _additional_case_revisions
        _additional_case_archive_shas
        _additional_case_ids
)
    list(LENGTH ${_metadata_list} _metadata_count)
    if(NOT _metadata_count EQUAL 8)
        message(FATAL_ERROR "Additional bundled-case metadata must contain exactly eight roots")
    endif()
endforeach()
set(_expected_executed_root_tokens
    "us.ca4.rule54b.asterglen|0.2.0|7e77bc0fbe02dc9e108681df73852859d6d0f577acdcb65fcfb7678eac78b728|ca4r54b.case.asterglen"
    "us.ca4.m4.cinderlake-writ|1.2.0|020517571a6c15f90765e12b94ab53d8598be3bc3081d47caecdf5950bacd05c|ca4m4.case.cinderlake-writ"
    "us.ca4.m4.arm-agency|1.2.0|ae33933c7cf18f77e662eb302d563afd860e8e900bac8debb081b81b35404edb|ca4m4.case.arm-agency"
    "us.ca4.m4.benton-retaliation|1.2.0|59467350af5f381ef429ecf210d38de5503d40fb2e9baf02f56b2ef5023ced28|ca4m4.case.benton-retaliation"
    "us.ca4.m4.norvale-injunction|1.2.0|a51383c0c1edcd56153b36291177425b09846ab607c39c28030820ef700df05f|ca4m4.case.norvale-injunction"
    "us.ca4.m4.ellison-immunity|1.2.0|c2a4f3bc07f05eb1429257320ed839ebaea837da7aa7330f4669bbb157168ce0|ca4m4.case.ellison-immunity"
    "us.ca4.m4.blueember-jmol|1.2.0|08d88e4811e8ed8ad6e642cc041365508808f7158862aa93199de867f31431ec|ca4m4.case.blueember-jmol"
    "us.ca4.m4.opengrid-foia|1.2.0|9cb2879b1cc27e98d8def7c926a38e9f4eb2cbec90785be74c009156b4a1e4c5|ca4m4.case.opengrid-foia"
    "us.ca4.m4.serrano-waiver|1.2.0|9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344|ca4m4.case.serrano-waiver"
)
list(SORT _expected_executed_root_tokens)

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
   NOT _compatibility_arch STREQUAL "x86_64" OR NOT _declared_pack_count EQUAL 13)
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
_appellate_assert_bundled_pack(
    5 "${_cinder}"
    "${_expected_cinder_pack_id}" "${_expected_cinder_version}"
    "${_expected_cinder_revision}" "${_expected_cinder_archive_sha}"
    "${_expected_cinder_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-cinderlake-writ-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    6 "${_arm_agency}"
    "${_expected_arm_agency_pack_id}" "${_expected_arm_agency_version}"
    "${_expected_arm_agency_revision}" "${_expected_arm_agency_archive_sha}"
    "${_expected_arm_agency_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-arm-agency-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    7 "${_benton_retaliation}"
    "${_expected_benton_retaliation_pack_id}" "${_expected_benton_retaliation_version}"
    "${_expected_benton_retaliation_revision}" "${_expected_benton_retaliation_archive_sha}"
    "${_expected_benton_retaliation_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-benton-retaliation-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    8 "${_norvale_injunction}"
    "${_expected_norvale_injunction_pack_id}" "${_expected_norvale_injunction_version}"
    "${_expected_norvale_injunction_revision}" "${_expected_norvale_injunction_archive_sha}"
    "${_expected_norvale_injunction_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-norvale-injunction-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    9 "${_ellison_immunity}"
    "${_expected_ellison_immunity_pack_id}" "${_expected_ellison_immunity_version}"
    "${_expected_ellison_immunity_revision}" "${_expected_ellison_immunity_archive_sha}"
    "${_expected_ellison_immunity_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-ellison-immunity-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    10 "${_blueember_jmol}"
    "${_expected_blueember_jmol_pack_id}" "${_expected_blueember_jmol_version}"
    "${_expected_blueember_jmol_revision}" "${_expected_blueember_jmol_archive_sha}"
    "${_expected_blueember_jmol_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-blueember-jmol-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    11 "${_opengrid_foia}"
    "${_expected_opengrid_foia_pack_id}" "${_expected_opengrid_foia_version}"
    "${_expected_opengrid_foia_revision}" "${_expected_opengrid_foia_archive_sha}"
    "${_expected_opengrid_foia_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-opengrid-foia-1.2.0.awpack"
)
_appellate_assert_bundled_pack(
    12 "${_serrano_waiver}"
    "${_expected_serrano_waiver_pack_id}" "${_expected_serrano_waiver_version}"
    "${_expected_serrano_waiver_revision}" "${_expected_serrano_waiver_archive_sha}"
    "${_expected_serrano_waiver_archive_size}"
    "share/appellate-workbench/packs/us-ca4-m4-serrano-waiver-1.2.0.awpack"
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

function(_appellate_assert_revision_pin_json json label expected_id expected_version
         expected_revision)
    string(JSON _field_count LENGTH "${json}")
    if(NOT _field_count EQUAL 3)
        message(FATAL_ERROR "${label} must be an exact three-field revision pin")
    endif()
    _appellate_assert_revision_json(
        "${json}" "${label}" "${expected_id}" "${expected_version}" "${expected_revision}"
    )
endfunction()

function(_appellate_assert_installed_dependencies json label expected_id dependency_order)
    if(expected_id STREQUAL _expected_ca4_pack_id)
        set(_expected_dependency_ids "${_expected_federal_pack_id}")
        set(_expected_dependency_versions "${_expected_federal_version}")
        set(_expected_dependency_revisions "${_expected_federal_revision}")
    elseif(expected_id MATCHES "^us[.]ca4[.]")
        if(dependency_order STREQUAL "install")
            set(_expected_dependency_ids
                "${_expected_federal_pack_id}"
                "${_expected_ca4_pack_id}"
                "${_expected_bench_pack_id}"
            )
            set(_expected_dependency_versions
                "${_expected_federal_version}"
                "${_expected_ca4_version}"
                "${_expected_bench_version}"
            )
            set(_expected_dependency_revisions
                "${_expected_federal_revision}"
                "${_expected_ca4_revision}"
                "${_expected_bench_revision}"
            )
        elseif(dependency_order STREQUAL "list")
            set(_expected_dependency_ids
                "${_expected_ca4_pack_id}"
                "${_expected_bench_pack_id}"
                "${_expected_federal_pack_id}"
            )
            set(_expected_dependency_versions
                "${_expected_ca4_version}"
                "${_expected_bench_version}"
                "${_expected_federal_version}"
            )
            set(_expected_dependency_revisions
                "${_expected_ca4_revision}"
                "${_expected_bench_revision}"
                "${_expected_federal_revision}"
            )
        else()
            message(FATAL_ERROR "${label} has an unknown dependency-order contract")
        endif()
    elseif(expected_id STREQUAL _expected_bench_pack_id OR
           expected_id STREQUAL _expected_federal_pack_id)
        set(_expected_dependency_ids)
        set(_expected_dependency_versions)
        set(_expected_dependency_revisions)
    else()
        message(FATAL_ERROR "${label} has no declared exact dependency contract")
    endif()

    string(JSON _dependency_count LENGTH "${json}" dependencies)
    list(LENGTH _expected_dependency_ids _expected_dependency_count)
    if(NOT _dependency_count EQUAL _expected_dependency_count)
        message(FATAL_ERROR "${label} dependency count differs")
    endif()
    if(_expected_dependency_count GREATER 0)
        math(EXPR _last_dependency_index "${_expected_dependency_count} - 1")
        foreach(_dependency_index RANGE 0 ${_last_dependency_index})
            string(JSON _dependency GET "${json}" dependencies ${_dependency_index})
            list(GET _expected_dependency_ids ${_dependency_index} _expected_dependency_id)
            list(GET _expected_dependency_versions ${_dependency_index}
                 _expected_dependency_version)
            list(GET _expected_dependency_revisions ${_dependency_index}
                 _expected_dependency_revision)
            _appellate_assert_revision_pin_json(
                "${_dependency}" "${label} dependency ${_dependency_index}"
                "${_expected_dependency_id}" "${_expected_dependency_version}"
                "${_expected_dependency_revision}"
            )
        endforeach()
    endif()
endfunction()

function(_appellate_install_exact_archive pack_cli archive catalog installed_at expected_id
         expected_version expected_revision expected_archive_sha)
    set(_scratch_environment)
    if(DEFINED _appellate_pack_tmpdir AND NOT _appellate_pack_tmpdir STREQUAL "")
        list(APPEND _scratch_environment "TMPDIR=${_appellate_pack_tmpdir}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            ${_scratch_environment}
            "${pack_cli}" install "${archive}" "${catalog}"
            --installed-at "${installed_at}"
        RESULT_VARIABLE _install_result
        OUTPUT_VARIABLE _install_output
        ERROR_VARIABLE _install_error
        TIMEOUT 60
    )
    if(NOT _install_result EQUAL 0 OR NOT _install_error STREQUAL "")
        message(
            FATAL_ERROR
            "Exact bundled pack installation failed for ${archive}:\n"
            "${_install_output}\n${_install_error}"
        )
    endif()
    string(JSON _install_field_count LENGTH "${_install_output}")
    string(JSON _install_schema GET "${_install_output}" schema_version)
    string(JSON _install_status GET "${_install_output}" status)
    string(JSON _install_command GET "${_install_output}" command)
    string(JSON _install_archive_sha GET "${_install_output}" archive_sha256)
    string(JSON _installed_at GET "${_install_output}" installed_at_utc)
    if(NOT _install_field_count EQUAL 9 OR NOT _install_schema EQUAL 1 OR
       NOT _install_status STREQUAL "ok" OR NOT _install_command STREQUAL "install" OR
       NOT _install_archive_sha STREQUAL expected_archive_sha OR
       NOT _installed_at STREQUAL installed_at)
        message(FATAL_ERROR "Exact bundled pack installation evidence differs for ${archive}")
    endif()
    _appellate_assert_revision_json(
        "${_install_output}" "Installed ${archive}"
        "${expected_id}" "${expected_version}" "${expected_revision}"
    )
    _appellate_assert_installed_dependencies(
        "${_install_output}" "Installed ${archive}" "${expected_id}" "install"
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
    if(NOT _resolved_result EQUAL 0 OR NOT _resolved_error STREQUAL "")
        message(
            FATAL_ERROR
            "Exact bundled Asterglen v0.2 closure validation failed:\n"
            "${_resolved_output}\n${_resolved_error}"
        )
    endif()
    string(JSON _resolved_field_count LENGTH "${_resolved_output}")
    string(JSON _resolved_schema GET "${_resolved_output}" schema_version)
    string(JSON _resolved_command GET "${_resolved_output}" command)
    string(JSON _resolved_status GET "${_resolved_output}" status)
    string(JSON _resolved_scope GET "${_resolved_output}" validation_scope)
    string(JSON _resolved_count GET "${_resolved_output}" resolved_revision_count)
    string(JSON _pin_count LENGTH "${_resolved_output}" revision_pins)
    if(NOT _resolved_field_count EQUAL 9 OR NOT _resolved_schema EQUAL 1 OR
       NOT _resolved_command STREQUAL "validate-resolved" OR
       NOT _resolved_status STREQUAL "ok" OR
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
        _appellate_assert_revision_pin_json(
            "${_pin}" "Resolved Asterglen v0.2 pin ${_pin_index}"
            "${_expected_pin_id}" "${_expected_pin_version}" "${_expected_pin_revision}"
        )
    endforeach()
endfunction()

function(_appellate_prepare_case_catalog pack_cli federal ca4 bench case_archive catalog
         label expected_id expected_version expected_revision expected_archive_sha)
    if(EXISTS "${catalog}")
        message(FATAL_ERROR "${label} release smoke catalog unexpectedly already exists: ${catalog}")
    endif()
    _appellate_install_exact_archive(
        "${pack_cli}" "${federal}" "${catalog}" "2026-08-19T00:00:00Z"
        "${_expected_federal_pack_id}" "${_expected_federal_version}"
        "${_expected_federal_revision}" "${_expected_federal_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${ca4}" "${catalog}" "2026-08-19T00:00:01Z"
        "${_expected_ca4_pack_id}" "${_expected_ca4_version}"
        "${_expected_ca4_revision}" "${_expected_ca4_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${bench}" "${catalog}" "2026-08-19T00:00:02Z"
        "${_expected_bench_pack_id}" "${_expected_bench_version}"
        "${_expected_bench_revision}" "${_expected_bench_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${case_archive}" "${catalog}" "2026-08-19T00:00:03Z"
        "${expected_id}" "${expected_version}"
        "${expected_revision}" "${expected_archive_sha}"
    )

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "${pack_cli}" list "${catalog}"
        RESULT_VARIABLE _list_result
        OUTPUT_VARIABLE _list_output
        ERROR_VARIABLE _list_error
        TIMEOUT 60
    )
    if(NOT _list_result EQUAL 0 OR NOT _list_error STREQUAL "")
        message(
            FATAL_ERROR
            "Exact bundled ${label} catalog listing failed:\n${_list_output}\n${_list_error}"
        )
    endif()
    string(JSON _list_field_count LENGTH "${_list_output}")
    string(JSON _list_schema GET "${_list_output}" schema_version)
    string(JSON _list_command GET "${_list_output}" command)
    string(JSON _list_status GET "${_list_output}" status)
    string(JSON _list_count LENGTH "${_list_output}" packs)
    if(NOT _list_field_count EQUAL 4 OR NOT _list_schema EQUAL 1 OR
       NOT _list_command STREQUAL "list" OR NOT _list_status STREQUAL "ok" OR
       NOT _list_count EQUAL 4)
        message(FATAL_ERROR "${label} release smoke catalog does not contain exactly four revisions")
    endif()
    set(_expected_list_ids
        "${_expected_ca4_pack_id}"
        "${_expected_bench_pack_id}"
        "${_expected_federal_pack_id}"
        "${expected_id}"
    )
    set(_expected_list_versions
        "${_expected_ca4_version}"
        "${_expected_bench_version}"
        "${_expected_federal_version}"
        "${expected_version}"
    )
    set(_expected_list_revisions
        "${_expected_ca4_revision}"
        "${_expected_bench_revision}"
        "${_expected_federal_revision}"
        "${expected_revision}"
    )
    set(_expected_list_archive_shas
        "${_expected_ca4_archive_sha}"
        "${_expected_bench_archive_sha}"
        "${_expected_federal_archive_sha}"
        "${expected_archive_sha}"
    )
    set(_expected_list_installed_at
        "2026-08-19T00:00:01Z"
        "2026-08-19T00:00:02Z"
        "2026-08-19T00:00:00Z"
        "2026-08-19T00:00:03Z"
    )
    foreach(_list_index RANGE 0 3)
        string(JSON _listed_pack GET "${_list_output}" packs ${_list_index})
        string(JSON _listed_field_count LENGTH "${_listed_pack}")
        string(JSON _listed_archive_sha GET "${_listed_pack}" archive_sha256)
        string(JSON _listed_installed_at GET "${_listed_pack}" installed_at_utc)
        list(GET _expected_list_ids ${_list_index} _expected_list_id)
        list(GET _expected_list_versions ${_list_index} _expected_list_version)
        list(GET _expected_list_revisions ${_list_index} _expected_list_revision)
        list(GET _expected_list_archive_shas ${_list_index} _expected_list_archive_sha)
        list(GET _expected_list_installed_at ${_list_index} _expected_list_install_time)
        _appellate_assert_revision_json(
            "${_listed_pack}" "Listed ${label} pin ${_list_index}"
            "${_expected_list_id}" "${_expected_list_version}"
            "${_expected_list_revision}"
        )
        _appellate_assert_installed_dependencies(
            "${_listed_pack}" "Listed ${label} pin ${_list_index}"
            "${_expected_list_id}" "list"
        )
        if(NOT _listed_field_count EQUAL 6 OR
           NOT _listed_archive_sha STREQUAL _expected_list_archive_sha OR
           NOT _listed_installed_at STREQUAL _expected_list_install_time)
            message(FATAL_ERROR "Listed ${label} archive ${_list_index} evidence differs")
        endif()
    endforeach()

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "${pack_cli}" validate-resolved "${catalog}"
            "${expected_id}" "${expected_version}" "${expected_revision}"
        RESULT_VARIABLE _resolved_result
        OUTPUT_VARIABLE _resolved_output
        ERROR_VARIABLE _resolved_error
        TIMEOUT 60
    )
    if(NOT _resolved_result EQUAL 0 OR NOT _resolved_error STREQUAL "")
        message(
            FATAL_ERROR
            "Exact bundled ${label} closure validation failed:\n"
            "${_resolved_output}\n${_resolved_error}"
        )
    endif()
    string(JSON _resolved_field_count LENGTH "${_resolved_output}")
    string(JSON _resolved_schema GET "${_resolved_output}" schema_version)
    string(JSON _resolved_command GET "${_resolved_output}" command)
    string(JSON _resolved_status GET "${_resolved_output}" status)
    string(JSON _resolved_scope GET "${_resolved_output}" validation_scope)
    string(JSON _resolved_count GET "${_resolved_output}" resolved_revision_count)
    string(JSON _pin_count LENGTH "${_resolved_output}" revision_pins)
    if(NOT _resolved_field_count EQUAL 9 OR NOT _resolved_schema EQUAL 1 OR
       NOT _resolved_command STREQUAL "validate-resolved" OR
       NOT _resolved_status STREQUAL "ok" OR
       NOT _resolved_scope STREQUAL "catalog_resolved" OR
       NOT _resolved_count EQUAL 4 OR NOT _pin_count EQUAL 4)
        message(FATAL_ERROR "${label} resolved-closure evidence is incomplete")
    endif()
    _appellate_assert_revision_json(
        "${_resolved_output}" "Resolved ${label} root"
        "${expected_id}" "${expected_version}" "${expected_revision}"
    )

    set(_expected_pin_ids
        "${_expected_ca4_pack_id}"
        "${_expected_bench_pack_id}"
        "${_expected_federal_pack_id}"
        "${expected_id}"
    )
    set(_expected_pin_versions
        "${_expected_ca4_version}"
        "${_expected_bench_version}"
        "${_expected_federal_version}"
        "${expected_version}"
    )
    set(_expected_pin_revisions
        "${_expected_ca4_revision}"
        "${_expected_bench_revision}"
        "${_expected_federal_revision}"
        "${expected_revision}"
    )
    foreach(_pin_index RANGE 0 3)
        string(JSON _pin GET "${_resolved_output}" revision_pins ${_pin_index})
        list(GET _expected_pin_ids ${_pin_index} _expected_pin_id)
        list(GET _expected_pin_versions ${_pin_index} _expected_pin_version)
        list(GET _expected_pin_revisions ${_pin_index} _expected_pin_revision)
        _appellate_assert_revision_pin_json(
            "${_pin}" "Resolved ${label} pin ${_pin_index}"
            "${_expected_pin_id}" "${_expected_pin_version}" "${_expected_pin_revision}"
        )
    endforeach()
endfunction()

function(_appellate_run_pack_success pack_cli label output_variable)
    set(_scratch_environment)
    if(DEFINED _appellate_pack_tmpdir AND NOT _appellate_pack_tmpdir STREQUAL "")
        list(APPEND _scratch_environment "TMPDIR=${_appellate_pack_tmpdir}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            ${_scratch_environment}
            "${pack_cli}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT 180
    )
    if(NOT _result EQUAL 0 OR NOT _error STREQUAL "")
        message(FATAL_ERROR "${label} failed:\n${_output}\n${_error}")
    endif()
    set("${output_variable}" "${_output}" PARENT_SCOPE)
endfunction()

function(_appellate_assert_success_envelope json command expected_field_count label)
    string(JSON _field_count LENGTH "${json}")
    string(JSON _schema_type TYPE "${json}" schema_version)
    string(JSON _schema GET "${json}" schema_version)
    string(JSON _status_type TYPE "${json}" status)
    string(JSON _status GET "${json}" status)
    string(JSON _command_type TYPE "${json}" command)
    string(JSON _reported_command GET "${json}" command)
    string(REGEX MATCHALL "\n" _newlines "${json}")
    list(LENGTH _newlines _newline_count)
    if(NOT _field_count EQUAL expected_field_count OR NOT _schema_type STREQUAL "NUMBER" OR
       NOT _schema EQUAL 1 OR NOT _status_type STREQUAL "STRING" OR
       NOT _status STREQUAL "ok" OR NOT _command_type STREQUAL "STRING" OR
       NOT _reported_command STREQUAL command OR NOT _newline_count EQUAL 1 OR
       NOT json MATCHES "^\\{" OR NOT json MATCHES "\\}\n$" OR json MATCHES "\r")
        message(FATAL_ERROR "${label} success envelope differs: ${json}")
    endif()
endfunction()

function(_appellate_run_serrano_independent_review
         pack_cli federal ca4 bench serrano source_catalog flow_root label output_identity)
    set(_subject_pack_id "${_expected_serrano_waiver_pack_id}")
    set(_subject_version "${_expected_serrano_waiver_version}")
    set(_subject_revision "${_expected_serrano_waiver_revision}")
    set(_subject_case_id "ca4m4.case.serrano-waiver")
    set(_expected_source_review_id "ca4m4.serrano.review.authoring-2026-08-19")
    set(_detached_pack_id "test.detached-review.release-serrano")
    set(_detached_version "2026.08.19")
    set(_detached_resource_id "test.detached-review.resource.release-serrano")

    if(EXISTS "${flow_root}")
        message(FATAL_ERROR "${label} detached-review verifier root already exists: ${flow_root}")
    endif()
    file(MAKE_DIRECTORY "${flow_root}")
    file(
        CHMOD "${flow_root}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )
    set(_appellate_pack_tmpdir "${flow_root}/secure-scratch-parent")
    file(MAKE_DIRECTORY "${_appellate_pack_tmpdir}")
    file(
        CHMOD "${_appellate_pack_tmpdir}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )

    set(_bundled_archives "${federal}" "${ca4}" "${bench}" "${serrano}")
    set(
        _expected_archive_shas
        "${_expected_federal_archive_sha}"
        "${_expected_ca4_archive_sha}"
        "${_expected_bench_archive_sha}"
        "${_expected_serrano_waiver_archive_sha}"
    )
    foreach(_archive_index RANGE 0 3)
        list(GET _bundled_archives ${_archive_index} _archive)
        list(GET _expected_archive_shas ${_archive_index} _expected_archive_sha)
        file(SHA256 "${_archive}" _archive_sha)
        if(NOT _archive_sha STREQUAL _expected_archive_sha)
            message(FATAL_ERROR "${label} bundled input archive changed before review flow: ${_archive}")
        endif()
    endforeach()
    _appellate_capture_tree_fingerprint(
        "${source_catalog}" "${label} source catalog before prepare" _source_catalog_before
    )

    set(_handoff_directory "${flow_root}/handoff")
    _appellate_run_pack_success(
        "${pack_cli}" "${label} prepare-independent-review" _prepare_output
        prepare-independent-review
        "${source_catalog}" "${_subject_pack_id}" "${_subject_version}"
        "${_subject_revision}" "${_subject_case_id}" "${_handoff_directory}"
    )
    _appellate_assert_success_envelope(
        "${_prepare_output}" "prepare-independent-review" 11 "${label} prepare"
    )
    if(NOT _prepare_output MATCHES "^\\{\"case_id\":\"ca4m4[.]case[.]serrano-waiver\",")
        message(FATAL_ERROR "${label} prepare output is not exact compact key order")
    endif()

    string(JSON _prepare_case_id GET "${_prepare_output}" case_id)
    string(JSON _prepare_closure_digest GET "${_prepare_output}" closure_digest)
    string(JSON _prepare_handoff_digest GET "${_prepare_output}" handoff_digest)
    string(JSON _prepare_trace_revision GET "${_prepare_output}" mechanical_trace_revision)
    string(JSON _prepare_review_id GET "${_prepare_output}" source_review_resource_id)
    string(JSON _prepare_file_count LENGTH "${_prepare_output}" files)
    string(JSON _prepare_file_0 GET "${_prepare_output}" files 0)
    string(JSON _prepare_file_1 GET "${_prepare_output}" files 1)
    string(JSON _prepare_subject GET "${_prepare_output}" subject_revision)
    string(JSON _prepare_count_fields LENGTH "${_prepare_output}" evidence_counts)
    string(LENGTH "${_prepare_closure_digest}" _closure_digest_length)
    string(LENGTH "${_prepare_handoff_digest}" _handoff_digest_length)
    if(NOT _prepare_case_id STREQUAL _subject_case_id OR
       NOT _prepare_trace_revision STREQUAL
           "appellate.realism-evidence.detached-review-replay.v1" OR
       NOT _prepare_review_id STREQUAL _expected_source_review_id OR
       NOT _prepare_file_count EQUAL 2 OR
       NOT _prepare_file_0 STREQUAL "handoff.json" OR
       NOT _prepare_file_1 STREQUAL "review-declaration.template.json" OR
       NOT _prepare_count_fields EQUAL 6 OR NOT _closure_digest_length EQUAL 64 OR
       NOT _prepare_closure_digest MATCHES "^[0-9a-f]+$" OR
       NOT _handoff_digest_length EQUAL 64 OR
       NOT _prepare_handoff_digest MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${label} prepare success fields differ")
    endif()
    _appellate_assert_revision_pin_json(
        "${_prepare_subject}" "${label} prepared subject"
        "${_subject_pack_id}" "${_subject_version}" "${_subject_revision}"
    )

    file(
        GLOB _handoff_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${_handoff_directory}"
        "${_handoff_directory}/*"
    )
    list(SORT _handoff_entries)
    set(_expected_handoff_entries "handoff.json" "review-declaration.template.json")
    if(NOT _handoff_entries STREQUAL _expected_handoff_entries)
        message(FATAL_ERROR "${label} handoff inventory differs: ${_handoff_entries}")
    endif()
    set(_handoff_file "${_handoff_directory}/handoff.json")
    set(_template_file "${_handoff_directory}/review-declaration.template.json")
    _appellate_assert_private_directory("${_handoff_directory}" "${label} handoff directory")
    _appellate_assert_private_file("${_handoff_file}" "${label} handoff.json")
    _appellate_assert_private_file("${_template_file}" "${label} declaration template")
    file(SHA256 "${_template_file}" _template_sha)
    if(NOT _template_sha STREQUAL
       "c3749adae144b712301688c86ab5f1d519bcab0b887aecc0a3bde8314409004b")
        message(FATAL_ERROR "${label} declaration template bytes differ")
    endif()
    file(SHA256 "${_handoff_file}" _handoff_sha)
    file(SIZE "${_handoff_file}" _handoff_size)
    file(SIZE "${_template_file}" _template_size)
    file(READ "${_handoff_file}" _handoff_json)
    file(READ "${_template_file}" _template_json)
    if(NOT _handoff_json MATCHES "^\\{\n    \"" OR NOT _handoff_json MATCHES "\\}\n$" OR
       NOT _template_json MATCHES "^\\{\n    \"" OR NOT _template_json MATCHES "\\}\n$")
        message(FATAL_ERROR "${label} generated handoff/template are not exact indented JSON files")
    endif()

    string(JSON _handoff_field_count LENGTH "${_handoff_json}")
    string(JSON _handoff_schema GET "${_handoff_json}" schema_version)
    string(JSON _handoff_kind GET "${_handoff_json}" handoff_kind)
    string(JSON _handoff_digest GET "${_handoff_json}" handoff_digest)
    string(JSON _payload_field_count LENGTH "${_handoff_json}" payload)
    string(JSON _payload_case GET "${_handoff_json}" payload case_id)
    string(JSON _payload_template_sha GET "${_handoff_json}" payload declaration_template_sha256)
    string(JSON _payload_trace_revision GET "${_handoff_json}" payload mechanical_trace_revision)
    string(JSON _handoff_subject GET "${_handoff_json}" payload subject_revision)
    string(JSON _source_review_field_count LENGTH "${_handoff_json}" payload source_review)
    string(JSON _handoff_source_review_id GET "${_handoff_json}" payload source_review resource_id)
    string(JSON _source_review_kind GET "${_handoff_json}" payload source_review resource_kind)
    string(JSON _source_review_state GET "${_handoff_json}" payload source_review review_state)
    string(JSON _source_review_date GET "${_handoff_json}" payload source_review reviewed_on)
    string(JSON _source_review_path GET "${_handoff_json}" payload source_review path)
    string(JSON _mechanical_field_count LENGTH "${_handoff_json}" payload mechanical_evidence)
    string(JSON _mechanical_closure GET "${_handoff_json}" payload mechanical_evidence closure_digest)
    if(NOT _handoff_field_count EQUAL 4 OR NOT _handoff_schema EQUAL 1 OR
       NOT _handoff_kind STREQUAL "independent_realism_review" OR
       NOT _handoff_digest STREQUAL _prepare_handoff_digest OR
       NOT _payload_field_count EQUAL 6 OR NOT _payload_case STREQUAL _subject_case_id OR
       NOT _payload_template_sha STREQUAL _template_sha OR
       NOT _payload_trace_revision STREQUAL _prepare_trace_revision OR
       NOT _source_review_field_count EQUAL 9 OR
       NOT _handoff_source_review_id STREQUAL _expected_source_review_id OR
       NOT _source_review_kind STREQUAL "realism_review" OR
       NOT _source_review_state STREQUAL "independent_review_pending" OR
       NOT _source_review_date STREQUAL "2026-08-19" OR
       NOT _source_review_path STREQUAL "resources/realism-review.json" OR
       NOT _mechanical_field_count EQUAL 8 OR
       NOT _mechanical_closure STREQUAL _prepare_closure_digest)
        message(FATAL_ERROR "${label} handoff payload differs")
    endif()
    _appellate_assert_revision_pin_json(
        "${_handoff_subject}" "${label} handoff subject"
        "${_subject_pack_id}" "${_subject_version}" "${_subject_revision}"
    )

    foreach(_count_name IN ITEMS authorities blobs packs record_checks resources traces)
        string(JSON _reported_count GET "${_prepare_output}" evidence_counts ${_count_name})
        string(JSON _reported_count_type TYPE "${_prepare_output}" evidence_counts ${_count_name})
        string(JSON _actual_count LENGTH "${_handoff_json}" payload mechanical_evidence ${_count_name})
        if(NOT _reported_count_type STREQUAL "NUMBER" OR
           NOT _reported_count EQUAL _actual_count OR _reported_count LESS 0)
            message(FATAL_ERROR "${label} prepare evidence count differs for ${_count_name}")
        endif()
    endforeach()
    foreach(_count_name IN ITEMS authorities blobs packs record_checks resources traces)
        string(
            JSON _prepare_count_${_count_name}
            GET "${_prepare_output}" evidence_counts ${_count_name}
        )
    endforeach()
    string(CONCAT _expected_prepare_output
        "{\"case_id\":\"${_subject_case_id}\","
        "\"closure_digest\":\"${_prepare_closure_digest}\","
        "\"command\":\"prepare-independent-review\","
        "\"evidence_counts\":{"
        "\"authorities\":${_prepare_count_authorities},"
        "\"blobs\":${_prepare_count_blobs},"
        "\"packs\":${_prepare_count_packs},"
        "\"record_checks\":${_prepare_count_record_checks},"
        "\"resources\":${_prepare_count_resources},"
        "\"traces\":${_prepare_count_traces}},"
        "\"files\":[\"handoff.json\",\"review-declaration.template.json\"],"
        "\"handoff_digest\":\"${_prepare_handoff_digest}\","
        "\"mechanical_trace_revision\":"
        "\"appellate.realism-evidence.detached-review-replay.v1\","
        "\"schema_version\":1,"
        "\"source_review_resource_id\":\"${_expected_source_review_id}\","
        "\"status\":\"ok\","
        "\"subject_revision\":{"
        "\"digest\":\"${_subject_revision}\","
        "\"pack_id\":\"${_subject_pack_id}\","
        "\"version\":\"${_subject_version}\"}}\n"
    )
    if(NOT _prepare_output STREQUAL _expected_prepare_output)
        message(FATAL_ERROR "${label} prepare compact success bytes differ")
    endif()
    string(JSON _trace_count LENGTH "${_handoff_json}" payload mechanical_evidence traces)
    if(NOT _trace_count EQUAL 2)
        message(FATAL_ERROR "${label} Serrano handoff must contain exactly two traces")
    endif()
    foreach(_trace_index RANGE 0 1)
        string(JSON _trace_revision GET "${_handoff_json}" payload mechanical_evidence traces
               ${_trace_index} engine_revision)
        if(NOT _trace_revision STREQUAL _prepare_trace_revision)
            message(FATAL_ERROR "${label} detached trace revision differs at ${_trace_index}")
        endif()
    endforeach()

    string(JSON _template_field_count LENGTH "${_template_json}")
    string(JSON _template_schema GET "${_template_json}" schema_version)
    string(JSON _template_kind GET "${_template_json}" declaration_kind)
    string(JSON _template_dimension_count LENGTH "${_template_json}" dimensions)
    string(JSON _template_reviewer_count LENGTH "${_template_json}" reviewer)
    if(NOT _template_field_count EQUAL 12 OR NOT _template_schema EQUAL 1 OR
       NOT _template_kind STREQUAL "independent_realism_review" OR
       NOT _template_dimension_count EQUAL 7 OR NOT _template_reviewer_count EQUAL 4)
        message(FATAL_ERROR "${label} declaration template shape differs")
    endif()
    foreach(_null_field IN ITEMS handoff_digest known_uncertainty review_pack_id
            review_pack_version review_resource_id review_state reviewed_on reviewer_reference)
        string(JSON _null_type TYPE "${_template_json}" ${_null_field})
        if(NOT _null_type STREQUAL "NULL")
            message(FATAL_ERROR "${label} declaration template field is not null: ${_null_field}")
        endif()
    endforeach()
    foreach(_dimension IN ITEMS bench_differentiation consequences deadlines_authority oral_argument
            procedural_law provenance record_consistency)
        string(JSON _null_type TYPE "${_template_json}" dimensions ${_dimension})
        if(NOT _null_type STREQUAL "NULL")
            message(FATAL_ERROR "${label} declaration template dimension is not null: ${_dimension}")
        endif()
    endforeach()
    foreach(_reviewer_field IN ITEMS affiliation display_name qualification reviewer_id)
        string(JSON _null_type TYPE "${_template_json}" reviewer ${_reviewer_field})
        if(NOT _null_type STREQUAL "NULL")
            message(FATAL_ERROR "${label} declaration template reviewer field is not null: ${_reviewer_field}")
        endif()
    endforeach()

    set(_declaration_file "${flow_root}/completed-declaration.json")
    string(CONCAT _declaration_json
        "{\n"
        "    \"declaration_kind\": \"independent_realism_review\",\n"
        "    \"dimensions\": {\n"
        "        \"bench_differentiation\": 2,\n"
        "        \"consequences\": 2,\n"
        "        \"deadlines_authority\": 2,\n"
        "        \"oral_argument\": 2,\n"
        "        \"procedural_law\": 2,\n"
        "        \"provenance\": 2,\n"
        "        \"record_consistency\": 2\n"
        "    },\n"
        "    \"handoff_digest\": \"${_prepare_handoff_digest}\",\n"
        "    \"known_uncertainty\": [],\n"
        "    \"review_pack_id\": \"${_detached_pack_id}\",\n"
        "    \"review_pack_version\": \"${_detached_version}\",\n"
        "    \"review_resource_id\": \"${_detached_resource_id}\",\n"
        "    \"review_state\": \"independently_reviewed\",\n"
        "    \"reviewed_on\": \"2026-08-19\",\n"
        "    \"reviewer\": {\n"
        "        \"affiliation\": null,\n"
        "        \"display_name\": \"TEST-ONLY release verifier reviewer\",\n"
        "        \"qualification\": \"TEST-ONLY fixture: no human independent review was performed\",\n"
        "        \"reviewer_id\": \"test.detached-review.reviewer\"\n"
        "    },\n"
        "    \"reviewer_reference\": \"TEST-ONLY release packaging verifier\",\n"
        "    \"schema_version\": 1\n"
        "}\n"
    )
    file(WRITE "${_declaration_file}" "${_declaration_json}")
    file(
        CHMOD "${_declaration_file}"
        PERMISSIONS OWNER_READ OWNER_WRITE
    )

    set(_pack_directory "${flow_root}/detached-pack")
    _appellate_run_pack_success(
        "${pack_cli}" "${label} finalize-independent-review" _finalize_output
        finalize-independent-review
        "${_handoff_directory}" "${_declaration_file}" "${source_catalog}" "${_pack_directory}"
    )
    _appellate_assert_success_envelope(
        "${_finalize_output}" "finalize-independent-review" 13 "${label} finalize"
    )
    if(NOT _finalize_output MATCHES "^\\{\"case_id\":\"ca4m4[.]case[.]serrano-waiver\",")
        message(FATAL_ERROR "${label} finalize output is not exact compact key order")
    endif()

    string(JSON _final_case_id GET "${_finalize_output}" case_id)
    string(JSON _final_closure_digest GET "${_finalize_output}" closure_digest)
    string(JSON _dependency_revision GET "${_finalize_output}" dependency_revision)
    string(JSON _detached_revision GET "${_finalize_output}" digest)
    string(JSON _final_file_count LENGTH "${_finalize_output}" files)
    string(JSON _final_file_0 GET "${_finalize_output}" files 0)
    string(JSON _final_file_1 GET "${_finalize_output}" files 1)
    string(JSON _final_handoff_digest GET "${_finalize_output}" handoff_digest)
    string(JSON _final_pack_id GET "${_finalize_output}" pack_id)
    string(JSON _final_resource_id GET "${_finalize_output}" review_resource_id)
    string(JSON _final_review_sha GET "${_finalize_output}" review_sha256)
    string(JSON _final_version GET "${_finalize_output}" version)
    string(LENGTH "${_detached_revision}" _detached_revision_length)
    string(LENGTH "${_final_review_sha}" _review_sha_length)
    if(NOT _final_case_id STREQUAL _subject_case_id OR
       NOT _final_closure_digest STREQUAL _prepare_closure_digest OR
       NOT _final_file_count EQUAL 2 OR NOT _final_file_0 STREQUAL "manifest.json" OR
       NOT _final_file_1 STREQUAL "resources/realism-review.json" OR
       NOT _final_handoff_digest STREQUAL _prepare_handoff_digest OR
       NOT _final_pack_id STREQUAL _detached_pack_id OR
       NOT _final_resource_id STREQUAL _detached_resource_id OR
       NOT _final_version STREQUAL _detached_version OR NOT _detached_revision_length EQUAL 64 OR
       NOT _detached_revision MATCHES "^[0-9a-f]+$" OR NOT _review_sha_length EQUAL 64 OR
       NOT _final_review_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${label} finalize success fields differ")
    endif()
    _appellate_assert_revision_pin_json(
        "${_dependency_revision}" "${label} detached dependency"
        "${_subject_pack_id}" "${_subject_version}" "${_subject_revision}"
    )
    string(CONCAT _expected_finalize_output
        "{\"case_id\":\"${_subject_case_id}\","
        "\"closure_digest\":\"${_prepare_closure_digest}\","
        "\"command\":\"finalize-independent-review\","
        "\"dependency_revision\":{"
        "\"digest\":\"${_subject_revision}\","
        "\"pack_id\":\"${_subject_pack_id}\","
        "\"version\":\"${_subject_version}\"},"
        "\"digest\":\"${_detached_revision}\","
        "\"files\":[\"manifest.json\",\"resources/realism-review.json\"],"
        "\"handoff_digest\":\"${_prepare_handoff_digest}\","
        "\"pack_id\":\"${_detached_pack_id}\","
        "\"review_resource_id\":\"${_detached_resource_id}\","
        "\"review_sha256\":\"${_final_review_sha}\","
        "\"schema_version\":1,"
        "\"status\":\"ok\","
        "\"version\":\"${_detached_version}\"}\n"
    )
    if(NOT _finalize_output STREQUAL _expected_finalize_output)
        message(FATAL_ERROR "${label} finalize compact success bytes differ")
    endif()

    file(
        GLOB_RECURSE _pack_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${_pack_directory}"
        "${_pack_directory}/*"
    )
    list(SORT _pack_entries)
    set(_expected_pack_entries "manifest.json" "resources" "resources/realism-review.json")
    if(NOT _pack_entries STREQUAL _expected_pack_entries)
        message(FATAL_ERROR "${label} detached pack inventory differs: ${_pack_entries}")
    endif()
    set(_manifest_file "${_pack_directory}/manifest.json")
    set(_review_file "${_pack_directory}/resources/realism-review.json")
    _appellate_assert_private_directory("${_pack_directory}" "${label} detached pack root")
    _appellate_assert_private_directory("${_pack_directory}/resources" "${label} resources directory")
    _appellate_assert_private_file("${_manifest_file}" "${label} manifest.json")
    _appellate_assert_private_file("${_review_file}" "${label} realism review")
    file(SHA256 "${_manifest_file}" _manifest_sha)
    file(SHA256 "${_review_file}" _review_sha)
    file(SIZE "${_manifest_file}" _manifest_size)
    file(SIZE "${_review_file}" _review_size)
    if(NOT _review_sha STREQUAL _final_review_sha)
        message(FATAL_ERROR "${label} review bytes do not match finalize response")
    endif()
    file(READ "${_manifest_file}" _manifest_json)
    file(READ "${_review_file}" _review_json)
    if(NOT _manifest_json MATCHES "^\\{\n    \"" OR NOT _manifest_json MATCHES "\\}\n$" OR
       NOT _review_json MATCHES "^\\{\n    \"" OR NOT _review_json MATCHES "\\}\n$")
        message(FATAL_ERROR "${label} finalized members are not exact indented JSON files")
    endif()

    string(JSON _manifest_fields LENGTH "${_manifest_json}")
    string(JSON _manifest_schema GET "${_manifest_json}" schema_version)
    string(JSON _manifest_pack_id GET "${_manifest_json}" pack_id)
    string(JSON _manifest_version GET "${_manifest_json}" version)
    string(JSON _manifest_blob_count LENGTH "${_manifest_json}" blobs)
    string(JSON _manifest_content_count LENGTH "${_manifest_json}" contents)
    string(JSON _manifest_dependency_count LENGTH "${_manifest_json}" dependencies)
    string(JSON _manifest_capability_count LENGTH "${_manifest_json}" required_capabilities)
    string(JSON _content_fields LENGTH "${_manifest_json}" contents 0)
    string(JSON _content_id GET "${_manifest_json}" contents 0 id)
    string(JSON _content_kind GET "${_manifest_json}" contents 0 kind)
    string(JSON _content_path GET "${_manifest_json}" contents 0 path)
    string(JSON _content_schema GET "${_manifest_json}" contents 0 schema_version)
    string(JSON _content_sha GET "${_manifest_json}" contents 0 sha256)
    string(JSON _manifest_dependency GET "${_manifest_json}" dependencies 0)
    string(JSON _manifest_dependency_fields LENGTH "${_manifest_dependency}")
    string(JSON _manifest_dependency_id GET "${_manifest_dependency}" pack_id)
    string(JSON _manifest_dependency_version GET "${_manifest_dependency}" version)
    string(JSON _manifest_dependency_sha GET "${_manifest_dependency}" sha256)
    string(JSON _capability_0_id GET "${_manifest_json}" required_capabilities 0 id)
    string(JSON _capability_0_version GET "${_manifest_json}" required_capabilities 0 version)
    string(JSON _capability_1_id GET "${_manifest_json}" required_capabilities 1 id)
    string(JSON _capability_1_version GET "${_manifest_json}" required_capabilities 1 version)
    if(NOT _manifest_fields EQUAL 7 OR NOT _manifest_schema EQUAL 2 OR
       NOT _manifest_pack_id STREQUAL _detached_pack_id OR
       NOT _manifest_version STREQUAL _detached_version OR NOT _manifest_blob_count EQUAL 0 OR
       NOT _manifest_content_count EQUAL 1 OR NOT _manifest_dependency_count EQUAL 1 OR
       NOT _manifest_capability_count EQUAL 2 OR NOT _content_fields EQUAL 5 OR
       NOT _content_id STREQUAL _detached_resource_id OR NOT _content_kind STREQUAL "realism_review" OR
       NOT _content_path STREQUAL "resources/realism-review.json" OR NOT _content_schema EQUAL 2 OR
       NOT _content_sha STREQUAL _review_sha OR
       NOT _manifest_dependency_fields EQUAL 3 OR
       NOT _manifest_dependency_id STREQUAL _subject_pack_id OR
       NOT _manifest_dependency_version STREQUAL _subject_version OR
       NOT _manifest_dependency_sha STREQUAL _subject_revision OR
       NOT _capability_0_id STREQUAL "workbench.pack.declarative-resources" OR
       NOT _capability_0_version EQUAL 2 OR
       NOT _capability_1_id STREQUAL "workbench.pack.realism-evidence" OR
       NOT _capability_1_version EQUAL 1)
        message(FATAL_ERROR "${label} final manifest differs")
    endif()
    string(JSON _review_fields LENGTH "${_review_json}")
    string(JSON _review_schema GET "${_review_json}" schema_version)
    string(JSON _review_case GET "${_review_json}" case_id)
    string(JSON _review_resource_id GET "${_review_json}" resource_id)
    string(JSON _review_kind GET "${_review_json}" resource_kind)
    string(JSON _review_state GET "${_review_json}" review_state)
    string(JSON _review_date GET "${_review_json}" reviewed_on)
    string(JSON _review_reference GET "${_review_json}" reviewer_reference)
    string(JSON _uncertainty_count LENGTH "${_review_json}" known_uncertainty)
    string(JSON _reviewer_fields LENGTH "${_review_json}" reviewer)
    string(JSON _reviewer_id GET "${_review_json}" reviewer reviewer_id)
    string(JSON _reviewer_name GET "${_review_json}" reviewer display_name)
    string(JSON _reviewer_qualification GET "${_review_json}" reviewer qualification)
    string(JSON _final_evidence GET "${_review_json}" evidence)
    string(JSON _handoff_evidence GET "${_handoff_json}" payload mechanical_evidence)
    if(NOT _review_fields EQUAL 11 OR NOT _review_schema EQUAL 2 OR
       NOT _review_case STREQUAL _subject_case_id OR
       NOT _review_resource_id STREQUAL _detached_resource_id OR
       NOT _review_kind STREQUAL "realism_review" OR
       NOT _review_state STREQUAL "independently_reviewed" OR
       NOT _review_date STREQUAL "2026-08-19" OR
       NOT _review_reference STREQUAL "TEST-ONLY release packaging verifier" OR
       NOT _uncertainty_count EQUAL 0 OR NOT _reviewer_fields EQUAL 3 OR
       NOT _reviewer_id STREQUAL "test.detached-review.reviewer" OR
       NOT _reviewer_name STREQUAL "TEST-ONLY release verifier reviewer" OR
       NOT _reviewer_qualification STREQUAL
           "TEST-ONLY fixture: no human independent review was performed" OR
       NOT _final_evidence STREQUAL _handoff_evidence)
        message(FATAL_ERROR "${label} final review differs from declaration/handoff")
    endif()
    foreach(_dimension IN ITEMS bench_differentiation consequences deadlines_authority oral_argument
            procedural_law provenance record_consistency)
        string(JSON _score_type TYPE "${_review_json}" dimensions ${_dimension})
        string(JSON _score GET "${_review_json}" dimensions ${_dimension})
        string(JSON _evidence_count LENGTH "${_review_json}" evidence dimension_evidence ${_dimension})
        if(NOT _score_type STREQUAL "NUMBER" OR NOT _score EQUAL 2 OR
           NOT _evidence_count GREATER 0)
            message(FATAL_ERROR "${label} final dimension differs: ${_dimension}")
        endif()
    endforeach()

    set(_detached_archive_a "${flow_root}/detached-a.awpack")
    set(_detached_archive_b "${flow_root}/detached-b.awpack")
    _appellate_run_pack_success(
        "${pack_cli}" "${label} first deferred export" _export_output_a
        export-deferred "${_pack_directory}" "${_detached_archive_a}"
    )
    _appellate_run_pack_success(
        "${pack_cli}" "${label} second deferred export" _export_output_b
        export-deferred "${_pack_directory}" "${_detached_archive_b}"
    )
    _appellate_assert_success_envelope(
        "${_export_output_a}" "export-deferred" 9 "${label} deferred export"
    )
    if(NOT _export_output_a STREQUAL _export_output_b)
        message(FATAL_ERROR "${label} repeated deferred-export stdout differs")
    endif()
    string(JSON _export_scope GET "${_export_output_a}" validation_scope)
    string(JSON _export_resolved_type TYPE "${_export_output_a}" resolved)
    string(JSON _export_resolved GET "${_export_output_a}" resolved)
    string(JSON _export_notice GET "${_export_output_a}" notice)
    _appellate_assert_revision_json(
        "${_export_output_a}" "${label} deferred export"
        "${_detached_pack_id}" "${_detached_version}" "${_detached_revision}"
    )
    if(NOT _export_scope STREQUAL "deferred_references" OR
       NOT _export_resolved_type STREQUAL "BOOLEAN" OR _export_resolved OR
       NOT _export_notice STREQUAL
           "Archive references remain deferred until catalog resolution")
        message(FATAL_ERROR "${label} deferred export evidence differs")
    endif()
    file(SHA256 "${_detached_archive_a}" _detached_archive_sha_a)
    file(SHA256 "${_detached_archive_b}" _detached_archive_sha_b)
    file(SIZE "${_detached_archive_a}" _detached_archive_size_a)
    file(SIZE "${_detached_archive_b}" _detached_archive_size_b)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E compare_files
            "${_detached_archive_a}" "${_detached_archive_b}"
        RESULT_VARIABLE _detached_archive_compare_result
    )
    if(NOT _detached_archive_compare_result EQUAL 0)
        message(FATAL_ERROR "${label} repeated deferred archives are not byte-identical")
    endif()
    if(NOT _detached_archive_sha_a STREQUAL _detached_archive_sha_b OR
       NOT _detached_archive_size_a EQUAL _detached_archive_size_b)
        message(FATAL_ERROR "${label} repeated deferred archive bytes differ")
    endif()

    set(_verification_catalog "${flow_root}/verification-catalog")
    _appellate_install_exact_archive(
        "${pack_cli}" "${federal}" "${_verification_catalog}" "2026-08-19T01:00:00Z"
        "${_expected_federal_pack_id}" "${_expected_federal_version}"
        "${_expected_federal_revision}" "${_expected_federal_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${ca4}" "${_verification_catalog}" "2026-08-19T01:00:01Z"
        "${_expected_ca4_pack_id}" "${_expected_ca4_version}"
        "${_expected_ca4_revision}" "${_expected_ca4_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${bench}" "${_verification_catalog}" "2026-08-19T01:00:02Z"
        "${_expected_bench_pack_id}" "${_expected_bench_version}"
        "${_expected_bench_revision}" "${_expected_bench_archive_sha}"
    )
    _appellate_install_exact_archive(
        "${pack_cli}" "${serrano}" "${_verification_catalog}" "2026-08-19T01:00:03Z"
        "${_subject_pack_id}" "${_subject_version}" "${_subject_revision}"
        "${_expected_serrano_waiver_archive_sha}"
    )
    _appellate_run_pack_success(
        "${pack_cli}" "${label} detached archive install" _detached_install_output
        install "${_detached_archive_a}" "${_verification_catalog}"
        --installed-at "2026-08-19T01:00:04Z"
    )
    _appellate_assert_success_envelope(
        "${_detached_install_output}" "install" 9 "${label} detached install"
    )
    string(JSON _installed_archive_sha GET "${_detached_install_output}" archive_sha256)
    string(JSON _installed_at GET "${_detached_install_output}" installed_at_utc)
    string(JSON _installed_dependency_count LENGTH "${_detached_install_output}" dependencies)
    string(JSON _installed_dependency GET "${_detached_install_output}" dependencies 0)
    _appellate_assert_revision_json(
        "${_detached_install_output}" "${label} installed detached review"
        "${_detached_pack_id}" "${_detached_version}" "${_detached_revision}"
    )
    if(NOT _installed_archive_sha STREQUAL _detached_archive_sha_a OR
       NOT _installed_at STREQUAL "2026-08-19T01:00:04Z" OR
       NOT _installed_dependency_count EQUAL 1)
        message(FATAL_ERROR "${label} detached install evidence differs")
    endif()
    _appellate_assert_revision_pin_json(
        "${_installed_dependency}" "${label} installed detached dependency"
        "${_subject_pack_id}" "${_subject_version}" "${_subject_revision}"
    )

    _appellate_run_pack_success(
        "${pack_cli}" "${label} detached resolved validation" _resolved_output
        validate-resolved "${_verification_catalog}"
        "${_detached_pack_id}" "${_detached_version}" "${_detached_revision}"
    )
    _appellate_assert_success_envelope(
        "${_resolved_output}" "validate-resolved" 9 "${label} detached resolved validation"
    )
    string(JSON _resolved_scope GET "${_resolved_output}" validation_scope)
    string(JSON _resolved_count GET "${_resolved_output}" resolved_revision_count)
    string(JSON _resolved_pin_count LENGTH "${_resolved_output}" revision_pins)
    _appellate_assert_revision_json(
        "${_resolved_output}" "${label} resolved detached review"
        "${_detached_pack_id}" "${_detached_version}" "${_detached_revision}"
    )
    if(NOT _resolved_scope STREQUAL "catalog_resolved" OR NOT _resolved_count EQUAL 5 OR
       NOT _resolved_pin_count EQUAL 5)
        message(FATAL_ERROR "${label} detached resolved closure differs")
    endif()
    set(
        _resolved_expected_ids
        "${_expected_ca4_pack_id}"
        "${_expected_bench_pack_id}"
        "${_expected_federal_pack_id}"
        "${_detached_pack_id}"
        "${_subject_pack_id}"
    )
    set(
        _resolved_expected_versions
        "${_expected_ca4_version}"
        "${_expected_bench_version}"
        "${_expected_federal_version}"
        "${_detached_version}"
        "${_subject_version}"
    )
    set(
        _resolved_expected_revisions
        "${_expected_ca4_revision}"
        "${_expected_bench_revision}"
        "${_expected_federal_revision}"
        "${_detached_revision}"
        "${_subject_revision}"
    )
    foreach(_pin_index RANGE 0 4)
        string(JSON _pin GET "${_resolved_output}" revision_pins ${_pin_index})
        list(GET _resolved_expected_ids ${_pin_index} _pin_id)
        list(GET _resolved_expected_versions ${_pin_index} _pin_version)
        list(GET _resolved_expected_revisions ${_pin_index} _pin_revision)
        _appellate_assert_revision_pin_json(
            "${_pin}" "${label} detached closure pin ${_pin_index}"
            "${_pin_id}" "${_pin_version}" "${_pin_revision}"
        )
    endforeach()

    _appellate_capture_tree_fingerprint(
        "${source_catalog}" "${label} source catalog after finalize" _source_catalog_after
    )
    if(NOT _source_catalog_after STREQUAL _source_catalog_before)
        message(FATAL_ERROR "${label} source catalog bytes or inventory changed")
    endif()
    foreach(_archive_index RANGE 0 3)
        list(GET _bundled_archives ${_archive_index} _archive)
        list(GET _expected_archive_shas ${_archive_index} _expected_archive_sha)
        file(SHA256 "${_archive}" _archive_sha)
        if(NOT _archive_sha STREQUAL _expected_archive_sha)
            message(FATAL_ERROR "${label} bundled input archive changed after review flow: ${_archive}")
        endif()
    endforeach()
    _appellate_assert_no_transient_residue("${source_catalog}" "${label} source catalog")
    _appellate_assert_no_transient_residue("${_verification_catalog}" "${label} verification catalog")
    _appellate_assert_no_transient_residue("${flow_root}" "${label} review flow")
    file(
        GLOB _secure_scratch_residue
        LIST_DIRECTORIES TRUE
        RELATIVE "${_appellate_pack_tmpdir}"
        "${_appellate_pack_tmpdir}/*"
    )
    if(_secure_scratch_residue)
        message(FATAL_ERROR "${label} retained secure scratch residue: ${_secure_scratch_residue}")
    endif()

    string(SHA256 _prepare_output_sha "${_prepare_output}")
    string(SHA256 _finalize_output_sha "${_finalize_output}")
    string(SHA256 _export_output_sha "${_export_output_a}")
    string(CONCAT _identity_material
        "${_prepare_output_sha}|${_finalize_output_sha}|${_export_output_sha}|"
        "${_handoff_sha}|${_handoff_size}|${_template_sha}|${_template_size}|"
        "${_manifest_sha}|${_manifest_size}|${_review_sha}|${_review_size}|"
        "${_detached_archive_sha_a}|${_detached_archive_size_a}|${_detached_revision}"
    )
    string(SHA256 _identity "${_identity_material}")
    set("${output_identity}" "${_identity}" PARENT_SCOPE)
endfunction()

function(_appellate_assert_desktop_smoke json label expected_id expected_version
         expected_revision expected_case_id output_token)
    string(JSON _field_count LENGTH "${json}")
    string(JSON _schema GET "${json}" schema_version)
    string(JSON _command GET "${json}" command)
    string(JSON _status GET "${json}" status)
    string(JSON _pack_id GET "${json}" pack_id)
    string(JSON _version GET "${json}" version)
    string(JSON _revision GET "${json}" revision_sha256)
    string(JSON _case_count GET "${json}" case_count)
    string(JSON _case_id_count LENGTH "${json}" case_ids)
    string(JSON _case_id GET "${json}" case_ids 0)
    if(NOT _field_count EQUAL 8 OR NOT _schema EQUAL 1 OR
       NOT _command STREQUAL "smoke-test" OR NOT _status STREQUAL "ok" OR
       NOT _pack_id STREQUAL expected_id OR NOT _version STREQUAL expected_version OR
       NOT _revision STREQUAL expected_revision OR NOT _case_count EQUAL 1 OR
       NOT _case_id_count EQUAL 1 OR NOT _case_id STREQUAL expected_case_id)
        message(FATAL_ERROR "${label} desktop smoke identity or case closure differs")
    endif()
    set(
        "${output_token}"
        "${_pack_id}|${_version}|${_revision}|${_case_id}"
        PARENT_SCOPE
    )
endfunction()

function(_appellate_run_case_desktop_smoke desktop catalog archive state_name label
         expected_id expected_version expected_revision expected_case_id output_token)
    set(_state "${_root}/${state_name}")
    set(_state_runtime "${_state}/runtime")
    if(EXISTS "${_state}")
        message(FATAL_ERROR "${label} desktop smoke state unexpectedly already exists: ${_state}")
    endif()
    file(MAKE_DIRECTORY "${_state_runtime}")
    file(
        CHMOD "${_state_runtime}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
    )
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=LD_LIBRARY_PATH
            --unset=LD_PRELOAD
            --unset=QT_PLUGIN_PATH
            --unset=QT_QPA_PLATFORM_PLUGIN_PATH
            "QT_QPA_PLATFORM=offscreen"
            "XDG_RUNTIME_DIR=${_state_runtime}"
            "XDG_CONFIG_HOME=${_state}/config"
            "XDG_DATA_HOME=${_state}/data"
            "XDG_CACHE_HOME=${_state}/cache"
            "${desktop}" --smoke-test --catalog "${catalog}" "${archive}"
        RESULT_VARIABLE _desktop_result
        OUTPUT_VARIABLE _desktop_output
        ERROR_VARIABLE _desktop_error
        TIMEOUT 120
    )
    if(NOT _desktop_result EQUAL 0 OR NOT _desktop_error STREQUAL "")
        message(
            FATAL_ERROR
            "${label} desktop smoke failed:\n${_desktop_output}\n${_desktop_error}"
        )
    endif()
    _appellate_assert_desktop_smoke(
        "${_desktop_output}" "${label}"
        "${expected_id}" "${expected_version}" "${expected_revision}"
        "${expected_case_id}" _executed_root_token
    )
    set("${output_token}" "${_executed_root_token}" PARENT_SCOPE)
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
            "Shipped starter extraction failed:\n"
            "${_template_output}\n${_template_error}"
        )
    endif()
    execute_process(
        COMMAND
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
            "Shipped starter export failed:\n"
            "${_export_output}\n${_export_error}"
        )
    endif()
    execute_process(
        COMMAND
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
            "Shipped starter validation failed:\n"
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
            "Installed offline E2E failed:\n"
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
    TIMEOUT 120
)
if(NOT _desktop_result EQUAL 0 OR NOT _desktop_error STREQUAL "")
    message(FATAL_ERROR "Installed desktop smoke failed:\n${_desktop_output}\n${_desktop_error}")
endif()
_appellate_assert_desktop_smoke(
    "${_desktop_output}" "Installed Asterglen v0.2"
    "${_expected_v2_pack_id}" "${_expected_v2_version}" "${_expected_v2_revision}"
    "ca4r54b.case.asterglen" _installed_default_token
)
set(_installed_executed_root_tokens "${_installed_default_token}")

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

foreach(_case_index RANGE 0 7)
    list(GET _additional_case_keys ${_case_index} _case_key)
    list(GET _additional_case_labels ${_case_index} _case_label)
    list(GET _additional_case_archive_names ${_case_index} _case_archive_name)
    list(GET _additional_case_pack_ids ${_case_index} _case_pack_id)
    list(GET _additional_case_revisions ${_case_index} _case_revision)
    list(GET _additional_case_archive_shas ${_case_index} _case_archive_sha)
    list(GET _additional_case_ids ${_case_index} _case_id)
    set(_case_archive "${_packs_root}/${_case_archive_name}")
    set(_case_catalog "${_root}/installed-${_case_key}-catalog")
    _appellate_prepare_case_catalog(
        "${_pack_cli}" "${_federal}" "${_ca4}" "${_bench}"
        "${_case_archive}" "${_case_catalog}" "${_case_label}"
        "${_case_pack_id}" "1.2.0" "${_case_revision}" "${_case_archive_sha}"
    )
    _appellate_run_case_desktop_smoke(
        "${_desktop}" "${_case_catalog}" "${_case_archive}"
        "installed-${_case_key}-desktop" "Installed ${_case_label}"
        "${_case_pack_id}" "1.2.0" "${_case_revision}" "${_case_id}"
        _installed_case_token
    )
    list(APPEND _installed_executed_root_tokens "${_installed_case_token}")
endforeach()
list(LENGTH _installed_executed_root_tokens _installed_executed_root_count)
list(SORT _installed_executed_root_tokens)
if(NOT _installed_executed_root_count EQUAL 9 OR
   NOT _installed_executed_root_tokens STREQUAL _expected_executed_root_tokens)
    message(FATAL_ERROR "Installed desktop smoke did not execute the exact nine-root token set")
endif()
_appellate_run_serrano_independent_review(
    "${_pack_cli}" "${_federal}" "${_ca4}" "${_bench}" "${_serrano_waiver}"
    "${_root}/installed-serrano-waiver-catalog"
    "${_root}/installed-detached-review-flow"
    "Installed Serrano detached review"
    _installed_detached_review_identity
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
set(_relocated_cinder
    "${_relocated_packs_root}/us-ca4-m4-cinderlake-writ-1.2.0.awpack")
_appellate_assert_documentation_tree("${_relocated}" "Relocated")
_appellate_assert_independent_review_wrong_arity("${_relocated_pack_cli}" "Relocated")
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
    TIMEOUT 120
)
if(NOT _relocated_result EQUAL 0)
    message(FATAL_ERROR "Relocated desktop smoke failed:\n${_relocated_output}\n${_relocated_error}")
endif()
# QT_DEBUG_PLUGINS is enabled only for this relocated default smoke. Its stderr is accepted solely
# as plugin-location evidence and is constrained to the relocated plugin tree below; every other
# ordinary root smoke requires empty stderr.
_appellate_assert_desktop_smoke(
    "${_relocated_output}" "Relocated Asterglen v0.2"
    "${_expected_v2_pack_id}" "${_expected_v2_version}" "${_expected_v2_revision}"
    "ca4r54b.case.asterglen" _relocated_default_token
)
set(_relocated_executed_root_tokens "${_relocated_default_token}")
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

foreach(_case_index RANGE 0 7)
    list(GET _additional_case_keys ${_case_index} _case_key)
    list(GET _additional_case_labels ${_case_index} _case_label)
    list(GET _additional_case_archive_names ${_case_index} _case_archive_name)
    list(GET _additional_case_pack_ids ${_case_index} _case_pack_id)
    list(GET _additional_case_revisions ${_case_index} _case_revision)
    list(GET _additional_case_archive_shas ${_case_index} _case_archive_sha)
    list(GET _additional_case_ids ${_case_index} _case_id)
    set(_case_archive "${_relocated_packs_root}/${_case_archive_name}")
    set(_case_catalog "${_root}/relocated-${_case_key}-catalog")
    _appellate_prepare_case_catalog(
        "${_relocated_pack_cli}" "${_relocated_federal}" "${_relocated_ca4}"
        "${_relocated_bench}" "${_case_archive}" "${_case_catalog}" "${_case_label}"
        "${_case_pack_id}" "1.2.0" "${_case_revision}" "${_case_archive_sha}"
    )
    _appellate_run_case_desktop_smoke(
        "${_relocated_desktop}" "${_case_catalog}" "${_case_archive}"
        "relocated-${_case_key}-desktop" "Relocated ${_case_label}"
        "${_case_pack_id}" "1.2.0" "${_case_revision}" "${_case_id}"
        _relocated_case_token
    )
    list(APPEND _relocated_executed_root_tokens "${_relocated_case_token}")
endforeach()
list(LENGTH _relocated_executed_root_tokens _relocated_executed_root_count)
list(SORT _relocated_executed_root_tokens)
if(NOT _relocated_executed_root_count EQUAL 9 OR
   NOT _relocated_executed_root_tokens STREQUAL _expected_executed_root_tokens)
    message(FATAL_ERROR "Relocated desktop smoke did not execute the exact nine-root token set")
endif()
_appellate_run_serrano_independent_review(
    "${_relocated_pack_cli}" "${_relocated_federal}" "${_relocated_ca4}"
    "${_relocated_bench}"
    "${_relocated_packs_root}/us-ca4-m4-serrano-waiver-1.2.0.awpack"
    "${_root}/relocated-serrano-waiver-catalog"
    "${_root}/relocated-detached-review-flow"
    "Relocated Serrano detached review"
    _relocated_detached_review_identity
)
if(NOT _relocated_detached_review_identity STREQUAL _installed_detached_review_identity)
    message(FATAL_ERROR "Installed and relocated detached-review stdout/artifact bytes differ")
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

file(READ "${_root}/.appellate-install-verifier-root" _verifier_root_sentinel)
if(NOT _verifier_root_sentinel STREQUAL "appellate-install-verifier-root\n")
    message(FATAL_ERROR "Refusing to remove an install verifier root without its exact sentinel")
endif()
file(REMOVE_RECURSE "${_root}")
if(EXISTS "${_root}")
    message(FATAL_ERROR "Temporary install tree could not be removed")
endif()

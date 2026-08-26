qt_add_executable(
    tst_pack_reader
    packs/tst_pack_reader.cpp
)
target_link_libraries(
    tst_pack_reader
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_pack_reader
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_pack_reader PRIVATE cxx_std_26)

add_test(NAME pack_reader COMMAND tst_pack_reader)

qt_add_executable(tst_schema_dispatch packs/tst_schema_dispatch.cpp)
target_link_libraries(
    tst_schema_dispatch
    PRIVATE
        appellate::packs
        appellate::engine
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_schema_dispatch
    PRIVATE
        APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        APPELLATE_GOLD_PACK="${PROJECT_SOURCE_DIR}/content/ca4-rule54b/pack"
)
target_compile_features(tst_schema_dispatch PRIVATE cxx_std_26)
add_test(NAME schema_dispatch COMMAND tst_schema_dispatch)

qt_add_executable(tst_pack_archive packs/tst_pack_archive.cpp)
target_link_libraries(
    tst_pack_archive
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        LibArchive::LibArchive
        Qt6::Test
)
target_compile_definitions(
    tst_pack_archive
    PRIVATE APPELLATE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
            APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(tst_pack_archive PRIVATE "${PROJECT_SOURCE_DIR}/src/packs/src")
target_compile_features(tst_pack_archive PRIVATE cxx_std_26)
add_test(NAME pack_archive COMMAND tst_pack_archive)

qt_add_executable(tst_pack_catalog packs/tst_pack_catalog.cpp)
target_link_libraries(
    tst_pack_catalog
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_include_directories(tst_pack_catalog PRIVATE "${PROJECT_SOURCE_DIR}/src/packs/src")
target_compile_definitions(tst_pack_catalog PRIVATE APPELLATE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_features(tst_pack_catalog PRIVATE cxx_std_26)
add_test(NAME pack_catalog COMMAND tst_pack_catalog)

qt_add_executable(
    tst_pack_catalog_protected_inputs
    packs/tst_pack_catalog_protected_inputs.cpp
)
target_link_libraries(
    tst_pack_catalog_protected_inputs
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_include_directories(
    tst_pack_catalog_protected_inputs
    PRIVATE "${PROJECT_SOURCE_DIR}/src/packs/src"
)
target_compile_features(tst_pack_catalog_protected_inputs PRIVATE cxx_std_26)
add_test(NAME pack_catalog_protected_inputs COMMAND tst_pack_catalog_protected_inputs)

qt_add_executable(
    tst_pack_dependency_resolution
    packs/tst_pack_dependency_resolution.cpp
)
target_link_libraries(
    tst_pack_dependency_resolution
    PRIVATE
        appellate::application
        appellate::pack_cli
        appellate::record_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_pack_dependency_resolution
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(
    tst_pack_dependency_resolution
    PRIVATE "${PROJECT_SOURCE_DIR}/src/packs/src"
)
target_compile_features(tst_pack_dependency_resolution PRIVATE cxx_std_26)
add_test(NAME pack_dependency_resolution COMMAND tst_pack_dependency_resolution)
set_tests_properties(
    pack_dependency_resolution
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(tst_pack_cli cli/tst_pack_cli.cpp)
target_link_libraries(
    tst_pack_cli
    PRIVATE
        appellate::engine
        appellate::pack_cli
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        appellate::storage
        Qt6::Test
)
target_compile_definitions(tst_pack_cli PRIVATE APPELLATE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_features(tst_pack_cli PRIVATE cxx_std_26)
add_test(NAME pack_cli COMMAND tst_pack_cli)
if(APPELLATE_ENABLE_ASAN OR APPELLATE_ENABLE_UBSAN)
    set(_appellate_pack_cli_function_timeout_ms 3600000)
    set_tests_properties(pack_cli PROPERTIES TIMEOUT 7200)
else()
    set(_appellate_pack_cli_function_timeout_ms 1800000)
    set_tests_properties(pack_cli PROPERTIES TIMEOUT 2400)
endif()
set_tests_properties(
    pack_cli
    PROPERTIES RUN_SERIAL TRUE
               ENVIRONMENT
               "QTEST_FUNCTION_TIMEOUT=${_appellate_pack_cli_function_timeout_ms}"
)

qt_add_executable(
    tst_independent_review_publisher
    cli/tst_independent_review_publisher.cpp
)
target_link_libraries(
    tst_independent_review_publisher
    PRIVATE
        appellate::pack_cli
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_include_directories(
    tst_independent_review_publisher
    PRIVATE "${PROJECT_SOURCE_DIR}/src/cli"
)
target_compile_features(tst_independent_review_publisher PRIVATE cxx_std_26)
add_test(NAME independent_review_publisher COMMAND tst_independent_review_publisher)

qt_add_executable(tst_procedure_engine engine/tst_procedure_engine.cpp)
target_link_libraries(
    tst_procedure_engine
    PRIVATE
        appellate::engine
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_procedure_engine PRIVATE cxx_std_26)
add_test(NAME procedure_engine COMMAND tst_procedure_engine)

qt_add_executable(tst_workflow_engine engine/tst_workflow_engine.cpp)
target_link_libraries(
    tst_workflow_engine
    PRIVATE
        appellate::engine
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_workflow_engine PRIVATE cxx_std_26)
add_test(NAME workflow_engine COMMAND tst_workflow_engine)

qt_add_executable(
    tst_workflow_action_planner
    app/tst_workflow_action_planner.cpp
)
target_link_libraries(
    tst_workflow_action_planner
    PRIVATE
        appellate::application
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_workflow_action_planner
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_workflow_action_planner PRIVATE cxx_std_26)
add_test(NAME workflow_action_planner COMMAND tst_workflow_action_planner)

qt_add_executable(tst_oral_argument_engine engine/tst_oral_argument_engine.cpp)
target_link_libraries(
    tst_oral_argument_engine
    PRIVATE
        appellate::engine
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_oral_argument_engine PRIVATE cxx_std_26)
add_test(NAME oral_argument_engine COMMAND tst_oral_argument_engine)

qt_add_executable(tst_asset_store storage/tst_asset_store.cpp)
target_link_libraries(
    tst_asset_store
    PRIVATE
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_asset_store PRIVATE cxx_std_26)
add_test(NAME asset_store COMMAND tst_asset_store)

qt_add_executable(tst_session_store storage/tst_session_store.cpp)
target_link_libraries(
    tst_session_store
    PRIVATE
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_session_store PRIVATE cxx_std_26)
add_test(NAME session_store COMMAND tst_session_store)

qt_add_executable(tst_session_archive storage/tst_session_archive.cpp)
target_link_libraries(
    tst_session_archive
    PRIVATE
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_session_archive PRIVATE cxx_std_26)
add_test(NAME session_archive COMMAND tst_session_archive)

qt_add_executable(tst_event_codec storage/tst_event_codec.cpp)
target_link_libraries(
    tst_event_codec
    PRIVATE
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_event_codec PRIVATE cxx_std_26)
add_test(NAME event_codec COMMAND tst_event_codec)

qt_add_executable(tst_workflow_codec storage/tst_workflow_codec.cpp)
target_link_libraries(
    tst_workflow_codec
    PRIVATE
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_workflow_codec PRIVATE cxx_std_26)
add_test(NAME workflow_codec COMMAND tst_workflow_codec)

qt_add_executable(tst_oral_argument_codec storage/tst_oral_argument_codec.cpp)
target_link_libraries(
    tst_oral_argument_codec
    PRIVATE
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_oral_argument_codec PRIVATE cxx_std_26)
add_test(NAME oral_argument_codec COMMAND tst_oral_argument_codec)

if(APPELLATE_SYNC_AVAILABLE)
    add_subdirectory(sync)
endif()

qt_add_executable(tst_initiation_resume integration/tst_initiation_resume.cpp)
target_link_libraries(
    tst_initiation_resume
    PRIVATE
        appellate::application
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_initiation_resume PRIVATE cxx_std_26)
add_test(NAME initiation_resume COMMAND tst_initiation_resume)

qt_add_executable(
    tst_workflow_session_resume
    integration/tst_workflow_session_resume.cpp
)
target_link_libraries(
    tst_workflow_session_resume
    PRIVATE
        appellate::application
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_workflow_session_resume PRIVATE cxx_std_26)
add_test(NAME workflow_session_resume COMMAND tst_workflow_session_resume)

qt_add_executable(
    tst_oral_argument_session_resume
    integration/tst_oral_argument_session_resume.cpp
)
target_link_libraries(
    tst_oral_argument_session_resume
    PRIVATE
        appellate::application
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_oral_argument_session_resume PRIVATE cxx_std_26)
add_test(NAME oral_argument_session_resume COMMAND tst_oral_argument_session_resume)

qt_add_executable(
    tst_oral_argument_desktop_e2e
    integration/tst_oral_argument_desktop_e2e.cpp
)
target_link_libraries(
    tst_oral_argument_desktop_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Sql
        Qt6::Test
)
target_compile_definitions(
    tst_oral_argument_desktop_e2e
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_oral_argument_desktop_e2e PRIVATE cxx_std_26)
add_test(NAME oral_argument_desktop_e2e COMMAND tst_oral_argument_desktop_e2e)
set_tests_properties(
    oral_argument_desktop_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "e2e;ui;local"
)

qt_add_executable(
    tst_main_window_local_sessions
    integration/tst_main_window_local_sessions.cpp
)
target_link_libraries(
    tst_main_window_local_sessions
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Sql
        Qt6::Test
)
target_compile_definitions(
    tst_main_window_local_sessions
    PRIVATE
        APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        APPELLATE_GOLD_ARCHIVE="${PROJECT_SOURCE_DIR}/content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack"
)
target_compile_features(tst_main_window_local_sessions PRIVATE cxx_std_26)
add_test(NAME main_window_local_sessions COMMAND tst_main_window_local_sessions)
set_tests_properties(
    main_window_local_sessions
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "e2e;ui;local"
)

qt_add_executable(
    tst_local_session_permissions
    integration/tst_local_session_permissions.cpp
)
target_link_libraries(
    tst_local_session_permissions
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_local_session_permissions PRIVATE cxx_std_26)
add_test(NAME local_session_permissions COMMAND tst_local_session_permissions)
set_tests_properties(local_session_permissions PROPERTIES LABELS "integration;local;security")

qt_add_executable(
    tst_desktop_command_line
    integration/tst_desktop_command_line.cpp
)
target_link_libraries(
    tst_desktop_command_line
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_desktop_command_line
    PRIVATE
        APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        APPELLATE_GOLD_ARCHIVE="${PROJECT_SOURCE_DIR}/content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack"
        APPELLATE_WORKBENCH_EXECUTABLE="$<TARGET_FILE:appellate-workbench>"
)
target_compile_features(tst_desktop_command_line PRIVATE cxx_std_26)
add_dependencies(tst_desktop_command_line appellate-workbench)
add_test(NAME desktop_command_line COMMAND tst_desktop_command_line)
set_tests_properties(
    desktop_command_line
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "e2e;ui;local;packaging"
)

qt_add_executable(tst_out_of_tree_pack integration/tst_out_of_tree_pack.cpp)
target_link_libraries(
    tst_out_of_tree_pack
    PRIVATE
        appellate::packs
        appellate::engine
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_out_of_tree_pack
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_out_of_tree_pack PRIVATE cxx_std_26)
add_test(NAME out_of_tree_pack COMMAND tst_out_of_tree_pack)

qt_add_executable(tst_gold_case_trace integration/tst_gold_case_trace.cpp)
target_link_libraries(
    tst_gold_case_trace
    PRIVATE
        appellate::packs
        appellate::engine
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_gold_case_trace
    PRIVATE
        APPELLATE_GOLD_PACK="${PROJECT_SOURCE_DIR}/content/ca4-rule54b/pack"
        APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_gold_case_trace PRIVATE cxx_std_26)
add_test(NAME gold_case_trace COMMAND tst_gold_case_trace)

qt_add_executable(
    tst_m4_foundation_packs
    integration/tst_m4_foundation_packs.cpp
)
target_link_libraries(
    tst_m4_foundation_packs
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        LibArchive::LibArchive
        Qt6::Test
)
target_compile_definitions(
    tst_m4_foundation_packs
    PRIVATE APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_foundation_packs PRIVATE cxx_std_26)
add_test(NAME m4_foundation_packs COMMAND tst_m4_foundation_packs)
set_tests_properties(
    m4_foundation_packs
    PROPERTIES LABELS "content;integration;local"
)

qt_add_executable(
    tst_gold_record_workspace
    integration/tst_gold_record_workspace.cpp
)
target_link_libraries(
    tst_gold_record_workspace
    PRIVATE
        appellate::record_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_gold_record_workspace
    PRIVATE
        APPELLATE_GOLD_ARCHIVE="${PROJECT_SOURCE_DIR}/content/ca4-rule54b/us-ca4-rule54b-asterglen-0.1.0.awpack"
)
target_compile_features(tst_gold_record_workspace PRIVATE cxx_std_26)
add_test(NAME gold_record_workspace COMMAND tst_gold_record_workspace)
set_tests_properties(
    gold_record_workspace
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(tst_bench_profile_editor ui/tst_bench_profile_editor.cpp)
target_link_libraries(
    tst_bench_profile_editor
    PRIVATE
        appellate::bench_profile_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_bench_profile_editor PRIVATE cxx_std_26)
add_test(NAME bench_profile_editor COMMAND tst_bench_profile_editor)
set_tests_properties(bench_profile_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME bench_profile_editor_high_dpi COMMAND tst_bench_profile_editor)
set_tests_properties(
    bench_profile_editor_high_dpi
    PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;APPELLATE_TEST_REQUIRE_2X_DPR=1"
        LABELS "ui;local;high-dpi"
)

qt_add_executable(tst_record_workspace ui/tst_record_workspace.cpp)
target_link_libraries(
    tst_record_workspace
    PRIVATE
        appellate::record_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_record_workspace PRIVATE cxx_std_26)
add_test(NAME record_workspace COMMAND tst_record_workspace)
set_tests_properties(record_workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME record_workspace_high_dpi COMMAND tst_record_workspace)
set_tests_properties(
    record_workspace_high_dpi
    PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;APPELLATE_TEST_REQUIRE_2X_DPR=1"
        LABELS "ui;local;high-dpi"
)

qt_add_executable(
    tst_installed_record_workspace
    integration/tst_installed_record_workspace.cpp
)
target_link_libraries(
    tst_installed_record_workspace
    PRIVATE
        appellate::record_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_installed_record_workspace
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_installed_record_workspace PRIVATE cxx_std_26)
add_test(NAME installed_record_workspace COMMAND tst_installed_record_workspace)
set_tests_properties(
    installed_record_workspace
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(tst_main_window ui/tst_main_window.cpp)
target_link_libraries(
    tst_main_window
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_main_window
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(tst_main_window PRIVATE cxx_std_26)
add_test(NAME main_window COMMAND tst_main_window)
set_tests_properties(
    main_window
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "ui;local"
)
add_test(NAME main_window_high_dpi COMMAND tst_main_window)
set_tests_properties(
    main_window_high_dpi
    PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;APPELLATE_TEST_REQUIRE_2X_DPR=1"
        LABELS "ui;local;high-dpi"
)

qt_add_executable(
    tst_oral_argument_workspace
    ui/tst_oral_argument_workspace.cpp
)
target_link_libraries(
    tst_oral_argument_workspace
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_oral_argument_workspace PRIVATE cxx_std_26)
add_test(NAME oral_argument_workspace COMMAND tst_oral_argument_workspace)
set_tests_properties(
    oral_argument_workspace
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "ui;local"
)
add_test(NAME oral_argument_workspace_high_dpi COMMAND tst_oral_argument_workspace)
set_tests_properties(
    oral_argument_workspace_high_dpi
    PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;APPELLATE_TEST_REQUIRE_2X_DPR=1"
        LABELS "ui;local;high-dpi"
)

qt_add_executable(
    tst_m4_arm_agency
    integration/tst_m4_arm_agency.cpp
)
target_link_libraries(
    tst_m4_arm_agency
    PRIVATE
        appellate::engine
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_arm_agency
    PRIVATE
        APPELLATE_M4_ARM_ROOT="${PROJECT_SOURCE_DIR}/content/m4/arm-agency"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_arm_agency PRIVATE cxx_std_26)
add_test(NAME m4_arm_agency COMMAND tst_m4_arm_agency)
set_tests_properties(
    m4_arm_agency
    PROPERTIES LABELS "content;integration;local"
)

qt_add_executable(
    tst_m4_arm_agency_ui_e2e
    integration/tst_m4_arm_agency_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_arm_agency_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_arm_agency_ui_e2e
    PRIVATE
        APPELLATE_M4_ARM_ROOT="${PROJECT_SOURCE_DIR}/content/m4/arm-agency"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_arm_agency_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_arm_agency_ui_e2e COMMAND tst_m4_arm_agency_ui_e2e)
set_tests_properties(
    m4_arm_agency_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "e2e;ui;local;offscreen"
)

qt_add_executable(
    tst_m4_benton_retaliation
    integration/tst_m4_benton_retaliation.cpp
)
target_link_libraries(
    tst_m4_benton_retaliation
    PRIVATE
        appellate::engine
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_benton_retaliation
    PRIVATE
        APPELLATE_M4_BENTON_ROOT="${PROJECT_SOURCE_DIR}/content/m4/benton-retaliation"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_benton_retaliation PRIVATE cxx_std_26)
add_test(NAME m4_benton_retaliation COMMAND tst_m4_benton_retaliation)
set_tests_properties(
    m4_benton_retaliation
    PROPERTIES LABELS "content;integration;local"
)

qt_add_executable(
    tst_m4_benton_retaliation_ui_e2e
    integration/tst_m4_benton_retaliation_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_benton_retaliation_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_benton_retaliation_ui_e2e
    PRIVATE
        APPELLATE_M4_BENTON_ROOT="${PROJECT_SOURCE_DIR}/content/m4/benton-retaliation"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_benton_retaliation_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_benton_retaliation_ui_e2e COMMAND tst_m4_benton_retaliation_ui_e2e)
set_tests_properties(
    m4_benton_retaliation_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "e2e;ui;local;offscreen"
)

qt_add_executable(
    tst_asterglen_rule54b_v02
    integration/tst_asterglen_rule54b_v02.cpp
)
target_link_libraries(
    tst_asterglen_rule54b_v02
    PRIVATE
        appellate::engine
        appellate::packs
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_asterglen_rule54b_v02
    PRIVATE
        APPELLATE_CA4_RULE54B_ROOT="${PROJECT_SOURCE_DIR}/content/ca4-rule54b"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_asterglen_rule54b_v02 PRIVATE cxx_std_26)
add_test(NAME asterglen_rule54b_v02 COMMAND tst_asterglen_rule54b_v02)
set_tests_properties(
    asterglen_rule54b_v02
    PROPERTIES LABELS "content;integration;local"
)

qt_add_executable(
    tst_asterglen_rule54b_v02_ui_e2e
    integration/tst_asterglen_rule54b_v02_ui_e2e.cpp
)
target_link_libraries(
    tst_asterglen_rule54b_v02_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_asterglen_rule54b_v02_ui_e2e
    PRIVATE
        APPELLATE_CA4_RULE54B_ROOT="${PROJECT_SOURCE_DIR}/content/ca4-rule54b"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_asterglen_rule54b_v02_ui_e2e PRIVATE cxx_std_26)
add_test(NAME asterglen_rule54b_v02_ui_e2e COMMAND tst_asterglen_rule54b_v02_ui_e2e)
set_tests_properties(
    asterglen_rule54b_v02_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

qt_add_executable(
    tst_m4_norvale_injunction
    integration/tst_m4_norvale_injunction.cpp
)
target_link_libraries(
    tst_m4_norvale_injunction
    PRIVATE
        appellate::engine
        appellate::packs
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_norvale_injunction
    PRIVATE
        APPELLATE_M4_NORVALE_ROOT="${PROJECT_SOURCE_DIR}/content/m4/norvale-injunction"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_norvale_injunction PRIVATE cxx_std_26)
add_test(NAME m4_norvale_injunction COMMAND tst_m4_norvale_injunction)
set_tests_properties(
    m4_norvale_injunction
    PROPERTIES
        LABELS "content;integration;local"
)

qt_add_executable(
    tst_m4_norvale_injunction_ui_e2e
    integration/tst_m4_norvale_injunction_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_norvale_injunction_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_norvale_injunction_ui_e2e
    PRIVATE
        APPELLATE_M4_NORVALE_ROOT="${PROJECT_SOURCE_DIR}/content/m4/norvale-injunction"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_norvale_injunction_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_norvale_injunction_ui_e2e COMMAND tst_m4_norvale_injunction_ui_e2e)
set_tests_properties(
    m4_norvale_injunction_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

qt_add_executable(
    tst_m4_ellison_immunity
    integration/tst_m4_ellison_immunity.cpp
)
target_link_libraries(
    tst_m4_ellison_immunity
    PRIVATE
        appellate::engine
        appellate::packs
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_ellison_immunity
    PRIVATE
        APPELLATE_M4_ELLISON_ROOT="${PROJECT_SOURCE_DIR}/content/m4/ellison-immunity"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_ellison_immunity PRIVATE cxx_std_26)
add_test(NAME m4_ellison_immunity COMMAND tst_m4_ellison_immunity)
set_tests_properties(
    m4_ellison_immunity
    PROPERTIES
        LABELS "content;integration;local"
)

qt_add_executable(
    tst_m4_ellison_immunity_ui_e2e
    integration/tst_m4_ellison_immunity_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_ellison_immunity_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_ellison_immunity_ui_e2e
    PRIVATE
        APPELLATE_M4_ELLISON_ROOT="${PROJECT_SOURCE_DIR}/content/m4/ellison-immunity"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_ellison_immunity_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_ellison_immunity_ui_e2e COMMAND tst_m4_ellison_immunity_ui_e2e)
set_tests_properties(
    m4_ellison_immunity_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

qt_add_executable(
    tst_m4_blueember_jmol
    integration/tst_m4_blueember_jmol.cpp
)
target_link_libraries(
    tst_m4_blueember_jmol
    PRIVATE
        appellate::engine
        appellate::packs
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_blueember_jmol
    PRIVATE
        APPELLATE_M4_BLUEEMBER_ROOT="${PROJECT_SOURCE_DIR}/content/m4/blueember-jmol"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_blueember_jmol PRIVATE cxx_std_26)
add_test(NAME m4_blueember_jmol COMMAND tst_m4_blueember_jmol)
set_tests_properties(
    m4_blueember_jmol
    PROPERTIES
        LABELS "content;integration;local"
)

qt_add_executable(
    tst_m4_blueember_jmol_ui_e2e
    integration/tst_m4_blueember_jmol_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_blueember_jmol_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_blueember_jmol_ui_e2e
    PRIVATE
        APPELLATE_M4_BLUEEMBER_ROOT="${PROJECT_SOURCE_DIR}/content/m4/blueember-jmol"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_blueember_jmol_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_blueember_jmol_ui_e2e COMMAND tst_m4_blueember_jmol_ui_e2e)
set_tests_properties(
    m4_blueember_jmol_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

qt_add_executable(
    tst_m4_opengrid_foia
    integration/tst_m4_opengrid_foia.cpp
)
target_link_libraries(
    tst_m4_opengrid_foia
    PRIVATE
        appellate::application
        appellate::engine
        appellate::packs
        appellate::record_ui
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_opengrid_foia
    PRIVATE
        APPELLATE_M4_OPENGRID_ROOT="${PROJECT_SOURCE_DIR}/content/m4/opengrid-foia"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_opengrid_foia PRIVATE cxx_std_26)
add_test(NAME m4_opengrid_foia COMMAND tst_m4_opengrid_foia)
set_tests_properties(
    m4_opengrid_foia
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;offscreen"
)

qt_add_executable(
    tst_m4_opengrid_foia_ui_e2e
    integration/tst_m4_opengrid_foia_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_opengrid_foia_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_opengrid_foia_ui_e2e
    PRIVATE
        APPELLATE_M4_OPENGRID_ROOT="${PROJECT_SOURCE_DIR}/content/m4/opengrid-foia"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_opengrid_foia_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_opengrid_foia_ui_e2e COMMAND tst_m4_opengrid_foia_ui_e2e)
set_tests_properties(
    m4_opengrid_foia_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

qt_add_executable(
    tst_m4_serrano_waiver
    integration/tst_m4_serrano_waiver.cpp
)
target_link_libraries(
    tst_m4_serrano_waiver
    PRIVATE
        appellate::application
        appellate::engine
        appellate::packs
        appellate::record_ui
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_serrano_waiver
    PRIVATE
        APPELLATE_M4_SERRANO_ROOT="${PROJECT_SOURCE_DIR}/content/m4/serrano-waiver"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_serrano_waiver PRIVATE cxx_std_26)
add_test(NAME m4_serrano_waiver COMMAND tst_m4_serrano_waiver)
set_tests_properties(
    m4_serrano_waiver
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;offscreen"
)

qt_add_executable(
    tst_m4_serrano_waiver_ui_e2e
    integration/tst_m4_serrano_waiver_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_serrano_waiver_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_serrano_waiver_ui_e2e
    PRIVATE
        APPELLATE_M4_SERRANO_ROOT="${PROJECT_SOURCE_DIR}/content/m4/serrano-waiver"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_serrano_waiver_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_serrano_waiver_ui_e2e COMMAND tst_m4_serrano_waiver_ui_e2e)
set_tests_properties(
    m4_serrano_waiver_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

qt_add_executable(
    tst_m4_cinderlake_writ
    integration/tst_m4_cinderlake_writ.cpp
)
target_link_libraries(
    tst_m4_cinderlake_writ
    PRIVATE
        appellate::application
        appellate::engine
        appellate::packs
        appellate::record_ui
        appellate::storage
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
)
target_compile_definitions(
    tst_m4_cinderlake_writ
    PRIVATE
        APPELLATE_M4_CINDER_ROOT="${PROJECT_SOURCE_DIR}/content/m4/cinderlake-writ"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_cinderlake_writ PRIVATE cxx_std_26)
add_test(NAME m4_cinderlake_writ COMMAND tst_m4_cinderlake_writ)
set_tests_properties(
    m4_cinderlake_writ
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;offscreen"
)

qt_add_executable(
    tst_m4_cinderlake_writ_ui_e2e
    integration/tst_m4_cinderlake_writ_ui_e2e.cpp
)
target_link_libraries(
    tst_m4_cinderlake_writ_ui_e2e
    PRIVATE
        appellate::desktop_ui
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_m4_cinderlake_writ_ui_e2e
    PRIVATE
        APPELLATE_M4_CINDER_ROOT="${PROJECT_SOURCE_DIR}/content/m4/cinderlake-writ"
        APPELLATE_M4_FOUNDATIONS="${PROJECT_SOURCE_DIR}/content/foundations"
)
target_compile_features(tst_m4_cinderlake_writ_ui_e2e PRIVATE cxx_std_26)
add_test(NAME m4_cinderlake_writ_ui_e2e COMMAND tst_m4_cinderlake_writ_ui_e2e)
set_tests_properties(
    m4_cinderlake_writ_ui_e2e
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "content;integration;local;e2e;ui;offscreen"
)

add_subdirectory(content)

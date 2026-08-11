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
    PRIVATE APPELLATE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
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
target_compile_features(tst_pack_catalog PRIVATE cxx_std_26)
add_test(NAME pack_catalog COMMAND tst_pack_catalog)

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
        appellate::pack_cli
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_pack_cli PRIVATE cxx_std_26)
add_test(NAME pack_cli COMMAND tst_pack_cli)

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
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_definitions(
    tst_gold_case_trace
    PRIVATE APPELLATE_GOLD_PACK="${PROJECT_SOURCE_DIR}/content/ca4-rule54b/pack"
)
target_compile_features(tst_gold_case_trace PRIVATE cxx_std_26)
add_test(NAME gold_case_trace COMMAND tst_gold_case_trace)

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

add_subdirectory(content)

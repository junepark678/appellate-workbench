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

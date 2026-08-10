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

qt_add_executable(tst_sync_protocol_codec tst_protocol_codec.cpp)
target_link_libraries(
    tst_sync_protocol_codec
    PRIVATE
        appellate::sync
        appellate::project_warnings
        appellate::project_sanitizers
        PkgConfig::Sodium
        Qt6::Test
)
target_compile_features(tst_sync_protocol_codec PRIVATE cxx_std_26)
add_test(NAME sync_protocol_codec COMMAND tst_sync_protocol_codec)
set_tests_properties(sync_protocol_codec PROPERTIES LABELS "sync;local")

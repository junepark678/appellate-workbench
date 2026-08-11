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

qt_add_executable(tst_local_folder_provider tst_local_folder_provider.cpp)
target_link_libraries(
    tst_local_folder_provider
    PRIVATE
        appellate::sync
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Test
)
target_compile_features(tst_local_folder_provider PRIVATE cxx_std_26)
add_test(NAME local_folder_provider COMMAND tst_local_folder_provider)
set_tests_properties(local_folder_provider PROPERTIES LABELS "e2e;sync;local")

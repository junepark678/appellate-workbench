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

qt_add_executable(tst_vault_keyring tst_vault_keyring.cpp)
target_link_libraries(
    tst_vault_keyring
    PRIVATE
        appellate::sync
        appellate::project_warnings
        appellate::project_sanitizers
        PkgConfig::Sodium
        Qt6::Test
)
target_compile_features(tst_vault_keyring PRIVATE cxx_std_26)
add_test(NAME vault_keyring COMMAND tst_vault_keyring)
set_tests_properties(vault_keyring PROPERTIES LABELS "sync;local")

qt_add_executable(tst_recovery_capsule tst_recovery_capsule.cpp)
target_link_libraries(
    tst_recovery_capsule
    PRIVATE
        appellate::sync
        appellate::project_warnings
        appellate::project_sanitizers
        PkgConfig::Sodium
        Qt6::Test
)
target_compile_features(tst_recovery_capsule PRIVATE cxx_std_26)
add_test(NAME recovery_capsule COMMAND tst_recovery_capsule)
set_tests_properties(recovery_capsule PROPERTIES LABELS "sync;local")

qt_add_library(
    appellate_sync STATIC
    src/local_folder_provider.cpp
    src/protocol_codec.cpp
    src/recovery_capsule.cpp
    src/secret_bytes.cpp
    src/secret_store.cpp
    src/vault_keyring.cpp
    include/appellate/sync/local_folder_provider.hpp
    include/appellate/sync/object_provider.hpp
    include/appellate/sync/protocol_codec.hpp
    include/appellate/sync/recovery_capsule.hpp
    include/appellate/sync/secret_bytes.hpp
    include/appellate/sync/secret_store.hpp
    include/appellate/sync/sync_object.hpp
    include/appellate/sync/vault_keyring.hpp
)
add_library(appellate::sync ALIAS appellate_sync)

target_include_directories(
    appellate_sync
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    appellate_sync
    PUBLIC Qt6::Core
    PRIVATE
        appellate::project_warnings
        appellate::project_sanitizers
        PkgConfig::Sodium
)
target_compile_features(appellate_sync PUBLIC cxx_std_26)

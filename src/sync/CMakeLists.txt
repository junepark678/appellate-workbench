qt_add_library(
    appellate_sync STATIC
    src/protocol_codec.cpp
    include/appellate/sync/protocol_codec.hpp
    include/appellate/sync/sync_object.hpp
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

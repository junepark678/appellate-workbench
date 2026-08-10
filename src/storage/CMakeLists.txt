qt_add_library(
    appellate_storage STATIC
    src/asset_store.cpp
    include/appellate/storage/asset_store.hpp
)
add_library(appellate::storage ALIAS appellate_storage)

target_include_directories(
    appellate_storage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    appellate_storage
    PUBLIC Qt6::Core
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_storage PUBLIC cxx_std_26)

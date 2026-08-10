qt_add_library(
    appellate_packs STATIC
    src/pack_reader.cpp
    include/appellate/packs/error.hpp
    include/appellate/packs/pack_reader.hpp
)
add_library(appellate::packs ALIAS appellate_packs)

target_include_directories(
    appellate_packs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    appellate_packs
    PUBLIC appellate::model Qt6::Core
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_packs PUBLIC cxx_std_26)

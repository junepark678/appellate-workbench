qt_add_library(
    appellate_packs STATIC
    src/pack_archive.cpp
    src/pack_catalog.cpp
    src/capability_registry.cpp
    src/pack_reader.cpp
    src/realism_evidence.cpp
    src/realism_evidence.hpp
    src/runtime_pack.cpp
    src/runtime_pack_internal.hpp
    src/schema_validator.cpp
    schemas.qrc
    include/appellate/packs/error.hpp
    include/appellate/packs/capability_registry.hpp
    include/appellate/packs/pack_archive.hpp
    include/appellate/packs/pack_catalog.hpp
    include/appellate/packs/pack_reader.hpp
    include/appellate/packs/resolved_pack.hpp
    include/appellate/packs/runtime_pack.hpp
    include/appellate/packs/schema_validator.hpp
)
add_library(appellate::packs ALIAS appellate_packs)
set_target_properties(appellate_packs PROPERTIES AUTORCC ON)

target_include_directories(
    appellate_packs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    appellate_packs
    PUBLIC appellate::model Qt6::Core Qt6::Sql
    PRIVATE
        appellate::project_warnings
        appellate::project_sanitizers
        appellate::engine
        appellate::storage
        LibArchive::LibArchive
)
target_compile_features(appellate_packs PUBLIC cxx_std_26)

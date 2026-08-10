qt_add_library(
    appellate_pack_cli STATIC
    pack_cli.cpp
    pack_cli.hpp
    templates.qrc
)
add_library(appellate::pack_cli ALIAS appellate_pack_cli)
set_target_properties(appellate_pack_cli PROPERTIES AUTORCC ON)
target_include_directories(
    appellate_pack_cli
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(
    appellate_pack_cli
    PUBLIC appellate::packs Qt6::Core
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_pack_cli PUBLIC cxx_std_26)

qt_add_executable(appellate-pack main.cpp)
target_link_libraries(
    appellate-pack
    PRIVATE
        appellate::pack_cli
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Core
)
target_compile_features(appellate-pack PRIVATE cxx_std_26)

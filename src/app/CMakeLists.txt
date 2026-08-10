qt_add_executable(
    appellate-workbench
    main.cpp
    main_window.cpp
    main_window.hpp
)

target_link_libraries(
    appellate-workbench
    PRIVATE
        appellate::packs
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Widgets
)
target_compile_features(appellate-workbench PRIVATE cxx_std_26)

set_target_properties(
    appellate-workbench
    PROPERTIES
        OUTPUT_NAME "Appellate Workbench"
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
)

qt_add_library(
    appellate_application STATIC
    session_controller.cpp
    session_controller.hpp
)
add_library(appellate::application ALIAS appellate_application)
target_include_directories(
    appellate_application
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(
    appellate_application
    PUBLIC appellate::engine appellate::storage Qt6::Core
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_application PUBLIC cxx_std_26)

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
        appellate::application
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

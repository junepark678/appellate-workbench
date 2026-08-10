qt_add_library(
    appellate_application STATIC
    oral_argument_session_controller.cpp
    oral_argument_session_controller.hpp
    session_controller.cpp
    session_controller.hpp
    workflow_session_controller.cpp
    workflow_session_controller.hpp
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

qt_add_library(
    appellate_bench_profile_ui STATIC
    bench_profile_codec.cpp
    bench_profile_codec.hpp
    bench_profile_editor.cpp
    bench_profile_editor.hpp
)
add_library(appellate::bench_profile_ui ALIAS appellate_bench_profile_ui)
target_include_directories(
    appellate_bench_profile_ui
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(
    appellate_bench_profile_ui
    PUBLIC appellate::model Qt6::Core Qt6::Widgets
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_bench_profile_ui PUBLIC cxx_std_26)

qt_add_library(
    appellate_record_ui STATIC
    installed_record_controller.cpp
    installed_record_controller.hpp
    record_workspace.cpp
    record_workspace.hpp
)
add_library(appellate::record_ui ALIAS appellate_record_ui)
target_include_directories(
    appellate_record_ui
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(
    appellate_record_ui
    PUBLIC appellate::packs Qt6::Core Qt6::Pdf Qt6::PdfWidgets Qt6::Widgets
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_record_ui PUBLIC cxx_std_26)

qt_add_library(
    appellate_desktop_ui STATIC
    main_window.cpp
    main_window.hpp
)
add_library(appellate::desktop_ui ALIAS appellate_desktop_ui)
target_include_directories(
    appellate_desktop_ui
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(
    appellate_desktop_ui
    PUBLIC
        appellate::bench_profile_ui
        appellate::packs
        appellate::record_ui
        Qt6::Widgets
    PRIVATE
        appellate::project_warnings
        appellate::project_sanitizers
)
target_compile_features(appellate_desktop_ui PUBLIC cxx_std_26)

qt_add_executable(appellate-workbench main.cpp)

target_link_libraries(
    appellate-workbench
    PRIVATE
        appellate::application
        appellate::desktop_ui
        appellate::record_ui
        appellate::project_warnings
        appellate::project_sanitizers
)
target_compile_features(appellate-workbench PRIVATE cxx_std_26)

set_target_properties(
    appellate-workbench
    PROPERTIES
        OUTPUT_NAME "Appellate Workbench"
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
)

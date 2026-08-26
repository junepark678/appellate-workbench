qt_add_library(
    appellate_content STATIC
    src/markdown_pdf_renderer.cpp
    src/render_cli.cpp
    include/appellate/content/markdown_pdf_renderer.hpp
    include/appellate/content/render_cli.hpp
)
add_library(appellate::content ALIAS appellate_content)

target_include_directories(
    appellate_content
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    appellate_content
    PUBLIC Qt6::Core Qt6::Gui
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_content PUBLIC cxx_std_26)

qt_add_executable(appellate-render render_main.cpp)
target_link_libraries(
    appellate-render
    PRIVATE
        appellate::content
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Gui
)
target_compile_features(appellate-render PRIVATE cxx_std_26)

qt_add_library(
    appellate_content STATIC
    src/markdown_pdf_renderer.cpp
    include/appellate/content/markdown_pdf_renderer.hpp
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

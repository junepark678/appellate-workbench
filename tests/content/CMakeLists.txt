qt_add_executable(tst_markdown_pdf_renderer tst_markdown_pdf_renderer.cpp)
target_link_libraries(
    tst_markdown_pdf_renderer
    PRIVATE
        appellate::content
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
        Qt6::Test
)
target_compile_features(tst_markdown_pdf_renderer PRIVATE cxx_std_26)
add_test(NAME markdown_pdf_renderer COMMAND tst_markdown_pdf_renderer)
set_tests_properties(markdown_pdf_renderer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(tst_render_cli tst_render_cli.cpp)
target_link_libraries(
    tst_render_cli
    PRIVATE
        appellate::content
        appellate::project_warnings
        appellate::project_sanitizers
        Qt6::Pdf
        Qt6::Test
)
target_compile_features(tst_render_cli PRIVATE cxx_std_26)
add_test(NAME render_cli COMMAND tst_render_cli)
set_tests_properties(render_cli PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

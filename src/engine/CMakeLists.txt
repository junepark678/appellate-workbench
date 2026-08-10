add_library(
    appellate_engine STATIC
    src/business_calendar.cpp
    src/procedure_engine.cpp
    include/appellate/engine/business_calendar.hpp
    include/appellate/engine/error.hpp
    include/appellate/engine/procedure_engine.hpp
)
add_library(appellate::engine ALIAS appellate_engine)

target_include_directories(
    appellate_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    appellate_engine
    PUBLIC appellate::model
    PRIVATE appellate::project_warnings appellate::project_sanitizers
)
target_compile_features(appellate_engine PUBLIC cxx_std_26)

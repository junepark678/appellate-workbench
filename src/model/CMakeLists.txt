add_library(appellate_model INTERFACE)
add_library(appellate::model ALIAS appellate_model)

target_include_directories(
    appellate_model
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(appellate_model INTERFACE cxx_std_26)

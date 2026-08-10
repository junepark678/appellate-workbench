option(APPELLATE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(APPELLATE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(APPELLATE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

add_library(appellate_project_warnings INTERFACE)
add_library(appellate::project_warnings ALIAS appellate_project_warnings)

if(MSVC)
    target_compile_options(
        appellate_project_warnings
        INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8
    )
    if(APPELLATE_WARNINGS_AS_ERRORS)
        target_compile_options(appellate_project_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(
        appellate_project_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Woverloaded-virtual
            -Wformat=2
    )
    if(APPELLATE_WARNINGS_AS_ERRORS)
        target_compile_options(appellate_project_warnings INTERFACE -Werror)
    endif()
endif()

add_library(appellate_project_sanitizers INTERFACE)
add_library(appellate::project_sanitizers ALIAS appellate_project_sanitizers)

if((APPELLATE_ENABLE_ASAN OR APPELLATE_ENABLE_UBSAN) AND NOT MSVC)
    set(_appellate_sanitizers "")
    if(APPELLATE_ENABLE_ASAN)
        list(APPEND _appellate_sanitizers address)
    endif()
    if(APPELLATE_ENABLE_UBSAN)
        list(APPEND _appellate_sanitizers undefined)
    endif()
    list(JOIN _appellate_sanitizers "," _appellate_sanitizer_list)
    target_compile_options(
        appellate_project_sanitizers
        INTERFACE -fsanitize=${_appellate_sanitizer_list} -fno-omit-frame-pointer
    )
    target_link_options(
        appellate_project_sanitizers
        INTERFACE -fsanitize=${_appellate_sanitizer_list}
    )
endif()

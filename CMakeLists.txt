cmake_minimum_required(VERSION 3.30)

project(
    AppellateWorkbench
    VERSION 0.1.0
    DESCRIPTION "A local-first appellate practice simulator"
    LANGUAGES CXX
)

if(NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    message(
        FATAL_ERROR
        "C++26 mode is required; ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} is not recognized as supporting it"
    )
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

find_package(LibArchive REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Pdf PdfWidgets Sql Widgets)
qt_standard_project_setup(REQUIRES 6.8)

option(APPELLATE_ENABLE_SYNC "Build optional encrypted immutable-object sync support" ON)
set(APPELLATE_SYNC_AVAILABLE OFF)
if(APPELLATE_ENABLE_SYNC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(Sodium REQUIRED IMPORTED_TARGET "libsodium>=1.0.14")
    set(APPELLATE_SYNC_AVAILABLE ON)
endif()

include(cmake/ProjectOptions.cmake)
add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

option(
    APPELLATE_ENABLE_LINUX_PACKAGING
    "Configure the relocatable Linux x86_64 pre-MVP archive and its install smoke test"
    OFF
)
if(APPELLATE_ENABLE_LINUX_PACKAGING)
    include(cmake/Packaging.cmake)
endif()

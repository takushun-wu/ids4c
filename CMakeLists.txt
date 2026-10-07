cmake_minimum_required(VERSION 3.20)

project(ids4c VERSION 0.4.0 LANGUAGES CXX)

set(IDS4C_COPYRIGHT "Copyright 2026 Takushun Wu.")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ids4c")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ids4c/version.h"
    @ONLY
)

if(WIN32)
    enable_language(RC)

    function(ids4c_add_windows_version target filename description file_type)
        set(IDS4C_RC_FILENAME "${filename}")
        set(IDS4C_RC_DESCRIPTION "${description}")
        set(IDS4C_RC_FILE_TYPE "${file_type}")
        configure_file(
            "${PROJECT_SOURCE_DIR}/cmake/version.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_version.rc"
            @ONLY
        )
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_version.rc")
    endfunction()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IDS4C_BUILD_GUI "Build the GTKmm desktop application" ON)
option(IDS4C_BUILD_CLI "Build the command-line application" ON)
option(IDS4C_BUILD_PYTHON "Build the pybind11 Python binding module" ON)
option(IDS4C_ENABLE_LTO "Enable link-time optimization for optimized builds" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(IDS4C_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IDS4C_IPO_SUPPORTED OUTPUT IDS4C_IPO_ERROR)

    if(IDS4C_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(WARNING "IPO/LTO is not supported by this toolchain: ${IDS4C_IPO_ERROR}")
    endif()
endif()

add_library(ids4c_core
    src/core/ids4c.cpp
)
add_library(ids4c::core ALIAS ids4c_core)
target_compile_features(ids4c_core PUBLIC cxx_std_17)
target_include_directories(ids4c_core
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/utf8cpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)

target_compile_options(ids4c_core
    PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# 第三方目录只保留 tinyxml2 源码；直接编译，避免其不完整的安装规则参与本项目配置。
add_library(ids4c_tinyxml2
    third_party/tinyxml2/tinyxml2.cpp
)
add_library(ids4c::tinyxml2 ALIAS ids4c_tinyxml2)
target_include_directories(ids4c_tinyxml2 PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/tinyxml2>
)
target_compile_features(ids4c_tinyxml2 PUBLIC cxx_std_17)

target_compile_definitions(ids4c_tinyxml2 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

add_library(ids4c_db
    src/db/idsdb.cpp
    src/db/idsdb_index.cpp
    src/db/idsdb_import.cpp
    src/db/idsdb_iwds.cpp
    src/db/idsdb_sqlite.cpp
)
add_library(ids4c::db ALIAS ids4c_db)
target_link_libraries(ids4c_db PUBLIC ids4c::core PkgConfig::SQLITE3 PRIVATE ids4c::tinyxml2)
target_compile_options(ids4c_db
    PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

if(IDS4C_BUILD_CLI)
    find_package(Boost REQUIRED COMPONENTS program_options filesystem system)
elseif(IDS4C_BUILD_GUI)
    find_package(Boost REQUIRED COMPONENTS filesystem system)
endif()

if(IDS4C_BUILD_CLI)
    add_executable(ids4c_cli
        src/cli/main.cpp
    )
    add_executable(ids4c::cli ALIAS ids4c_cli)
    target_link_libraries(ids4c_cli PRIVATE ids4c::db Boost::program_options Boost::filesystem Boost::system)
    target_compile_features(ids4c_cli PRIVATE cxx_std_17)
    set_target_properties(ids4c_cli PROPERTIES
        OUTPUT_NAME "ids4c-cli"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:ids4c_cli>"
    )

    if(WIN32)
        ids4c_add_windows_version(ids4c_cli "ids4c-cli.exe" "ids4c command-line interface" VFT_APP)
    endif()
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(ids4c_db_tests
        tests/db_tests.cpp
    )
    target_link_libraries(ids4c_db_tests PRIVATE ids4c::db)
    target_compile_features(ids4c_db_tests PRIVATE cxx_std_17)

    set(IDS4C_TEST_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/tests/data")
    file(MAKE_DIRECTORY "${IDS4C_TEST_DATA_DIR}/db")
    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/replace-query.dat"
        "${IDS4C_TEST_DATA_DIR}/db/replace-query.dat"
        COPYONLY
    )

    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/private-extension.dat"
        "${IDS4C_TEST_DATA_DIR}/db/private-extension.dat"
        COPYONLY
    )
    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/private-extension-reimport.dat"
        "${IDS4C_TEST_DATA_DIR}/db/private-extension-reimport.dat"
        COPYONLY
    )
    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/private-extension-invalid.dat"
        "${IDS4C_TEST_DATA_DIR}/db/private-extension-invalid.dat"
        COPYONLY
    )
    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/iwds-srcseparation.xml"
        "${IDS4C_TEST_DATA_DIR}/db/iwds-srcseparation.xml"
        COPYONLY
    )
    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/iwds-levels.xml"
        "${IDS4C_TEST_DATA_DIR}/db/iwds-levels.xml"
        COPYONLY
    )

    configure_file(
        "${PROJECT_SOURCE_DIR}/tests/data/db/iwds-variable-extension.dat"
        "${IDS4C_TEST_DATA_DIR}/db/iwds-variable-extension.dat"
        COPYONLY
    )
    add_test(NAME ids4c_db_tests COMMAND ids4c_db_tests)
    set_tests_properties(ids4c_db_tests PROPERTIES
        WORKING_DIRECTORY "${IDS4C_TEST_DATA_DIR}"
    )

    if(IDS4C_BUILD_CLI)
        add_test(NAME ids4c_cli_version COMMAND ids4c_cli --version)
        set_tests_properties(ids4c_cli_version PROPERTIES
            PASS_REGULAR_EXPRESSION "ids4c-cli ${PROJECT_VERSION}"
        )
    endif()
endif()

if(IDS4C_BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(ids4c_python
        src/python/ids4c_module.cpp
    )
    add_library(ids4c::python ALIAS ids4c_python)
    target_link_libraries(ids4c_python PRIVATE ids4c::core ids4c::db)
    target_compile_features(ids4c_python PRIVATE cxx_std_17)
    target_compile_options(ids4c_python
        PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    set_target_properties(ids4c_python PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ids4c"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(WIN32)
        set_target_properties(ids4c_python PROPERTIES SUFFIX ".pyd")
        ids4c_add_windows_version(ids4c_python "ids4c.pyd" "ids4c Python bindings" VFT_DLL)
    endif()
endif()

if(IDS4C_BUILD_GUI)
    pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-3.0)
    find_package(Boost REQUIRED COMPONENTS filesystem system)

    add_library(ids4c_ui
        src/ui/ui.cpp
    )
    add_library(ids4c::ui ALIAS ids4c_ui)
    target_link_libraries(ids4c_ui
        PUBLIC
        ids4c::core
        ids4c::db
        PkgConfig::GTKMM
        PRIVATE
        Boost::filesystem
        Boost::system
    )
    target_include_directories(ids4c_ui
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/toml11>
    )
    target_compile_options(ids4c_ui
        PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
        $<$<BOOL:${MINGW}>:-mms-bitfields>
    )

    add_executable(ids4c
        src/app/main.cpp
    )
    target_link_libraries(ids4c PRIVATE ids4c::ui)
    set_target_properties(ids4c PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:ids4c>"
    )

    if(WIN32)
        set_target_properties(ids4c PROPERTIES WIN32_EXECUTABLE TRUE)
        ids4c_add_windows_version(ids4c "ids4c.exe" "ids4c graphical interface" VFT_APP)
    endif()
endif()

include(GNUInstallDirs)
set(IDS4C_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/${PROJECT_NAME}")

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/ids4c/version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ids4c"
)
install(TARGETS ids4c_core ids4c_db ids4c_tinyxml2
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(IDS4C_BUILD_CLI)
    install(TARGETS ids4c_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(IDS4C_BUILD_GUI)
    install(TARGETS ids4c_ui ids4c
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

if(IDS4C_BUILD_PYTHON)
    install(TARGETS ids4c_python
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

install(FILES README.md README.zh-CN.md CHANGELOG.md LICENSE NOTICE DESTINATION "${IDS4C_INSTALL_DOCDIR}")
install(DIRECTORY docs/ DESTINATION "${IDS4C_INSTALL_DOCDIR}/docs")

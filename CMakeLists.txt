cmake_minimum_required(VERSION 3.21)

# Force static-MD triplet so the output is a single drop-in DLL.
# (Matches the convention used by Cell Offset Generator Starfield et al.)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(StarfieldPauseLODHold
    VERSION 1.1.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single shared DLL — no exe target
add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/Hook.cpp
    src/MenuWatcher.cpp
    src/Scanner.cpp
    src/AddressLib.cpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

# Diagnostic build: compiles the verbose defaults in, so a tester build needs no
# INI at all. OFF for anything shipped publicly — with it on the log grows by
# roughly 10 MB per four hours of play.
option(SLH_DIAGNOSTIC_BUILD "Compile in verbose diagnostic defaults (tester builds only)" OFF)
if(SLH_DIAGNOSTIC_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SLH_DIAGNOSTIC_BUILD=1)
    message(STATUS "*** DIAGNOSTIC BUILD — verbose logging compiled in, do not ship publicly ***")
endif()

# Dependencies (vcpkg-provided)
find_package(spdlog CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    spdlog::spdlog_header_only
    minhook::minhook
    Version.lib       # GetFileVersionInfo
    Dbghelp.lib
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

# Windows-specific
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE
    UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# /W4, /WX, /Zc:preprocessor, no exceptions on the hot path
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4 /permissive- /Zc:preprocessor /utf-8
        /EHsc
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "StarfieldPauseLODHold"
    SUFFIX ".dll"
)

# Output binary location and post-build deploy copy.
# SFSE plugins live under <Starfield install>\Data\SFSE\Plugins\.
# Override via -DSF_DEPLOY_DIR="C:/Games/Starfield 1.7/Data/SFSE/Plugins"
if(NOT DEFINED SF_DEPLOY_DIR)
    set(SF_DEPLOY_DIR "C:/Games/Starfield 1.7/Data/SFSE/Plugins"
        CACHE PATH "Deploy folder for SFSE plugin DLL")
endif()

# The INI is NOT deployed by default: the mod ships DLL-only, and compiled
# defaults (including SLH_DIAGNOSTIC_BUILD's verbose ones) are the intended
# configuration. An auto-copied INI silently overrides them — that bit us when
# a release INI in the deploy dir masked a diagnostic build's defaults.
option(SLH_DEPLOY_INI "Also copy data/StarfieldPauseLODHold.ini to SF_DEPLOY_DIR" OFF)

set(SLH_DEPLOY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SF_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${SF_DEPLOY_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>")
if(SLH_DEPLOY_INI)
    list(APPEND SLH_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data/StarfieldPauseLODHold.ini"
            "${SF_DEPLOY_DIR}/StarfieldPauseLODHold.ini")
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    ${SLH_DEPLOY_COMMANDS}
    COMMENT "Deployed to ${SF_DEPLOY_DIR}"
)

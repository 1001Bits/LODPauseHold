cmake_minimum_required(VERSION 3.21)

# Force static-MD triplet so the output is a single drop-in DLL.
# (Matches the convention used by Cell Offset Generator Starfield et al.)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(StarfieldPauseLODHold
    VERSION 1.0.0
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

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SF_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${SF_DEPLOY_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/StarfieldPauseLODHold.ini"
        "${SF_DEPLOY_DIR}/StarfieldPauseLODHold.ini"
    COMMENT "Deployed to ${SF_DEPLOY_DIR}"
)

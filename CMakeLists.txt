cmake_minimum_required(VERSION 3.29)

project(Kessoku CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pin MSVC runtime library to match vcpkg x64-windows default (dynamic CRT).
# CMP0091 is NEW by default (cmake_minimum_required 3.29 > policy intro 3.15).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Export compile_commands.json for clangd (advisory only; MSVC is truth).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(src/core)

add_executable(Kessoku WIN32 src/main.cpp)

find_package(FLAC CONFIG REQUIRED)
find_package(TagLib CONFIG REQUIRED)

# WTL is header-only with no CMake config (headers live directly in
# vcpkg's include/ dir, e.g. atlapp.h), so locate its include dir.
find_path(WTL_INCLUDE_DIR NAMES atlapp.h REQUIRED)

target_include_directories(Kessoku PRIVATE
    ${WTL_INCLUDE_DIR}
)

target_link_libraries(Kessoku PRIVATE
    kessoku_core
    FLAC::FLAC
    TagLib::TagLib
)

target_compile_options(Kessoku PRIVATE /W4)

target_compile_definitions(Kessoku PRIVATE
    _UNICODE
    UNICODE
)

add_subdirectory(tests)

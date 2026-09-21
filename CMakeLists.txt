cmake_minimum_required(VERSION 3.29)

project(Kessoku CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd (advisory only; MSVC is truth).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake maps CXX_STANDARD 23 to the moving-target /std:c++latest on MSVC, and
# MSVC 19.51 only implements C++23 under preview (full /std:c++23 needs 19.52),
# so pin the preview flag explicitly.
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++23preview")

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
    FLAC::FLAC
    TagLib::TagLib
)

target_compile_options(Kessoku PRIVATE /W4)

target_compile_definitions(Kessoku PRIVATE
    _UNICODE
    UNICODE
)

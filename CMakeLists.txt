cmake_minimum_required(VERSION 3.20)

project(Kessoku CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

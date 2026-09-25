cmake_minimum_required(VERSION 3.29)

project(Kessoku CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pin MSVC runtime library to match vcpkg x64-windows default (dynamic CRT).
# CMP0091 is NEW by default (cmake_minimum_required 3.29 > policy intro 3.15).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Option to mark when building under the asan preset.
option(KESSOKU_ENABLE_ASAN "Building under the asan preset" OFF)

# Copy the ASan runtime DLL next to a target's output when KESSOKU_ENABLE_ASAN
# is ON. Call this for every test executable registered with CTest.
function(kessoku_copy_asan_runtime target)
    if(NOT KESSOKU_ENABLE_ASAN)
        return()
    endif()
    get_filename_component(_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_asan_dll "${_msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${_asan_dll}")
        message(WARNING "ASan runtime DLL not found at ${_asan_dll} — "
                         "verify its actual location on this machine")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_asan_dll}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying ASan runtime DLL for ${target}"
        VERBATIM)
endfunction()

# Export compile_commands.json for clangd (advisory only; MSVC is truth).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

find_package(FLAC CONFIG REQUIRED)
find_package(TagLib CONFIG REQUIRED)

add_subdirectory(src/core)
add_subdirectory(src/audio)

# WTL is header-only with no CMake config (headers live directly in
# vcpkg's include/ dir, e.g. atlapp.h), so locate its include dir.
find_path(WTL_INCLUDE_DIR NAMES atlapp.h REQUIRED)

add_subdirectory(src/library)

add_executable(Kessoku WIN32 src/main.cpp)

target_include_directories(Kessoku PRIVATE
    ${WTL_INCLUDE_DIR}
)

target_link_libraries(Kessoku PRIVATE
    kessoku_core
    kessoku_audio
    kessoku_library
    FLAC::FLAC
    TagLib::TagLib
)

target_compile_options(Kessoku PRIVATE /W4)

target_compile_definitions(Kessoku PRIVATE
    _UNICODE
    UNICODE
)

add_subdirectory(tests)

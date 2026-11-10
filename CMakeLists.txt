cmake_minimum_required(VERSION 3.31)

project(Kessoku VERSION 0.1.0 LANGUAGES CXX)

# C++20 is the newest standard MSVC 19.51 implements without preview mode.
# C++23 remains deferred until MSVC ships final /std:c++23 (see commit 811e4a5).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pin MSVC runtime library to match vcpkg x64-windows default (dynamic CRT).
# CMP0091 is NEW by default (cmake_minimum_required 3.31 > policy intro 3.15).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Shared compile contract for every first-party target. Third-party imported
# targets keep their own flags; these apply only to our sources.
add_library(kessoku_build_options INTERFACE)
target_compile_options(kessoku_build_options INTERFACE
    /W4
    /utf-8
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
)
target_compile_definitions(kessoku_build_options INTERFACE
    _UNICODE
    UNICODE
)

# Release-class configurations are the ones the player runs in normal use.
# This build is tuned for this machine's CPU (AMD Ryzen 7 5700X, Zen 3).
# AVX2 is the highest ISA level that CPU supports, and /arch:AVX2 also lets
# MSVC use FMA and BMI. The rest is MSVC's maximum-speed profile:
#   /O2 /Oi /Ot /Oy /GF /Gy  - speed, intrinsics, fast code, no frame pointer,
#                              string pooling, function-level linking
#   /Ob3                      - more aggressive inlining than /Ob2
#   /Gw /Zc:inline            - let the linker drop unreferenced data/COMDATs
#   /fp:fast                  - safe here: PCM is integer, and the only FP work
#                              is the WASAPI buffer-duration calculation
# Debug and ASan keep their /Od flags for debugging and sanitizer accuracy.
#
# CMake's MSVC defaults already contain /Ob2 (Release) and /Ob1
# (RelWithDebInfo). Remove those weaker switches so /Ob3 below is the only
# inlining setting; otherwise every compile emits D9025 "invalidating /Ob2".
string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "/Ob1" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

foreach(_config IN ITEMS Release RelWithDebInfo)
    target_compile_options(kessoku_build_options INTERFACE
        "$<$<CONFIG:${_config}>:/O2>"
        "$<$<CONFIG:${_config}>:/Ob3>"
        "$<$<CONFIG:${_config}>:/Oi>"
        "$<$<CONFIG:${_config}>:/Ot>"
        "$<$<CONFIG:${_config}>:/Oy>"
        "$<$<CONFIG:${_config}>:/GF>"
        "$<$<CONFIG:${_config}>:/Gy>"
        "$<$<CONFIG:${_config}>:/Gw>"
        "$<$<CONFIG:${_config}>:/Zc:inline>"
        "$<$<CONFIG:${_config}>:/arch:AVX2>"
        "$<$<CONFIG:${_config}>:/fp:fast>"
    )
    # Keep the release link step aggressive and non-incremental. /LTCG is
    # supplied by CMAKE_INTERPROCEDURAL_OPTIMIZATION below; these pin the
    # linker passes that remove unreferenced code and fold identical COMDATs.
    # The explicit /LTCG overrides MSBuild's default /LTCG:incremental, which
    # trades whole-program reoptimization for link speed.
    target_link_options(kessoku_build_options INTERFACE
        "$<$<CONFIG:${_config}>:/INCREMENTAL:NO>"
        "$<$<CONFIG:${_config}>:/OPT:REF>"
        "$<$<CONFIG:${_config}>:/OPT:ICF>"
        "$<$<CONFIG:${_config}>:/LTCG>"
    )
endforeach()

# Whole-program optimization (MSVC /GL + /LTCG) for release-class builds.
# This lets the compiler inline and specialize across the core, audio,
# library, and app translation units, which /O2 cannot do on its own. The
# check keeps configuration working if a future toolchain drops IPO support.
include(CheckIPOSupported)
check_ipo_supported(RESULT kessoku_ipo_supported LANGUAGES CXX)
if(kessoku_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
else()
    message(WARNING "IPO is unavailable; release builds will use /O2 only")
endif()

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
    kessoku_build_options
    kessoku_core
    kessoku_audio
    kessoku_library
    FLAC::FLAC
    TagLib::TagLib
)

add_subdirectory(tests)

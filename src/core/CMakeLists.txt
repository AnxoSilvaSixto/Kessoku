add_library(kessoku_core STATIC
    src/library_root.cpp
)

target_include_directories(kessoku_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_options(kessoku_core PRIVATE /W4)

target_compile_definitions(kessoku_core PRIVATE
    _UNICODE
    UNICODE
)

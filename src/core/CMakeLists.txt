add_library(kessoku_core STATIC
    src/library_root.cpp
    src/utf8.cpp
)

target_include_directories(kessoku_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(kessoku_core PRIVATE
    kessoku_build_options
)

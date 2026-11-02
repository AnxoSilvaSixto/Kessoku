add_library(kessoku_library STATIC
    src/scanner.cpp
    src/metadata.cpp
)

target_include_directories(kessoku_library PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(kessoku_library PUBLIC
    kessoku_core
    TagLib::TagLib
)

target_link_libraries(kessoku_library PRIVATE
    kessoku_build_options
)

add_library(kessoku_audio STATIC
    src/player.cpp
    src/wav_format.cpp
)

target_include_directories(kessoku_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(kessoku_audio PUBLIC
    kessoku_core
)

target_compile_options(kessoku_audio PRIVATE /W4)

target_compile_definitions(kessoku_audio PRIVATE
    _UNICODE
    UNICODE
)

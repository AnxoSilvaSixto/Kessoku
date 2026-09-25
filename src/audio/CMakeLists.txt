add_library(kessoku_audio STATIC
    src/player.cpp
    src/wav_format.cpp
    src/flac_format.cpp
)

target_include_directories(kessoku_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(kessoku_audio PUBLIC
    kessoku_core
)

# Completes the approved libFLAC decision (CLAUDE.md 16.1): Kessoku.exe
# already links FLAC::FLAC; this makes the decoder available to the audio
# engine itself. No new dependency. PRIVATE: no FLAC type appears in the
# public headers.
target_link_libraries(kessoku_audio PRIVATE
    FLAC::FLAC
)

target_compile_options(kessoku_audio PRIVATE /W4)

target_compile_definitions(kessoku_audio PRIVATE
    _UNICODE
    UNICODE
)

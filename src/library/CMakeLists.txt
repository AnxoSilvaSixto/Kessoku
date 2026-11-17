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

# The metadata reader uses the player's WAV validator (2.2): a file is only
# listed when Player::Create would accept it. PRIVATE because no audio type
# appears in the library's public headers.
target_link_libraries(kessoku_library PRIVATE
    kessoku_audio
)

target_link_libraries(kessoku_library PRIVATE
    kessoku_build_options
)

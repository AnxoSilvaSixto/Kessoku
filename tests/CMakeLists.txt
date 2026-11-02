add_library(test_helpers STATIC
    shared/test_helpers.cpp
)

target_include_directories(test_helpers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(test_helpers PRIVATE kessoku_build_options)

add_executable(library_root_tests
    core/library_root_tests.cpp
)

target_include_directories(library_root_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(library_root_tests PRIVATE kessoku_core test_helpers)

target_link_libraries(library_root_tests PRIVATE kessoku_build_options)

add_test(NAME library_root_tests COMMAND library_root_tests)

kessoku_copy_asan_runtime(library_root_tests)

add_executable(utf8_tests
    core/utf8_tests.cpp
)

target_include_directories(utf8_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
)

target_link_libraries(utf8_tests PRIVATE kessoku_core)

target_link_libraries(utf8_tests PRIVATE kessoku_build_options)

add_test(NAME utf8_tests COMMAND utf8_tests)

kessoku_copy_asan_runtime(utf8_tests)

add_executable(scanner_tests
    library/scanner_tests.cpp
)

target_include_directories(scanner_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/library/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(scanner_tests PRIVATE kessoku_core kessoku_library test_helpers)

target_link_libraries(scanner_tests PRIVATE kessoku_build_options)

add_test(NAME scanner_tests COMMAND scanner_tests)

kessoku_copy_asan_runtime(scanner_tests)

add_executable(metadata_tests
    library/metadata_tests.cpp
)

target_include_directories(metadata_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/library/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(metadata_tests PRIVATE kessoku_core kessoku_library test_helpers TagLib::TagLib FLAC::FLAC)

target_link_libraries(metadata_tests PRIVATE kessoku_build_options)

add_test(NAME metadata_tests COMMAND metadata_tests)

kessoku_copy_asan_runtime(metadata_tests)

add_executable(player_tests
    audio/player_tests.cpp
)

target_include_directories(player_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/audio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(player_tests PRIVATE kessoku_core kessoku_audio test_helpers FLAC::FLAC)

target_link_libraries(player_tests PRIVATE kessoku_build_options)

add_test(NAME player_tests COMMAND player_tests)

kessoku_copy_asan_runtime(player_tests)

add_executable(transport_position_tests
    ui/transport_position_tests.cpp
)

target_include_directories(transport_position_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(transport_position_tests PRIVATE kessoku_build_options)

add_test(NAME transport_position_tests COMMAND transport_position_tests)

kessoku_copy_asan_runtime(transport_position_tests)

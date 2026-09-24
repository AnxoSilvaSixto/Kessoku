add_library(test_helpers STATIC
    shared/test_helpers.cpp
)

target_include_directories(test_helpers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_compile_options(test_helpers PRIVATE /W4)

target_compile_definitions(test_helpers PRIVATE
    _UNICODE
    UNICODE
)

add_executable(library_root_tests
    core/library_root_tests.cpp
)

target_include_directories(library_root_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(library_root_tests PRIVATE kessoku_core test_helpers)

target_compile_options(library_root_tests PRIVATE /W4)

target_compile_definitions(library_root_tests PRIVATE
    _UNICODE
    UNICODE
)

add_test(NAME library_root_tests COMMAND library_root_tests)

kessoku_copy_asan_runtime(library_root_tests)

add_executable(scanner_tests
    library/scanner_tests.cpp
)

target_include_directories(scanner_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/library/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

target_link_libraries(scanner_tests PRIVATE kessoku_core kessoku_library test_helpers)

target_compile_options(scanner_tests PRIVATE /W4)

target_compile_definitions(scanner_tests PRIVATE
    _UNICODE
    UNICODE
)

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

target_link_libraries(metadata_tests PRIVATE kessoku_core kessoku_library test_helpers TagLib::TagLib)

target_compile_options(metadata_tests PRIVATE /W4)

target_compile_definitions(metadata_tests PRIVATE
    _UNICODE
    UNICODE
)

add_test(NAME metadata_tests COMMAND metadata_tests)

kessoku_copy_asan_runtime(metadata_tests)

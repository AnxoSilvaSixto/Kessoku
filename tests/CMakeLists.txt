add_executable(library_root_tests
    core/library_root_tests.cpp
)

target_include_directories(library_root_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
)

target_link_libraries(library_root_tests PRIVATE kessoku_core)

target_compile_options(library_root_tests PRIVATE /W4)

target_compile_definitions(library_root_tests PRIVATE
    _UNICODE
    UNICODE
)

add_test(NAME library_root_tests COMMAND library_root_tests)

add_executable(jam2_tests
    jam2_tests.cpp
)

target_link_libraries(jam2_tests PRIVATE jam2_core)

add_test(NAME jam2_help COMMAND jam2 --help)
add_test(NAME jam2_core_tests COMMAND jam2_tests)

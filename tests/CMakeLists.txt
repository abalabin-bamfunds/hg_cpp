find_package(Catch2 3 REQUIRED)

add_executable(
    bg_tests
    polymorphic_value_test.cpp
    symbol_test.cpp
    type_system_test.cpp
    version_test.cpp
)
target_link_libraries(bg_tests PRIVATE bg::bg Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bg_tests)

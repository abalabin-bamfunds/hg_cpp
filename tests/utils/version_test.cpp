#include <bg/bg.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version reports the project version", "[version][coverage-exempt]") {
    CHECK(bg::version() == "0.1.0");
}

#include <bg/bg.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bg;

TEST_CASE("version reports the project version", "[version][coverage-exempt]") {
    CHECK(version() == "0.1.0");
}

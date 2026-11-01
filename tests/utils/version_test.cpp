#include <hg/hg.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;

TEST_CASE("version reports the project version", "[version][coverage-exempt]") {
    CHECK(version() == "0.1.0");
}

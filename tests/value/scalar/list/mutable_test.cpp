#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace bg;

TEST_CASE("a mutable scalar list appends and replaces items",
          "[value][scalar][list][scalar:list][mutable][feature:scalar-collection]") {
    auto state = GlobalState{};
    const auto initial = ScalarValue<List<i64>>{std::array<i64, 1>{1}};
    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);

        const auto items = values.mutable_at<List<i64>>("items");
        items.append(3);
        items.set(1, 2);

        REQUIRE(items.size() == 2);
        CHECK(items[0] == 1);
        CHECK(items[1] == 2);
    });
}

TEST_CASE("a mutable scalar list erases and clears items",
          "[value][scalar][list][scalar:list][mutable][feature:scalar-collection][transition:removal]") {
    auto state = GlobalState{};
    const auto initial = ScalarValue<List<i64>>{std::array<i64, 3>{1, 2, 3}};
    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);

        const auto items = values.mutable_at<List<i64>>("items");
        items.erase(1);

        REQUIRE(items.size() == 2);
        CHECK(items[0] == 1);
        CHECK(items[1] == 3);

        items.clear();
        CHECK(items.empty());
    });
}

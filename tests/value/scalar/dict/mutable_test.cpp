#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>

using namespace bg;

TEST_CASE("a mutable scalar dictionary inserts and replaces entries",
          "[value][scalar][dict][scalar:dict][mutable][feature:scalar-collection][transition:addition]") {
    using Entry = std::pair<i64, i64>;

    auto state = GlobalState{};
    const auto initial = ScalarValue<Dict<i64, i64>>{std::array{Entry{1, 2}}};
    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);

        const auto items = values.mutable_at<Dict<i64, i64>>("items");
        items.insert_or_assign(1, 3);
        items.insert_or_assign(2, 4);

        CHECK(items.size() == 2);
        CHECK(items.at(1) == 3);
        CHECK(items.at(2) == 4);
    });
}

TEST_CASE("a mutable scalar dictionary erases and clears entries",
          "[value][scalar][dict][scalar:dict][mutable][feature:scalar-collection][transition:removal]") {
    using Entry = std::pair<i64, i64>;

    auto state = GlobalState{};
    const auto initial = ScalarValue<Dict<i64, i64>>{std::array{Entry{1, 2}, Entry{3, 4}}};
    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);

        const auto items = values.mutable_at<Dict<i64, i64>>("items");
        CHECK(items.erase(1));
        CHECK_FALSE(items.erase(1));
        CHECK_FALSE(items.contains(1));

        items.clear();
        CHECK(items.empty());
    });
}

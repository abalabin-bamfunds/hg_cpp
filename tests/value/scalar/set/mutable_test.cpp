#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace bg;

TEST_CASE("a mutable scalar set inserts unique items",
          "[value][scalar][set][scalar:set][mutable][feature:scalar-collection][transition:addition]") {
    auto state = GlobalState{};
    const auto initial = ScalarValue<Set<i64>>{std::array<i64, 1>{1}};
    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);

        const auto items = values.mutable_at<Set<i64>>("items");
        CHECK(items.insert(2));
        CHECK_FALSE(items.insert(2));
        CHECK(items.contains(1));
        CHECK(items.contains(2));
        CHECK(items.size() == 2);
    });
}

TEST_CASE("a mutable scalar set erases and clears items",
          "[value][scalar][set][scalar:set][mutable][feature:scalar-collection][transition:removal]") {
    auto state = GlobalState{};
    const auto initial = ScalarValue<Set<i64>>{std::array<i64, 2>{1, 2}};
    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);

        const auto items = values.mutable_at<Set<i64>>("items");
        CHECK(items.erase(1));
        CHECK_FALSE(items.erase(1));
        CHECK_FALSE(items.contains(1));

        items.clear();
        CHECK(items.empty());
    });
}

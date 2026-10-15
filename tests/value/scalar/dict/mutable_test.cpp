#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>

TEST_CASE("a mutable scalar dictionary inserts and replaces entries",
          "[value][scalar][dict][scalar:dict][mutable][feature:scalar-collection][transition:addition]") {
    using Entry = std::pair<bg::i64, bg::i64>;

    auto state = bg::GlobalState{};
    const auto initial = bg::ScalarValue<bg::Dict<bg::i64, bg::i64>>{std::array{Entry{1, 2}}};
    state.write([&](bg::GlobalState::WriteAccess &values) {
        values.set(bg::util::symbol{"items"}, initial);

        const auto items = values.mutable_at<bg::Dict<bg::i64, bg::i64>>("items");
        items.insert_or_assign(1, 3);
        items.insert_or_assign(2, 4);

        CHECK(items.size() == 2);
        CHECK(items.at(1) == 3);
        CHECK(items.at(2) == 4);
    });
}

TEST_CASE("a mutable scalar dictionary erases and clears entries",
          "[value][scalar][dict][scalar:dict][mutable][feature:scalar-collection][transition:removal]") {
    using Entry = std::pair<bg::i64, bg::i64>;

    auto state = bg::GlobalState{};
    const auto initial = bg::ScalarValue<bg::Dict<bg::i64, bg::i64>>{std::array{Entry{1, 2}, Entry{3, 4}}};
    state.write([&](bg::GlobalState::WriteAccess &values) {
        values.set(bg::util::symbol{"items"}, initial);

        const auto items = values.mutable_at<bg::Dict<bg::i64, bg::i64>>("items");
        CHECK(items.erase(1));
        CHECK_FALSE(items.erase(1));
        CHECK_FALSE(items.contains(1));

        items.clear();
        CHECK(items.empty());
    });
}

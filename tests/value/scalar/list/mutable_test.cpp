#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

TEST_CASE("a mutable scalar list appends and replaces items", "[value][scalar][list][mutable]") {
    auto state = bg::GlobalState{};
    const auto initial = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 1>{1}};
    auto values = state.write();
    values.set(bg::util::symbol{"items"}, initial);

    const auto items = values.mutable_at<bg::List<std::int64_t>>("items");
    items.append(3);
    items.set(1, 2);

    REQUIRE(items.size() == 2);
    CHECK(items[0] == 1);
    CHECK(items[1] == 2);
}

TEST_CASE("a mutable scalar list erases and clears items", "[value][scalar][list][mutable]") {
    auto state = bg::GlobalState{};
    const auto initial = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 3>{1, 2, 3}};
    auto values = state.write();
    values.set(bg::util::symbol{"items"}, initial);

    const auto items = values.mutable_at<bg::List<std::int64_t>>("items");
    items.erase(1);

    REQUIRE(items.size() == 2);
    CHECK(items[0] == 1);
    CHECK(items[1] == 3);

    items.clear();
    CHECK(items.empty());
}

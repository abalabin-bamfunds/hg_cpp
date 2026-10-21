#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

using namespace bg;

TEST_CASE("global state owns inserted scalar values",
          "[global_state][scalar][feature:global-state][transition:addition]") {
    auto state = GlobalState{};
    auto source = ScalarValue{std::string_view{"value"}};

    state.write([&](auto &values) { values.set(util::symbol{"result"}, source); });
    source = ScalarValue{std::string_view{"changed"}};

    state.read([](const auto &values) {
        const auto stored = scalar_cast<string>(values.at("result"));
        CHECK(*stored == "value");
    });
}

TEST_CASE("global state replaces values and supports heterogeneous lookup",
          "[global_state][scalar][feature:global-state]") {
    auto state = GlobalState{};
    const auto key = util::symbol{"result"};
    const auto first = ScalarValue{std::string_view{"first"}};
    const auto second = ScalarValue{2};

    state.write([&](auto &values) {
        values.set(key, first);
        values.set(key, second);

        CHECK(values.contains("result"));
        CHECK(*scalar_cast<i64>(values.at(key)) == 2);
        CHECK(values.erase("result"));
        CHECK_FALSE(values.contains(key));
    });
}

TEST_CASE("global state write access mutates collection values",
          "[global_state][scalar][collection][feature:global-state]") {
    auto state = GlobalState{};
    const auto initial = ScalarValue<List<i64>>{std::array<i64, 1>{1}};

    state.write([&](GlobalState::WriteAccess &values) {
        values.set(util::symbol{"items"}, initial);
        values.mutable_at<List<i64>>("items").append(2);
    });

    state.read([](const auto &values) {
        const auto items = scalar_cast<List<i64>>(values.at("items"));
        CHECK(items.size() == 2);
        CHECK(items[1].value() == 2);
    });
}

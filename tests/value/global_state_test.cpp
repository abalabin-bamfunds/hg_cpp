#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

TEST_CASE("global state owns inserted scalar values",
          "[global_state][scalar][feature:global-state][transition:addition]") {
    auto state = bg::GlobalState{};
    auto source = bg::ScalarValue{std::string_view{"value"}};

    state.write([&](auto &values) { values.set(bg::util::symbol{"result"}, source); });
    source = bg::ScalarValue{std::string_view{"changed"}};

    state.read([](const auto &values) {
        const auto stored = bg::scalar_cast<bg::string>(values.at("result"));
        CHECK(*stored == "value");
    });
}

TEST_CASE("global state replaces values and supports heterogeneous lookup",
          "[global_state][scalar][feature:global-state]") {
    auto state = bg::GlobalState{};
    const auto key = bg::util::symbol{"result"};
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{2};

    state.write([&](auto &values) {
        values.set(key, first);
        values.set(key, second);

        CHECK(values.contains("result"));
        CHECK(*bg::scalar_cast<bg::i64>(values.at(key)) == 2);
        CHECK(values.erase("result"));
        CHECK_FALSE(values.contains(key));
    });
}

TEST_CASE("global state write access mutates collection values",
          "[global_state][scalar][collection][feature:global-state]") {
    auto state = bg::GlobalState{};
    const auto initial = bg::ScalarValue<bg::List<bg::i64>>{std::array<bg::i64, 1>{1}};

    state.write([&](bg::GlobalState::WriteAccess &values) {
        values.set(bg::util::symbol{"items"}, initial);
        values.mutable_at<bg::List<bg::i64>>("items").append(2);
    });

    state.read([](const auto &values) {
        const auto items = bg::scalar_cast<bg::List<bg::i64>>(values.at("items"));
        CHECK(items.size() == 2);
        CHECK(items[1].value() == 2);
    });
}

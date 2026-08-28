#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

TEST_CASE("global state owns inserted scalar values", "[global_state][scalar]") {
    auto state = bg::GlobalState{};
    auto source = bg::ScalarValue{std::string_view{"value"}};

    {
        auto values = state.write();
        values.set(bg::util::symbol{"result"}, source);
    }
    source = bg::ScalarValue{std::string_view{"changed"}};

    const auto values = state.read();
    const auto stored = bg::scalar_cast<bg::string>(values.at("result"));
    CHECK(stored->value() == "value");
}

TEST_CASE("global state replaces values and supports heterogeneous lookup", "[global_state][scalar]") {
    auto state = bg::GlobalState{};
    const auto key = bg::util::symbol{"result"};
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{2};

    auto values = state.write();
    values.set(key, first);
    values.set(key, second);

    CHECK(values.contains("result"));
    CHECK(bg::scalar_cast<std::int64_t>(values.at(key))->value() == 2);
    CHECK(values.erase("result"));
    CHECK_FALSE(values.contains(key));
}

TEST_CASE("global state write access mutates collection values", "[global_state][scalar][collection]") {
    auto state = bg::GlobalState{};
    const auto initial = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 1>{1}};

    {
        auto values = state.write();
        values.set(bg::util::symbol{"items"}, initial);
        values.mutable_at<bg::List<std::int64_t>>("items").append(2);
    }

    const auto values = state.read();
    const auto items = bg::scalar_cast<bg::List<std::int64_t>>(values.at("items"));
    CHECK(items.size() == 2);
    CHECK(items[1].value() == 2);
}

#include <bg/global_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <typeinfo>

TEST_CASE("global state owns inserted scalar values", "[global_state][scalar]") {
    auto state = bg::GlobalState{};
    auto source = bg::ScalarValue{std::string_view{"value"}};

    state.set(bg::util::symbol{"result"}, source);
    source = bg::ScalarValue{std::string_view{"changed"}};

    const auto stored = bg::scalar_cast<bg::string>(state.at("result"));
    CHECK(stored->value() == "value");
}

TEST_CASE("global state replaces values and supports heterogeneous lookup", "[global_state][scalar]") {
    auto state = bg::GlobalState{};
    const auto key = bg::util::symbol{"result"};
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{2};

    state.set(key, first);
    state.set(key, second);

    CHECK(state.contains("result"));
    CHECK(bg::scalar_cast<std::int64_t>(state.at(key))->value() == 2);
    CHECK(state.erase("result"));
    CHECK_FALSE(state.contains(key));
}

TEST_CASE("checked scalar casts reject an incompatible value", "[scalar]") {
    const auto value = std::int64_t{42};
    const auto erased = bg::Scalar<>::from<std::int64_t>(value);

    CHECK(bg::scalar_cast<std::int64_t>(erased)->value() == value);
    CHECK(bg::unchecked_scalar_cast<std::int64_t>(erased)->value() == value);
    CHECK_THROWS_AS(bg::scalar_cast<double>(erased), std::bad_cast);
}

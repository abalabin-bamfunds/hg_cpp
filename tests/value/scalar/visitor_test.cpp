#include <bg/scalar/value.hpp>

#include "test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace bg;

TEST_CASE("a scalar visitor identifies a scalar value", "[value][scalar][visitor][feature:visitor][scalar:i64]") {
    const auto value = ScalarValue{42};
    auto called = false;

    value.view().visit(
        [&](const Scalar<i64> *view) {
            called = true;
            CHECK(&view->type() == &int64_type());
        },
        ddv::noop);

    CHECK(called);
}

TEST_CASE("one scalar visitor handles multiple built-in scalar types",
          "[value][scalar][visitor][feature:visitor][scalar:i64][scalar:symbol]") {
    const auto integer = ScalarValue{42};
    const auto name = ScalarValue{symbol{"name"}};
    auto integer_called = false;
    auto symbol_called = false;
    auto visitor = ddv::serial{
        [&](const Scalar<i64> *) { integer_called = true; },
        [&](const Scalar<symbol> *) { symbol_called = true; },
        ddv::noop,
    };

    integer.view().visit(visitor);
    name.view().visit(visitor);

    CHECK(integer_called);
    CHECK(symbol_called);
}

TEST_CASE("a scalar visitor identifies each collection type",
          "[value][scalar][visitor][feature:visitor][scalar:list][scalar:set][scalar:dict]") {
    const auto list = ScalarValue<List<i64>>{std::vector<i64>{1}};
    const auto set = ScalarValue<Set<i64>>{std::vector<i64>{2}};
    const auto dict = ScalarValue<Dict<i64, i64>>{std::vector<std::pair<i64, i64>>{{3, 4}}};
    auto list_called = false;
    auto set_called = false;
    auto dict_called = false;

    list.view().visit(
        [&](const Scalar<List<SCALAR>> *view) {
            list_called = true;
            CHECK(view->size() == 1);
        },
        ddv::noop);
    set.view().visit(
        [&](const Scalar<Set<SCALAR>> *view) {
            set_called = true;
            CHECK(view->size() == 1);
        },
        ddv::noop);
    dict.view().visit(
        [&](const Scalar<Dict<SCALAR, SCALAR>> *view) {
            dict_called = true;
            CHECK(view->size() == 1);
        },
        ddv::noop);

    CHECK(list_called);
    CHECK(set_called);
    CHECK(dict_called);
}

TEST_CASE("a scalar visitor selects every built-in type by its type index",
          "[feature:visitor][scalar:byte][scalar:bytes][scalar:boolean][scalar:i64][scalar:floating-point]"
          "[scalar:evaluation-time][scalar:timedelta][scalar:string][scalar:symbol]") {
    const auto byte = ScalarValue{bg::byte{0x2a}};
    const auto boolean = ScalarValue{true};
    const auto integer = ScalarValue{i64{42}};
    const auto floating_point = ScalarValue{42.0};
    const auto evaluation_time = ScalarValue{MIN_DT + std::chrono::microseconds{42}};
    const auto timedelta = ScalarValue{timedelta_t{42}};
    const auto text = ScalarValue{"value"};
    const auto payload = ScalarValue{std::span<const std::byte>{}};
    const auto name = ScalarValue{symbol{"name"}};

    const auto check_builtin = []<typename T>(const ScalarValue<T> &value) {
        const auto *builtin = type_cast<BuiltinScalarType>(&value.view().type());
        REQUIRE(builtin != nullptr);

        auto selected = false;
        builtin->visit_builtin_type([&]<typename Selected>() {
            selected = std::same_as<Selected, T>;
        });
        CHECK(selected);

        auto visited = false;
        value.view().visit([&](const Scalar<T> *) { visited = true; }, ddv::noop);
        CHECK(visited);
    };

    check_builtin(byte);
    check_builtin(boolean);
    check_builtin(integer);
    check_builtin(floating_point);
    check_builtin(evaluation_time);
    check_builtin(timedelta);
    check_builtin(text);
    check_builtin(payload);
    check_builtin(name);
}

TEST_CASE("a scalar visitor reserves the erased view for extension types",
          "[value][scalar][visitor][feature:visitor][scalar:i64]") {
    const auto value = ScalarValue{42};
    auto typed_called = false;
    auto erased_called = false;

    value.view().visit(
        [&](const Scalar<i64> *view) {
            typed_called = true;
            CHECK(&view->type() == &int64_type());
        },
        [&](const Scalar<> *) { erased_called = true; }, ddv::noop);

    CHECK(typed_called);
    CHECK_FALSE(erased_called);
}

TEST_CASE("an opaque scalar visitor uses the base-view interface",
          "[value][scalar][visitor][feature:visitor][scalar:opaque]") {
    const auto value = ScalarValue<Opaque<int>>{42};
    auto called = false;

    value.view().visit(
        [&](const Scalar<> *view) {
            called = true;
            CHECK(&view->type() == &cpp_scalar_type<Opaque<int>>());
        },
        ddv::noop);

    CHECK(called);
}

TEST_CASE("integer and floating-point aliases use the erased visitor",
          "[feature:visitor][scalar:integer][scalar:floating-point][declaration:concrete]") {
    const auto integer = ScalarValue<int>{42};
    const auto floating_point = ScalarValue<float>{42.0F};

    const auto check_erased = [](const auto &value) {
        CHECK(type_cast<BuiltinScalarType>(&value.view().type()) == nullptr);
        auto erased_called = false;
        auto typed_called = false;
        value.view().visit([&](const Scalar<i64> *) { typed_called = true; },
                           [&](const Scalar<> *view) {
                               erased_called = true;
                               CHECK(&view->type() == &value.view().type());
                           },
                           ddv::noop);
        CHECK(erased_called);
        CHECK_FALSE(typed_called);
    };

    check_erased(integer);
    check_erased(floating_point);
}

TEST_CASE("an external scalar provider uses the erased visitor",
          "[feature:visitor][scalar:generic][declaration:runtime]") {
    const auto extension = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};

    CHECK(type_cast<BuiltinScalarType>(&extension.view().type()) == nullptr);
    auto erased_called = false;
    extension.view().visit(
        [&](const Scalar<> *view) {
            erased_called = true;
            CHECK(&view->type() == &extension.view().type());
        },
        ddv::noop);
    CHECK(erased_called);
}

TEST_CASE("a scalar visitor can ignore an unmatched view",
          "[value][scalar][visitor][feature:visitor][scalar:integer]") {
    const auto value = ScalarValue{42};
    auto called = false;

    value.view().visit([&](const Scalar<List<SCALAR>> *) { called = true; }, ddv::noop);

    CHECK_FALSE(called);
}

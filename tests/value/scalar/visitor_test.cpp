#include <bg/scalar/value.hpp>

#include "test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <utility>
#include <vector>

TEST_CASE("a scalar visitor identifies a scalar value", "[value][scalar][visitor][feature:visitor][scalar:i64]") {
    const auto value = bg::ScalarValue{42};
    auto called = false;

    value.view().visit(
        [&](const bg::Scalar<bg::i64> *view) {
            called = true;
            CHECK(&view->type() == &bg::int64_type());
        },
        ddv::noop);

    CHECK(called);
}

TEST_CASE("one scalar visitor handles multiple built-in scalar types",
          "[value][scalar][visitor][feature:visitor][scalar:i64][scalar:symbol]") {
    const auto integer = bg::ScalarValue{42};
    const auto name = bg::ScalarValue{bg::symbol{"name"}};
    auto integer_called = false;
    auto symbol_called = false;
    auto visitor = ddv::serial{
        [&](const bg::Scalar<bg::i64> *) { integer_called = true; },
        [&](const bg::Scalar<bg::symbol> *) { symbol_called = true; },
        ddv::noop,
    };

    integer.view().visit(visitor);
    name.view().visit(visitor);

    CHECK(integer_called);
    CHECK(symbol_called);
}

TEST_CASE("a scalar visitor identifies each collection type",
          "[value][scalar][visitor][feature:visitor][scalar:list][scalar:set][scalar:dict]") {
    const auto list = bg::ScalarValue<bg::List<bg::i64>>{std::vector<bg::i64>{1}};
    const auto set = bg::ScalarValue<bg::Set<bg::i64>>{std::vector<bg::i64>{2}};
    const auto dict = bg::ScalarValue<bg::Dict<bg::i64, bg::i64>>{
        std::vector<std::pair<bg::i64, bg::i64>>{{3, 4}}};
    auto list_called = false;
    auto set_called = false;
    auto dict_called = false;

    list.view().visit(
        [&](const bg::Scalar<bg::List<bg::SCALAR>> *view) {
            list_called = true;
            CHECK(view->size() == 1);
        },
        ddv::noop);
    set.view().visit(
        [&](const bg::Scalar<bg::Set<bg::SCALAR>> *view) {
            set_called = true;
            CHECK(view->size() == 1);
        },
        ddv::noop);
    dict.view().visit(
        [&](const bg::Scalar<bg::Dict<bg::SCALAR, bg::SCALAR>> *view) {
            dict_called = true;
            CHECK(view->size() == 1);
        },
        ddv::noop);

    CHECK(list_called);
    CHECK(set_called);
    CHECK(dict_called);
}

TEST_CASE("a scalar visitor selects every built-in type by its type index",
          "[feature:visitor][scalar:byte][scalar:boolean][scalar:i64][scalar:floating-point]"
          "[scalar:evaluation-time][scalar:timedelta][scalar:string][scalar:symbol]") {
    const auto byte = bg::ScalarValue{bg::byte{0x2a}};
    const auto boolean = bg::ScalarValue{true};
    const auto integer = bg::ScalarValue{bg::i64{42}};
    const auto floating_point = bg::ScalarValue{42.0};
    const auto evaluation_time = bg::ScalarValue{bg::MIN_DT + std::chrono::microseconds{42}};
    const auto timedelta = bg::ScalarValue{bg::timedelta_t{42}};
    const auto text = bg::ScalarValue{"value"};
    const auto name = bg::ScalarValue{bg::symbol{"name"}};

    const auto check_builtin = []<typename T>(const bg::ScalarValue<T> &value) {
        const auto *builtin = bg::type_cast<bg::BuiltinScalarType>(&value.view().type());
        REQUIRE(builtin != nullptr);

        auto selected = false;
        builtin->visit_builtin_type([&]<typename Selected>() {
            selected = std::same_as<Selected, T>;
        });
        CHECK(selected);

        auto visited = false;
        value.view().visit([&](const bg::Scalar<T> *) { visited = true; }, ddv::noop);
        CHECK(visited);
    };

    check_builtin(byte);
    check_builtin(boolean);
    check_builtin(integer);
    check_builtin(floating_point);
    check_builtin(evaluation_time);
    check_builtin(timedelta);
    check_builtin(text);
    check_builtin(name);
}

TEST_CASE("a scalar visitor reserves the erased view for extension types",
          "[value][scalar][visitor][feature:visitor][scalar:i64]") {
    const auto value = bg::ScalarValue{42};
    auto typed_called = false;
    auto erased_called = false;

    value.view().visit(
        [&](const bg::Scalar<bg::i64> *view) {
            typed_called = true;
            CHECK(&view->type() == &bg::int64_type());
        },
        [&](const bg::Scalar<> *) { erased_called = true; },
        ddv::noop);

    CHECK(typed_called);
    CHECK_FALSE(erased_called);
}

TEST_CASE("an opaque scalar visitor uses the base-view interface",
          "[value][scalar][visitor][feature:visitor][scalar:opaque]") {
    const auto value = bg::ScalarValue<bg::Opaque<int>>{42};
    auto called = false;

    value.view().visit(
        [&](const bg::Scalar<> *view) {
            called = true;
            CHECK(&view->type() == &bg::cpp_scalar_type<bg::Opaque<int>>());
        },
        ddv::noop);

    CHECK(called);
}

TEST_CASE("integer and floating-point aliases use the erased visitor",
          "[feature:visitor][scalar:integer][scalar:floating-point][declaration:concrete]") {
    const auto integer = bg::ScalarValue<int>{42};
    const auto floating_point = bg::ScalarValue<float>{42.0F};

    const auto check_erased = [](const auto &value) {
        CHECK(bg::type_cast<bg::BuiltinScalarType>(&value.view().type()) == nullptr);
        auto erased_called = false;
        auto typed_called = false;
        value.view().visit(
            [&](const bg::Scalar<bg::i64> *) { typed_called = true; },
            [&](const bg::Scalar<> *view) {
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
    const auto extension = bg::ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};

    CHECK(bg::type_cast<bg::BuiltinScalarType>(&extension.view().type()) == nullptr);
    auto erased_called = false;
    extension.view().visit(
        [&](const bg::Scalar<> *view) {
            erased_called = true;
            CHECK(&view->type() == &extension.view().type());
        },
        ddv::noop);
    CHECK(erased_called);
}

TEST_CASE("a scalar visitor can ignore an unmatched view",
          "[value][scalar][visitor][feature:visitor][scalar:integer]") {
    const auto value = bg::ScalarValue{42};
    auto called = false;

    value.view().visit([&](const bg::Scalar<bg::List<bg::SCALAR>> *) { called = true; }, ddv::noop);

    CHECK_FALSE(called);
}

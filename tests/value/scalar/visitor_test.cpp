#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

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
    const auto list = bg::ScalarValue<bg::List<std::int64_t>>{std::vector<std::int64_t>{1}};
    const auto set = bg::ScalarValue<bg::Set<std::int64_t>>{std::vector<std::int64_t>{2}};
    const auto dict = bg::ScalarValue<bg::Dict<std::int64_t, std::int64_t>>{
        std::vector<std::pair<std::int64_t, std::int64_t>>{{3, 4}}};
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

TEST_CASE("a scalar visitor can ignore an unmatched view",
          "[value][scalar][visitor][feature:visitor][scalar:integer]") {
    const auto value = bg::ScalarValue{42};
    auto called = false;

    value.view().visit([&](const bg::Scalar<bg::List<bg::SCALAR>> *) { called = true; }, ddv::noop);

    CHECK_FALSE(called);
}

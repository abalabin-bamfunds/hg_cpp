#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

using namespace bg;

TEST_CASE("built-in scalar aliases expose their C++ representations",
          "[feature:type-system][declaration:concrete][scalar:byte][scalar:i64][scalar:evaluation-time]"
          "[scalar:timedelta][scalar:symbol]") {
    STATIC_CHECK(std::same_as<byte, std::byte>);
    STATIC_CHECK(std::same_as<i64, std::int64_t>);
    STATIC_CHECK(std::same_as<date_time_t, EvaluationTime>);
    STATIC_CHECK(std::same_as<timedelta_t, std::chrono::microseconds>);
    STATIC_CHECK(std::same_as<symbol, util::symbol>);
    STATIC_CHECK(sizeof(Scalar<byte>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(Scalar<timedelta_t>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(ScalarValue<byte>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(ScalarValue<timedelta_t>) == 2 * sizeof(void *));
}

TEST_CASE("built-in scalar aliases retain their semantic type identities",
          "[feature:type-system][declaration:concrete][scalar:byte][scalar:i64][scalar:evaluation-time]"
          "[scalar:timedelta][scalar:symbol]") {
    CHECK(&cpp_scalar_type<byte>() == &byte_type());
    CHECK(&cpp_scalar_type<i64>() == &int64_type());
    CHECK(&cpp_scalar_type<date_time_t>() == &evaluation_time_type());
    CHECK(&cpp_scalar_type<timedelta_t>() == &timedelta_type());
    CHECK(&cpp_scalar_type<symbol>() == &symbol_type());
}

TEST_CASE("built-in scalar types have diagnostic names",
          "[feature:type-system][declaration:concrete][scalar:byte][scalar:i64][scalar:evaluation-time]"
          "[scalar:timedelta]") {
    auto byte_text = std::string{};
    byte_type().format_name(byte_text);
    auto integer_text = std::string{};
    int64_type().format_name(integer_text);
    auto date_time_text = std::string{};
    evaluation_time_type().format_name(date_time_text);
    auto timedelta_text = std::string{};
    timedelta_type().format_name(timedelta_text);

    CHECK(byte_text == "byte");
    CHECK(integer_text == "i64");
    CHECK(date_time_text == "date_time_t");
    CHECK(timedelta_text == "timedelta_t");
}

TEST_CASE("byte scalar values use canonical storage, equality, and hashing",
          "[feature:scalar-api][feature:scalar-storage][scalar:byte][storage:inline]") {
    const auto value = ScalarValue{byte{0x2a}};
    const auto equal = ScalarValue{byte{0x2a}};
    const auto different = ScalarValue{byte{0x2b}};
    const auto &runtime_type = value.runtime_type();

    STATIC_CHECK(std::same_as<decltype(value), const ScalarValue<byte>>);
    CHECK(value.view().value() == byte{0x2a});
    CHECK(value == equal);
    CHECK_FALSE(value == different);
    CHECK(value.hash() == equal.hash());
    CHECK(runtime_type.size() == sizeof(byte));
    CHECK(runtime_type.alignment() == alignof(byte));
    CHECK(runtime_type.supports_byte_copy_initialization());
}

TEST_CASE("timedelta scalar values use canonical storage, equality, and hashing",
          "[feature:scalar-api][feature:scalar-storage][scalar:timedelta][storage:inline]") {
    const auto value = ScalarValue{timedelta_t{42}};
    const auto equal = ScalarValue{timedelta_t{42}};
    const auto different = ScalarValue{timedelta_t{43}};
    const auto &runtime_type = value.runtime_type();

    STATIC_CHECK(std::same_as<decltype(value), const ScalarValue<timedelta_t>>);
    CHECK(value.view().value() == timedelta_t{42});
    CHECK(value == equal);
    CHECK_FALSE(value == different);
    CHECK(value.hash() == equal.hash());
    CHECK(runtime_type.size() == sizeof(timedelta_t));
    CHECK(runtime_type.alignment() == alignof(timedelta_t));
    CHECK(runtime_type.supports_byte_copy_initialization());
}

TEST_CASE("canonical scalar sources retain built-ins and normalize integers to i64",
          "[feature:scalar-api][scalar:byte][scalar:i64][scalar:timedelta][scalar:symbol]") {
    STATIC_CHECK(std::same_as<canonical_scalar_t<byte>, byte>);
    STATIC_CHECK(std::same_as<canonical_scalar_t<int>, i64>);
    STATIC_CHECK(std::same_as<canonical_scalar_t<i64>, i64>);
    STATIC_CHECK(std::same_as<canonical_scalar_t<timedelta_t>, timedelta_t>);
    STATIC_CHECK(std::same_as<canonical_scalar_t<symbol>, symbol>);
    STATIC_CHECK_FALSE(scalar_source<char>);
    STATIC_CHECK_FALSE(scalar_source<signed char>);
    STATIC_CHECK_FALSE(scalar_source<unsigned char>);
    STATIC_CHECK_FALSE(scalar_source<std::uint8_t>);
}

TEST_CASE("a default symbol is a safe null value distinct from the empty symbol",
          "[feature:scalar-api][scalar:symbol]") {
    const auto null = symbol{};
    const auto empty = symbol{""};
    const auto null_value = ScalarValue{null};

    CHECK_FALSE(null.has_value());
    CHECK(null.text().empty());
    CHECK(null == symbol{});
    CHECK(null != empty);
    CHECK(empty.has_value());
    CHECK(null_value.hash() == ScalarValue{symbol{}}.hash());
    CHECK_FALSE(null_value.equals(ScalarValue{empty}));
}

TEST_CASE("a scalar visitor selects typed built-in leaf views",
          "[feature:visitor][declaration:concrete][scalar:i64][scalar:symbol]") {
    const auto integer = ScalarValue{i64{42}};
    const auto name = ScalarValue{symbol{"answer"}};
    auto integer_visited = false;
    auto symbol_visited = false;

    integer.view().visit(
        [&](const Scalar<i64> *value) {
            integer_visited = true;
            CHECK(value->value() == 42);
        },
        ddv::noop);
    name.view().visit(
        [&](const Scalar<symbol> *value) {
            symbol_visited = true;
            CHECK(value->value().text() == "answer");
        },
        ddv::noop);

    CHECK(integer_visited);
    CHECK(symbol_visited);
}

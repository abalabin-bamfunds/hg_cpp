#include <bg/scalar/value.hpp>
#include <bg/type/type_text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

TEST_CASE("built-in scalar aliases expose their C++ representations",
          "[feature:type-system][declaration:concrete][scalar:byte][scalar:i64][scalar:evaluation-time]"
          "[scalar:timedelta][scalar:symbol]") {
    STATIC_CHECK(std::same_as<bg::byte, std::byte>);
    STATIC_CHECK(std::same_as<bg::i64, std::int64_t>);
    STATIC_CHECK(std::same_as<bg::date_time_t, bg::EvaluationTime>);
    STATIC_CHECK(std::same_as<bg::timedelta_t, std::chrono::microseconds>);
    STATIC_CHECK(std::same_as<bg::symbol, bg::util::symbol>);
    STATIC_CHECK(sizeof(bg::Scalar<bg::byte>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(bg::Scalar<bg::timedelta_t>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(bg::ScalarValue<bg::byte>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(bg::ScalarValue<bg::timedelta_t>) == 2 * sizeof(void *));
}

TEST_CASE("built-in scalar aliases retain their semantic type identities",
          "[feature:type-system][declaration:concrete][scalar:byte][scalar:i64][scalar:evaluation-time]"
          "[scalar:timedelta][scalar:symbol]") {
    CHECK(&bg::cpp_scalar_type<bg::byte>() == &bg::byte_type());
    CHECK(&bg::cpp_scalar_type<bg::i64>() == &bg::int64_type());
    CHECK(&bg::cpp_scalar_type<bg::date_time_t>() == &bg::evaluation_time_type());
    CHECK(&bg::cpp_scalar_type<bg::timedelta_t>() == &bg::timedelta_type());
    CHECK(&bg::cpp_scalar_type<bg::symbol>() == &bg::symbol_type());
}

TEST_CASE("built-in scalar types have diagnostic names",
          "[feature:type-system][declaration:concrete][scalar:byte][scalar:i64][scalar:evaluation-time]"
          "[scalar:timedelta]") {
    auto byte_text = std::string{};
    bg::append_type_text(byte_text, bg::byte_type());
    auto integer_text = std::string{};
    bg::append_type_text(integer_text, bg::int64_type());
    auto date_time_text = std::string{};
    bg::append_type_text(date_time_text, bg::evaluation_time_type());
    auto timedelta_text = std::string{};
    bg::append_type_text(timedelta_text, bg::timedelta_type());

    CHECK(byte_text == "byte");
    CHECK(integer_text == "i64");
    CHECK(date_time_text == "date_time_t");
    CHECK(timedelta_text == "timedelta_t");
}

TEST_CASE("byte scalar values use canonical storage, equality, and hashing",
          "[feature:scalar-api][feature:scalar-storage][scalar:byte][storage:inline]") {
    const auto value = bg::ScalarValue{bg::byte{0x2a}};
    const auto equal = bg::ScalarValue{bg::byte{0x2a}};
    const auto different = bg::ScalarValue{bg::byte{0x2b}};
    const auto &runtime_type = value.runtime_type();

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::byte>>);
    CHECK(value.view().value() == bg::byte{0x2a});
    CHECK(value == equal);
    CHECK_FALSE(value == different);
    CHECK(value.hash() == equal.hash());
    CHECK(runtime_type.size() == sizeof(bg::byte));
    CHECK(runtime_type.alignment() == alignof(bg::byte));
    CHECK(runtime_type.supports_byte_copy_initialization());
}

TEST_CASE("timedelta scalar values use canonical storage, equality, and hashing",
          "[feature:scalar-api][feature:scalar-storage][scalar:timedelta][storage:inline]") {
    const auto value = bg::ScalarValue{bg::timedelta_t{42}};
    const auto equal = bg::ScalarValue{bg::timedelta_t{42}};
    const auto different = bg::ScalarValue{bg::timedelta_t{43}};
    const auto &runtime_type = value.runtime_type();

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::timedelta_t>>);
    CHECK(value.view().value() == bg::timedelta_t{42});
    CHECK(value == equal);
    CHECK_FALSE(value == different);
    CHECK(value.hash() == equal.hash());
    CHECK(runtime_type.size() == sizeof(bg::timedelta_t));
    CHECK(runtime_type.alignment() == alignof(bg::timedelta_t));
    CHECK(runtime_type.supports_byte_copy_initialization());
}

TEST_CASE("canonical scalar sources retain built-ins and normalize integers to i64",
          "[feature:scalar-api][scalar:byte][scalar:i64][scalar:timedelta][scalar:symbol]") {
    STATIC_CHECK(std::same_as<bg::canonical_scalar_t<bg::byte>, bg::byte>);
    STATIC_CHECK(std::same_as<bg::canonical_scalar_t<int>, bg::i64>);
    STATIC_CHECK(std::same_as<bg::canonical_scalar_t<bg::i64>, bg::i64>);
    STATIC_CHECK(std::same_as<bg::canonical_scalar_t<bg::timedelta_t>, bg::timedelta_t>);
    STATIC_CHECK(std::same_as<bg::canonical_scalar_t<bg::symbol>, bg::symbol>);
    STATIC_CHECK_FALSE(bg::scalar_source<char>);
    STATIC_CHECK_FALSE(bg::scalar_source<signed char>);
    STATIC_CHECK_FALSE(bg::scalar_source<unsigned char>);
    STATIC_CHECK_FALSE(bg::scalar_source<std::uint8_t>);
}

TEST_CASE("a scalar visitor selects typed built-in leaf views",
          "[feature:visitor][declaration:concrete][scalar:i64][scalar:symbol]") {
    const auto integer = bg::ScalarValue{bg::i64{42}};
    const auto name = bg::ScalarValue{bg::symbol{"answer"}};
    auto integer_visited = false;
    auto symbol_visited = false;

    integer.view().visit(
        [&](const bg::Scalar<bg::i64> *value) {
            integer_visited = true;
            CHECK(value->value() == 42);
        },
        ddv::noop);
    name.view().visit(
        [&](const bg::Scalar<bg::symbol> *value) {
            symbol_visited = true;
            CHECK(value->value().text() == "answer");
        },
        ddv::noop);

    CHECK(integer_visited);
    CHECK(symbol_visited);
}

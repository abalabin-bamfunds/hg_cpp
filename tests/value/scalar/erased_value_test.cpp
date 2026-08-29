#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <typeinfo>

TEST_CASE("an erased scalar value copies a borrowed value", "[value][scalar][erased]") {
    auto source = std::pmr::string{"value"};
    const auto view = bg::Scalar<bg::SCALAR>::from<bg::string>(source);
    auto value = bg::ScalarValue<>{view};

    source.assign("changed");

    CHECK(&value.input_view().type() == &bg::string_type());

    auto copy = std::pmr::string{};
    value.input_view().assign(&copy, bg::cpp_scalar_storage_representation<bg::string>());
    CHECK(copy == "value");
}

TEST_CASE("an erased scalar value can replace its value", "[value][scalar][erased]") {
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{42};
    auto value = bg::ScalarValue<>{first};

    value = second;

    const auto view = value.view();
    CHECK(bg::scalar_cast<std::int64_t>(view)->value() == 42);
}

TEST_CASE("an erased scalar value can replace its value from a borrowed view", "[value][scalar][erased]") {
    const auto source_value = std::int64_t{42};
    const auto source = bg::Scalar<std::int64_t>::from(source_value);
    const auto initial = bg::ScalarValue{std::string_view{"first"}};
    auto value = bg::ScalarValue<>{initial};

    value = source;

    const auto view = value.view();
    CHECK(bg::scalar_cast<std::int64_t>(view)->value() == source_value);
}

TEST_CASE("scalar handles compare by type and value", "[value][scalar][erased]") {
    const auto left_value = std::int64_t{42};
    const auto equal_value = std::int64_t{42};
    const auto unequal_value = std::int64_t{43};
    const auto double_value = 42.0;
    const auto left = bg::Scalar<std::int64_t>::from(left_value);
    const auto equal = bg::Scalar<std::int64_t>::from(equal_value);
    const auto different_value = bg::Scalar<std::int64_t>::from(unequal_value);
    const auto different_type = bg::Scalar<double>::from(double_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different_value);
    CHECK_FALSE(left == different_type);
    CHECK(left->hash() == equal->hash());
}

TEST_CASE("a constrained scalar variable exposes its required operations", "[value][scalar][erased]") {
    using Key = bg::ScalarVariable<"Key", bg::EqualityComparable, bg::Hashable>;

    const auto left_value = std::int64_t{42};
    const auto equal_value = std::int64_t{42};
    const auto different_value = std::int64_t{43};
    const auto left = bg::Scalar<Key>::from<std::int64_t>(left_value);
    const auto equal = bg::Scalar<Key>::from<std::int64_t>(equal_value);
    const auto different = bg::Scalar<Key>::from<std::int64_t>(different_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left->hash() == equal->hash());
    CHECK(&left->type() == &bg::int64_type());
}

TEST_CASE("checked scalar casts reject an incompatible value", "[value][scalar][erased]") {
    const auto value = std::int64_t{42};
    const auto erased = bg::Scalar<>::from<std::int64_t>(value);

    CHECK(bg::scalar_cast<std::int64_t>(erased)->value() == value);
    CHECK(bg::unchecked_scalar_cast<std::int64_t>(erased)->value() == value);
    CHECK_THROWS_AS(bg::scalar_cast<double>(erased), std::bad_cast);
}

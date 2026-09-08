#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("a scalar list owns its items", "[value][scalar][list][scalar:list][feature:scalar-collection]") {
    auto source = std::vector{1.0F, 2.0F};
    auto value = bg::ScalarValue<bg::List<float>>{source};

    source[0] = 3.0F;

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0].value() == 1.0F);
    CHECK(list[1].value() == 2.0F);
    const auto &type = dynamic_cast<const bg::ListScalarType &>(value.view().type());
    CHECK(&type.item_type() == &bg::float_type());
}

TEST_CASE("a scalar list owns variable-size items",
          "[value][scalar][list][scalar:list][feature:scalar-collection][storage:variable]") {
    auto source_text = std::string{"first"};
    const auto source = std::vector<std::string_view>{source_text, "second"};
    const auto value = bg::ScalarValue<bg::List<bg::string>>{source};

    source_text.assign("changed");

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0].value() == "first");
    CHECK(list[1].value() == "second");
}

TEST_CASE("a scalar list can contain scalar lists", "[value][scalar][list][scalar:list][feature:scalar-collection]") {
    const auto first = std::vector{1.0F, 2.0F};
    const auto second = std::vector{3.0F};
    const auto first_value = bg::ScalarValue<bg::List<float>>{first};
    const auto second_value = bg::ScalarValue<bg::List<float>>{second};
    const auto source = std::vector{first_value.view(), second_value.view()};
    const auto value = bg::ScalarValue<bg::List<bg::List<float>>>{source};

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0] == first_value.view());
    CHECK(list[1] == second_value.view());
}

TEST_CASE("a scalar list can contain evaluation times",
          "[value][scalar][list][scalar:list][feature:scalar-collection][scalar:evaluation-time]") {
    const auto source =
        std::vector{bg::MIN_DT + std::chrono::microseconds{2}, bg::MIN_DT + std::chrono::microseconds{5}};
    const auto value = bg::ScalarValue<bg::List<bg::EvaluationTime>>{source};
    const auto equal = bg::ScalarValue<bg::List<bg::EvaluationTime>>{source};

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0].value() == source[0]);
    CHECK(list[1].value() == source[1]);
    CHECK(value == equal);
    CHECK(value.view().hash() == equal.view().hash());
}

TEST_CASE("a generic scalar list exposes its declared comparison operations",
          "[value][scalar][list][scalar:list][generic][declaration:generic][feature:scalar-collection]"
          "[scalar:generic]") {
    using Item = bg::ScalarVariable<"Item", bg::EqualityComparable, bg::Hashable>;

    const auto first = bg::ScalarValue<bg::List<std::int64_t>>{std::vector<std::int64_t>{1, 2}};
    const auto second = bg::ScalarValue<bg::List<std::int64_t>>{std::vector<std::int64_t>{1, 2}};
    const auto left = bg::Scalar<bg::List<Item>>::from(first);
    const auto right = bg::Scalar<bg::List<Item>>::from(second);

    CHECK(left == right);
    CHECK(left == second);
    CHECK(second == left);
    CHECK(left.hash() == right.hash());
}

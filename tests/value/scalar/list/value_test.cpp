#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bg;

TEST_CASE("a scalar list owns its items", "[value][scalar][list][scalar:list][feature:scalar-collection]") {
    auto source = std::vector{1.0F, 2.0F};
    auto value = ScalarValue<List<float>>{source};

    source[0] = 3.0F;

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0].value() == 1.0F);
    CHECK(list[1].value() == 2.0F);
    const auto &type = dynamic_cast<const ListScalarType &>(value.view().type());
    CHECK(&type.item_type() == &float_type());
}

TEST_CASE("a scalar list owns variable-size items",
          "[value][scalar][list][scalar:list][feature:scalar-collection][storage:variable]") {
    auto source_text = std::string{"first"};
    const auto source = std::vector<std::string_view>{source_text, "second"};
    const auto value = ScalarValue<List<string>>{source};

    source_text.assign("changed");

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0].value() == "first");
    CHECK(list[1].value() == "second");
}

TEST_CASE("a scalar list can contain scalar lists", "[value][scalar][list][scalar:list][feature:scalar-collection]") {
    const auto first = std::vector{1.0F, 2.0F};
    const auto second = std::vector{3.0F};
    const auto first_value = ScalarValue<List<float>>{first};
    const auto second_value = ScalarValue<List<float>>{second};
    const auto source = std::vector{first_value.view(), second_value.view()};
    const auto value = ScalarValue<List<List<float>>>{source};

    const auto list = value.view();
    REQUIRE(list.size() == 2);
    CHECK(list[0] == first_value.view());
    CHECK(list[1] == second_value.view());
}

TEST_CASE("scalar list construction rejects time-series markers",
          "[value][scalar][list][scalar:list][feature:scalar-collection]") {
    CHECK_THROWS_AS((ScalarValue<List<i64>>::from_literal(
                        literal::TimeSeriesLiteral{1, literal::INVALID})),
                    std::invalid_argument);
}

TEST_CASE("a scalar list can contain evaluation times",
          "[value][scalar][list][scalar:list][feature:scalar-collection][scalar:evaluation-time]") {
    const auto source =
        std::vector{MIN_DT + std::chrono::microseconds{2}, MIN_DT + std::chrono::microseconds{5}};
    const auto value = ScalarValue<List<EvaluationTime>>{source};
    const auto equal = ScalarValue<List<EvaluationTime>>{source};

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
    using ITEM = ScalarVariable<"ITEM", EqualityComparable, Hashable>;

    const auto first = ScalarValue<List<i64>>{std::vector<i64>{1, 2}};
    const auto second = ScalarValue<List<i64>>{std::vector<i64>{1, 2}};
    const auto left = Scalar<List<ITEM>>::from(first);
    const auto right = Scalar<List<ITEM>>::from(second);

    CHECK(left == right);
    CHECK(left == second);
    CHECK(second == left);
    CHECK(left.hash() == right.hash());
}

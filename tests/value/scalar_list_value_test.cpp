#include <bg/scalar/list.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/list_scalar_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename T>
concept hashable_scalar_handle = requires(const T &value) { value->hash(); };

template <typename T>
concept equality_comparable_scalar_handle = requires(const T &left, const T &right) { left == right; };

using Key = bg::ScalarVariable<"Key", bg::EqualityComparable, bg::Hashable>;

static_assert(hashable_scalar_handle<bg::Scalar<bg::List<Key>>>);
static_assert(equality_comparable_scalar_handle<bg::Scalar<bg::List<Key>>>);
static_assert(!std::default_initializable<bg::List<bg::SCALAR>>);

} // namespace

TEST_CASE("a scalar list owns its items", "[value][scalar][list]") {
    auto source = std::vector{1.0F, 2.0F};
    auto value = bg::ScalarValue<bg::List<float>>{source};

    source[0] = 3.0F;

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == 1.0F);
    CHECK(value->value()[1] == 2.0F);
    const auto &type = dynamic_cast<const bg::ListScalarType &>(value->type());
    CHECK(&type.item_type() == &bg::float_type());
}

TEST_CASE("a scalar list owns variable-size items", "[value][scalar][list]") {
    auto source_text = std::string{"first"};
    const auto source = std::vector<std::string_view>{source_text, "second"};
    const auto value = bg::ScalarValue<bg::List<bg::string>>{source};

    source_text.assign("changed");

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == "first");
    CHECK(value->value()[1] == "second");
}

TEST_CASE("a scalar list can contain scalar lists", "[value][scalar][list]") {
    const auto first = std::vector{1.0F, 2.0F};
    const auto second = std::vector{3.0F};
    const auto first_value = bg::ScalarValue<bg::List<float>>{first};
    const auto second_value = bg::ScalarValue<bg::List<float>>{second};
    const auto source = std::vector{first_value->value(), second_value->value()};
    const auto value = bg::ScalarValue<bg::List<bg::List<float>>>{source};

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == first_value->value());
    CHECK(value->value()[1] == second_value->value());
}

TEST_CASE("a scalar list can contain evaluation times", "[value][scalar][list]") {
    const auto source =
        std::vector{bg::MIN_DT + std::chrono::microseconds{2}, bg::MIN_DT + std::chrono::microseconds{5}};
    const auto value = bg::ScalarValue<bg::List<bg::EvaluationTime>>{source};
    const auto equal = bg::ScalarValue<bg::List<bg::EvaluationTime>>{source};

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == source[0]);
    CHECK(value->value()[1] == source[1]);
    CHECK(value == equal);
    CHECK(value->hash() == equal->hash());
}

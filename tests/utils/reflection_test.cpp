#include <bg/util/reflection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string_view>

namespace {

struct reflected_values {
    int first;
    float second;
};

[[maybe_unused]] void reflected_function(int) {}

} // namespace

TEST_CASE("aggregate reflection exposes names and member types", "[reflection][coverage-exempt]") {
    STATIC_CHECK(bg::util::aggregate_member_count<reflected_values> == 2);
    STATIC_CHECK(bg::util::aggregate_member_names<reflected_values>[0] == "first");
    STATIC_CHECK(bg::util::aggregate_member_names<reflected_values>[1] == "second");
    STATIC_CHECK(std::same_as<bg::util::aggregate_member_type<0, reflected_values>, int>);
    STATIC_CHECK(std::same_as<bg::util::aggregate_member_type<1, reflected_values>, float>);
    STATIC_CHECK(bg::util::type_name<float>() == "float");
    STATIC_CHECK(bg::util::unqualified_type_name<reflected_values>() == "reflected_values");
    STATIC_CHECK(bg::util::function_name<reflected_function>() == "reflected_function");
}

namespace {

struct wide_node {
    int input_0;
    int input_1;
    int input_2;
    int input_3;
    int input_4;
    int input_5;
    int input_6;
    int input_7;
    int input_8;
    int input_9;
    int input_10;
    int input_11;
    int input_12;
    int input_13;
    int input_14;
    int input_15;
    int output;
};

} // namespace

TEST_CASE("aggregate reflection supports seventeen members", "[reflection][coverage-exempt]") {
    STATIC_CHECK(bg::util::aggregate_member_count<wide_node> == 17);
    STATIC_CHECK(bg::util::aggregate_member_names<wide_node>[16] == "output");
}

#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/format.hpp>
#include <bg/scalar/value.hpp>

#include "test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <string>
#include <utility>
#include <vector>

using namespace bg;

namespace {

void format_provider_scalar(Scalar<> value, fmt::format_context &context) {
    context.advance_to(fmt::format_to(context.out(), "provider({})",
                                      scalar_cast<test_scalar_provider::value>(value).value().get()));
}

} // namespace

TEST_CASE("scalar formatting handles built-ins and native aliases",
          "[feature:scalar-api][scalar:integer][scalar:floating-point][scalar:string]") {
    const auto integer = ScalarValue<i64>{42};
    const auto native_integer = ScalarValue<int>{7};
    const auto floating_point = ScalarValue<double>{3.5};
    const auto native_floating_point = ScalarValue<float>{1.25F};
    const auto text = ScalarValue<string>{"hello"};
    const auto key = ScalarValue<symbol>{symbol{"price"}};
    const auto byte_value = ScalarValue<byte>{byte{3}};

    CHECK(fmt::format("{}", integer.view()) == "42");
    CHECK(fmt::format("{}", native_integer.view()) == "7");
    CHECK(fmt::format("{}", floating_point.view()) == "3.5");
    CHECK(fmt::format("{}", native_floating_point.view()) == "1.25");
    CHECK(fmt::format("{}", text.view()) == "\"hello\"");
    CHECK(fmt::format("{}", key.view()) == "symbol(\"price\")");
    CHECK(fmt::format("{}", byte_value.view()) == "3");
}

TEST_CASE("scalar formatting recursively formats collections",
          "[feature:scalar-collection][scalar:list][scalar:set][scalar:dict]") {
    const auto list = ScalarValue<List<i64>>{std::vector<i64>{1, 2}};
    const auto set = ScalarValue<Set<i64>>{std::vector<i64>{2}};
    const auto dictionary = ScalarValue<Dict<i64, string>>{std::vector<std::pair<i64, std::string_view>>{{1, "one"}}};

    CHECK(fmt::format("{}", list.view()) == "[1, 2]");
    CHECK(fmt::format("{}", set.view()) == "{{2}}");
    CHECK(fmt::format("{}", dictionary.view()) == "{{1: \"one\"}}");
}

TEST_CASE("scalar formatting dispatches extension scalar types",
          "[feature:visitor][scalar:generic][declaration:runtime]") {
    const auto added = ScalarDispatch<ScalarFormatOperations>::instance().add(
        test_scalar_provider::semantic_type_from_provider(), ScalarFormatOperations{&format_provider_scalar});
    REQUIRE(added);

    const auto value = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};
    CHECK(fmt::format("{}", value.view()) == "provider(42)");
}

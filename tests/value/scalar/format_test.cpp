#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/format.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/value.hpp>

#include "test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bg;

namespace {

using FormattedTuple = Tuple<i64, string>;
using FormattedSingletonTuple = Tuple<i64>;

template <template <typename, auto...> typename Field> struct FormattedState {
    Field<bool> active;
    Field<string, optional> label;
};

struct FormattedPrice {
    i64 quantity;
    double value;
};

void format_provider_scalar(Scalar<> value, fmt::format_context &context) {
    context.advance_to(fmt::format_to(context.out(), "provider({})",
                                      scalar_cast<test_scalar_provider::value>(value).value().get()));
}

void format_provider_runtime(Scalar<> value, fmt::format_context &context) {
    context.advance_to(fmt::format_to(context.out(), "runtime-provider({})",
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
    const auto empty_key = ScalarValue<symbol>{symbol{}};
    const auto escaped_text = ScalarValue<string>{"a\n\""};
    const auto date_time = ScalarValue<date_time_t>{
        date_time_t{std::chrono::sys_days{std::chrono::year{2024} / 1 / 2}.time_since_epoch() +
                    std::chrono::hours{3} + std::chrono::minutes{4} + std::chrono::seconds{5}}};
    const auto duration = ScalarValue<timedelta_t>{timedelta_t{42}};
    const auto bytes = ScalarValue<bg::bytes>{std::array{byte{0x00}, byte{0x7f}, byte{0xff}}};

    CHECK(fmt::format("{}", integer.view()) == "42");
    CHECK(fmt::format("{}", native_integer.view()) == "7");
    CHECK(fmt::format("{}", floating_point.view()) == "3.5");
    CHECK(fmt::format("{}", native_floating_point.view()) == "1.25");
    CHECK(fmt::format("{}", text.view()) == "\"hello\"");
    CHECK(fmt::format("{}", key.view()) == "symbol(\"price\")");
    CHECK(fmt::format("{}", empty_key.view()) == "symbol()");
    CHECK(fmt::format("{}", byte_value.view()) == "3");
    CHECK(fmt::format("{}", escaped_text.view()) == "\"a\\n\\\"\"");
    CHECK(fmt::format("{}", date_time.view()) == "2024-01-02T03:04:05.000000Z");
    CHECK(fmt::format("{}", duration.view()) == "42us");
    CHECK(fmt::format("{}", bytes.view()) == "bytes(\"007fff\")");

    auto formatted = std::string{};
    fmt::format_to(std::back_inserter(formatted), "prefix:{}:suffix", bytes.view());
    CHECK(formatted == "prefix:bytes(\"007fff\"):suffix");
}

TEST_CASE("scalar formatting recursively formats collections",
          "[feature:scalar-collection][scalar:list][scalar:set][scalar:dict]") {
    const auto list = ScalarValue<List<i64>>{std::vector<i64>{1, 2}};
    const auto set = ScalarValue<Set<i64>>{std::vector<i64>{2}};
    const auto dictionary = ScalarValue<Dict<i64, string>>{std::vector<std::pair<i64, std::string_view>>{{1, "one"}}};

    CHECK(fmt::format("{}", list.view()) == "[1, 2]");
    CHECK(fmt::format("{}", set.view()) == "{2}");
    CHECK(fmt::format("{}", dictionary.view()) == "{1: \"one\"}");
}

TEST_CASE("scalar formatting handles tuples, collections, and hierarchical values",
          "[feature:scalar-api][scalar:tuple][scalar:compound]") {
    const auto tuple = ScalarValue<FormattedTuple>::from_literal(literal::ScalarLiteral{42, "item"});
    const auto singleton = ScalarValue<FormattedSingletonTuple>::from_literal(literal::ScalarLiteral{42});
    const auto price = ScalarValue{FormattedPrice{.quantity = 7, .value = 3.5}};
    const auto empty_set = ScalarValue<Set<i64>>{std::vector<i64>{}};
    auto memory = allocator{};
    const auto state = bundle<FormattedState>(memory, true);

    CHECK(fmt::format("{}", tuple.view()) == "(42, \"item\")");
    CHECK(fmt::format("{}", singleton.view()) == "(42,)");
    CHECK(fmt::format("{}", price.view()) == "{\"quantity\": 7, \"value\": 3.5}");
    CHECK(fmt::format("{}", empty_set.view()) == "set()");
    CHECK(fmt::format("{}", state.view()) == "{\"active\": true}");
}

TEST_CASE("scalar formatting dispatches extension scalar types",
          "[feature:visitor][scalar:generic][declaration:runtime]") {
    auto &dispatch = ScalarDispatch<ScalarFormatOperations>::instance();
    REQUIRE(dispatch.add(test_scalar_provider::semantic_type_from_provider(),
                         ScalarFormatOperations{&format_provider_scalar}));
    REQUIRE(dispatch.add(test_scalar_provider::preserving_runtime_type(),
                         ScalarFormatOperations{&format_provider_runtime}));
    REQUIRE(dispatch.add(test_scalar_provider::canonicalizing_runtime_type(), ScalarFormatOperations{}));

    const auto value = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};
    CHECK(fmt::format("{}", value.view()) == "runtime-provider(42)");

    const auto storage = test_scalar_provider::storage{43};
    const auto alternate = Scalar<test_scalar_provider::value>::from(
        test_scalar_provider::canonicalizing_runtime_type(), &storage);
    CHECK(fmt::format("{}", alternate) == "provider(43)");
}

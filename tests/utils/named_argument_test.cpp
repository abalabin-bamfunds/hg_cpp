#include <bg/util/named_argument.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string>

namespace {

using namespace bg;
using namespace bg::literals;

TEST_CASE("argument value types remove names and cvref without decaying arrays", "[coverage-exempt]") {
    using NamedReference = details::named_argument<"value", const int &>;
    using NamedArrayReference = details::named_argument<"text", const char (&)[6]>;
    using StringLiteral = decltype("text"_n = "value");

    static_assert(std::same_as<details::argument_value_t<int>, int>);
    static_assert(std::same_as<details::argument_value_t<const int &>, int>);
    static_assert(std::same_as<details::argument_value_t<volatile int &&>, int>);
    static_assert(std::same_as<details::argument_value_t<const NamedReference &>, int>);
    static_assert(std::same_as<details::argument_value_t<NamedReference &&>, int>);
    static_assert(std::same_as<details::argument_value_t<const char (&)[6]>, char[6]>);
    static_assert(std::same_as<details::argument_value_t<NamedArrayReference &&>, char[6]>);
    static_assert(std::same_as<details::argument_value_t<StringLiteral>, std::string>);
}

} // namespace

#include <bg/util/compile_time_string.hpp>
#include <bg/util/symbol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace {

template <bg::util::compile_time_string Text> inline constexpr auto text = Text.view();

static_assert(text<"value"> == std::string_view{"value"});
static_assert(sizeof(bg::util::symbol) == sizeof(void *));

} // namespace

TEST_CASE("symbols with the same text compare equal", "[symbol][coverage-exempt]") {
    using namespace bg::util::literals;

    const auto literal = "value"_sym;
    const bg::util::symbol constructed{"value"};
    const bg::util::symbol different{"other"};

    CHECK(literal == constructed);
    CHECK(literal != different);
    CHECK(literal.text() == "value");
}

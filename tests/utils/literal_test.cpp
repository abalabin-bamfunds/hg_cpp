#include <bg/literal/literal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace bg;

TEST_CASE("literal sequences can reference caller-owned nested storage", "[coverage-exempt]") {
    using Literal = literal::Literal<int>;

    const auto nested_items = std::vector<Literal>{Literal(1), Literal(2)};
    const auto nested = Literal(Literal::sequence_type{nested_items});
    const auto items = std::vector<Literal>{nested, Literal(3)};
    const auto sequence = Literal(Literal::sequence_type{items});

    sequence.visit(
        [](int) { FAIL("the outer literal is a sequence"); },
        [&](Literal::sequence_type values) {
            REQUIRE(values.size() == 2);

            values[0].visit(
                [](int) { FAIL("the first outer item is a sequence"); },
                [](Literal::sequence_type nested_values) {
                    REQUIRE(nested_values.size() == 2);
                    nested_values[0].visit([](int value) { CHECK(value == 1); }, ddv::noop);
                    nested_values[1].visit([](int value) { CHECK(value == 2); }, ddv::noop);
                });

            values[1].visit([](int value) { CHECK(value == 3); }, ddv::noop);
        });
}

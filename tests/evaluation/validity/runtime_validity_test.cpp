#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("a required input delays evaluation until it becomes valid",
          "[evaluation][validity][feature:validity][validity:required][transition:first-tick]"
          "[series:value][scalar:floating-point]") {
    auto evaluated_sums = std::vector<float>{};
    const auto add = node([&](float lhs, float rhs) {
        const auto sum = lhs + rhs;
        evaluated_sums.push_back(sum);
        return sum;
    });

    const auto actual = testing::test_graph(add, {1.0F, literal::SKIP}, {literal::SKIP, 2.0F});

    CHECK(actual.equals({literal::SKIP, 3.0F}));
    CHECK(evaluated_sums == std::vector<float>{3.0F});
}

TEST_CASE("revalidating an active required input schedules normal evaluation",
          "[evaluation][validity][feature:validity][validity:required][transition:revalidation]"
          "[series:value][scalar:floating-point]") {
    auto evaluated_values = std::vector<float>{};
    const auto copy = node([&](float input) {
        evaluated_values.push_back(input);
        return input;
    });
    auto consumed_values = std::vector<float>{};
    const auto consume = node([&](float input) { consumed_values.push_back(input); });

    const auto graph = bg::graph("name"_n = "copy_and_consume", [&](wp<TS<float>> input) {
        const auto copied = copy(input);
        consume(copied);
        return copied;
    });

    const auto actual = testing::test_graph(graph, {4.0F, literal::INVALID, 4.0F});

    CHECK(actual.equals({4.0F, literal::INVALID, 4.0F}));
    CHECK(evaluated_values == std::vector<float>{4.0F, 4.0F});
    CHECK(consumed_values == std::vector<float>{4.0F, 4.0F});
}

namespace {

struct required_pair_definition {
    In<TS<float>> lhs;
    In<TS<float>> rhs;
    Out<TS<float>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(*lhs + *rhs);
    }

    void on_invalidation() const {
        ++invalidation_count;
        output.set(-static_cast<float>(invalidation_count));
    }

    inline static int evaluation_count{};
    inline static int invalidation_count{};
};

} // namespace

TEST_CASE("a node resumes after all required inputs become valid",
          "[evaluation][validity][feature:validity][validity:required][transition:revalidation]"
          "[series:value][scalar:floating-point]") {
    const auto add = node<required_pair_definition>();
    required_pair_definition::evaluation_count = 0;
    required_pair_definition::invalidation_count = 0;
    const auto actual = testing::test_graph(
        add, {1.0F, literal::INVALID, 1.0F, literal::SKIP},
        {2.0F, literal::INVALID, literal::SKIP, 2.0F});

    CHECK(actual.equals({3.0F, -1.0F, literal::SKIP, 3.0F}));
    CHECK(required_pair_definition::evaluation_count == 2);
    CHECK(required_pair_definition::invalidation_count == 1);
}

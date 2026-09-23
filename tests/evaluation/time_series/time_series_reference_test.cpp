#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <vector>

namespace {

struct changing_reference_target {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.set(20);
        } else if (*step == 1) {
            output.invalidate();
        } else {
            output.set(30);
            ++*step;
            return;
        }

        ++*step;
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct reference_pass {
    bg::In<bg::REF<bg::TS<int>>> input;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void evaluate() const {
        output.copy(input);
        ++evaluation_count;
    }

    inline static int evaluation_count{};
};

struct optional_reference_consumer {
    bg::In<bg::TS<int>, bg::optional> input;

    void evaluate() const {}
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed target",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<changing_reference_target>();
    const auto pass = bg::node<reference_pass>();
    reference_pass::evaluation_count = 0;
    const auto graph = [&] { return pass(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20, std::nullopt, 30}));
}

TEST_CASE("a default reference output does not schedule subscribers for the same reference",
          "[evaluation][reference][feature:reference][transition:update][series:value][scalar:integer]"
          "[guarantee:deduplicated][binding:value-to-reference][binding:reference-to-reference]") {
    const auto forward = bg::node([](bg::In<bg::TS<int>>, bg::In<bg::REF<bg::TS<int>>, bg::passive> input,
                                     bg::Out<bg::REF<bg::TS<int>>> output) { output.copy(input); });
    auto evaluations = 0;
    const auto sink = bg::node([&](bg::In<bg::REF<bg::TS<int>>>) { ++evaluations; });
    const auto graph = [&](bg::wp<bg::TS<int>> input) {
        sink(forward(input, input));
        return input;
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
    CHECK(evaluations == 1);
}

TEST_CASE("reference provider access preserves lazy allocation",
          "[evaluation][reference][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<changing_reference_target>();
    const auto pass = bg::node<reference_pass>();
    const auto sink = bg::node<optional_reference_consumer>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        sink(pass(source()));
    }

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(bg::MIN_DT + std::chrono::microseconds{1});
    runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{2});

    auto &root = *static_cast<bg::details::RootReference *>(runtime.node(1).state());
    CHECK(root.subscribers_or_provider.holds<bg::details::DynamicSubscribers>());
    CHECK_FALSE(root.subscribers_or_provider.holds<bg::details::ReferenceProvider>());

    const auto reference = bg::Reference{&root};
    CHECK(bg::details::ReferenceProvider::get({}) == nullptr);
    CHECK(bg::details::ReferenceProvider::get(reference) == nullptr);

    auto &provider = bg::details::ReferenceProvider::require(reference);
    CHECK(bg::details::ReferenceProvider::get(reference) == &provider);

    runtime.stop();
}

namespace {

struct sampled_left_source {
    bg::Out<bg::TS<int>> output;
    void start() const { output.set(10); }
    void evaluate() const {}
};

struct sampled_right_source {
    bg::Out<bg::TS<int>> output;
    void start() const { output.set(20); }
    void evaluate() const {}
};

struct sampled_condition_source {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(0);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct reference_selector {
    bg::In<bg::TS<int>> condition;
    bg::In<bg::REF<bg::TS<int>>, bg::passive, bg::optional> left;
    bg::In<bg::REF<bg::TS<int>>, bg::passive, bg::optional> right;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void evaluate() const { *condition ? output.copy(right) : output.copy(left); }
};

struct sampled_reference_sink {
    bg::In<bg::TS<int>> input;

    void evaluate(bg::EvaluationContext *context) const {
        values.push_back(*input);
        sample_times.push_back(input.last_updated());
        evaluation_times.push_back(context->evaluation_time());
    }

    inline static std::vector<int> values;
    inline static std::vector<bg::EvaluationTime> sample_times;
    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

struct passive_reference_value {
    bg::In<bg::TS<int>, bg::passive, bg::optional> input;
    bg::In<bg::TS<int>> trigger;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("changing a reference samples its target",
          "[evaluation][reference][sampling][feature:reference][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][series:value]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<sampled_right_source>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto sink = bg::node<sampled_reference_sink>();
    sampled_reference_sink::values.clear();
    sampled_reference_sink::sample_times.clear();
    sampled_reference_sink::evaluation_times.clear();

    bg::run_graph([&] { sink(select(condition(), left(), right())); });

    CHECK(sampled_reference_sink::values == std::vector{10, 20});
    CHECK(sampled_reference_sink::sample_times == sampled_reference_sink::evaluation_times);
}

TEST_CASE("a passive dereferenced input observes its current target",
          "[evaluation][reference][activity][feature:reference][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][activity:passive]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<sampled_right_source>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto value = bg::node<passive_reference_value>();
    const auto graph = [&] {
        const auto trigger = condition();
        return value(select(trigger, left(), right()), trigger);
    };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20}));
}

namespace {

struct invalid_reference_target {
    bg::Out<bg::TS<int>> output;
    void evaluate() const {}
};

struct required_reference_forward {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("changing a reference to an invalid target invalidates required consumers",
          "[evaluation][reference][validity][feature:reference][validity:required]"
          "[transition:invalidation][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][series:value]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<invalid_reference_target>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto forward = bg::node<required_reference_forward>();
    const auto graph = [&] { return forward(select(condition(), left(), right())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, std::nullopt}));
}

namespace {

struct referenced_list_source {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSL<bg::TS<int>, 2>> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[0].set(20); }
};

struct list_reference_pass {
    bg::In<bg::REF<bg::TSL<bg::TS<int>, 2>>> input;
    bg::Out<bg::REF<bg::TSL<bg::TS<int>, 2>>> output;

    void evaluate() const { output.copy(input); }
};

struct referenced_list_value {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input[0]); }
};

struct generic_list_reference_pass {
    bg::In<bg::REF<bg::TSL<bg::TS<bg::SCALAR>, 2>>> input;
    bg::Out<bg::REF<bg::TSL<bg::TS<bg::SCALAR>, 2>>> output;

    void evaluate() const { output.copy(input); }
};

struct generic_referenced_list_value {
    bg::In<bg::TSL<bg::TS<bg::SCALAR>, 2>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*bg::scalar_cast<int>(input[0].value_view())); }
};

struct reference_list {
    bg::In<bg::REF<bg::TS<int>>> input;
    bg::Out<bg::TSL<bg::REF<bg::TS<int>>, 1>> output;

    void evaluate() const { output[0].copy(input); }
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed collection item",
          "[evaluation][reference][tsl][feature:reference][series:fixed-tsl][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<referenced_list_source>();
    const auto pass = bg::node<reference_pass>();
    const auto first =
        bg::node([](bg::In<bg::TSL<bg::TS<int>, 2>> input, bg::Out<bg::TS<int>> output) { output.set(*input[0]); });
    const auto graph = [&] { return pass(first(source())); };
    reference_pass::evaluation_count = 0;

    const auto actual = bg::testing::test_graph(graph);

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a fixed collection can pass through a reference",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<referenced_list_source>();
    const auto pass = bg::node<list_reference_pass>();
    const auto value = bg::node<referenced_list_value>();
    const auto graph = [&] { return value(pass(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a generic fixed collection input uses the referenced target runtime_type",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][declaration:generic]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<referenced_list_source>();
    const auto pass = bg::node<generic_list_reference_pass>();
    const auto value = bg::node<generic_referenced_list_value>();
    const auto graph = [&] { return value(pass(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20}));
}

namespace {

struct owned_pair_definition {
    bg::In<bg::TS<int>> first;
    bg::In<bg::TS<int>> second;
    bg::Out<bg::TSL<bg::TS<int>, 2>> output;

    void evaluate() const {
        output[0].set(*first);
        output[1].set(*second);
    }
};

struct pair_reference_selector_definition {
    bg::In<bg::TS<int>> condition;
    bg::In<bg::REF<bg::TSL<bg::TS<int>, 2>>, bg::passive, bg::optional> owned;
    bg::In<bg::REF<bg::TSL<bg::TS<int>, 2>>, bg::passive, bg::optional> free;
    bg::Out<bg::REF<bg::TSL<bg::TS<int>, 2>>> output;

    void evaluate() const { *condition == 0 ? output.copy(owned) : output.copy(free); }
};

struct pair_sum_definition {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

} // namespace

TEST_CASE("a dereferenced list reference selects the current target runtime_type",
          "[evaluation][reference][tsl][free][feature:reference][series:fixed-tsl][representation:free]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto owned_pair = bg::node<owned_pair_definition>();
    const auto select = bg::node<pair_reference_selector_definition>();
    const auto sum = bg::node<pair_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> condition, bg::wp<bg::TS<int>> first_owned,
                           bg::wp<bg::TS<int>> second_owned, bg::wp<bg::TS<int>> first_free_item,
                           bg::wp<bg::TS<int>> second_free_item) {
        const auto owned = owned_pair(first_owned, second_owned);
        const auto free = bg::tsl(first_free_item, second_free_item);
        return sum(select(condition, owned, free));
    };
    const auto condition = std::vector{0, 1, 0};
    const auto first_owned = std::vector{1, 3, 5};
    const auto second_owned = std::vector{2, 4, 6};
    const auto first_free_item = std::vector{10, 20, 30};
    const auto second_free_item = std::vector{20, 40, 60};

    const auto result =
        bg::testing::test_graph(graph, condition, first_owned, second_owned, first_free_item, second_free_item);

    CHECK(result.equals({3, 60, 11}));
}

TEST_CASE("a reference stored in a fixed collection item updates its consumer",
          "[evaluation][reference][tsl][series:fixed-tsl][feature:reference][transition:update]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value]") {
    const auto source = bg::node<changing_reference_target>();
    const auto list = bg::node<reference_list>();
    const auto first = bg::node([](bg::In<bg::TSL<bg::REF<bg::TS<int>>, 1>> input,
                                   bg::Out<bg::REF<bg::TS<int>>> output) { output.copy(input[0]); });
    const auto graph = [&] { return first(list(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 20, std::nullopt, 30}));
}

namespace {

struct invalidating_reference {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<bg::REF<bg::TS<int>>, bg::passive, bg::optional> target;
    bg::State<int> step;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output.copy(target);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.copy(target);
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("reference invalidation unbinds and revalidation resamples consumers",
          "[evaluation][reference][validity][feature:reference][transition:invalidation]"
          "[transition:revalidation][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][series:value]") {
    const auto source = bg::node<sampled_left_source>();
    const auto reference = bg::node<invalidating_reference>();
    const auto graph = [&] { return reference(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, std::nullopt, 10}));
}

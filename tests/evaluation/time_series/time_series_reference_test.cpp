#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <vector>

namespace {

struct changing_reference_target {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(10);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
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
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct reference_pass {
    bg::In<bg::REF<bg::TS<int>>> input;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void evaluate() const {
        output.set(input);
        ++evaluation_count;
    }

    inline static int evaluation_count{};
};

struct optional_reference_sink {
    bg::In<bg::TS<int>, bg::optional> input;

    void evaluate() const { values.push_back(input.valid() ? std::optional{input.value()} : std::nullopt); }

    inline static std::vector<std::optional<int>> values;
};

struct required_reference_sink {
    bg::In<bg::TS<int>> input;

    void evaluate() const { values.push_back(input.value()); }

    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed target", "[evaluation][reference]") {
    const auto source = bg::node<changing_reference_target>();
    const auto pass = bg::node<reference_pass>();
    const auto optional_sink = bg::node<optional_reference_sink>();
    const auto required_sink = bg::node<required_reference_sink>();
    reference_pass::evaluation_count = 0;
    optional_reference_sink::values.clear();
    required_reference_sink::values.clear();

    bg::run_graph([&] {
        const auto reference = pass(source());
        optional_sink(reference);
        required_sink(reference);
    });

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(optional_reference_sink::values == std::vector<std::optional<int>>{10, 20, std::nullopt, 30});
    CHECK(required_reference_sink::values == std::vector{10, 20, 30});
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
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(0);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct reference_selector {
    bg::In<bg::TS<int>> condition;
    bg::In<bg::REF<bg::TS<int>>, bg::passive, bg::optional> left;
    bg::In<bg::REF<bg::TS<int>>, bg::passive, bg::optional> right;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void evaluate() const { condition.value() ? output.set(right) : output.set(left); }
};

struct sampled_reference_sink {
    bg::In<bg::TS<int>> input;

    void evaluate(bg::EvaluationContext *context) const {
        values.push_back(input.value());
        sample_times.push_back(input.last_updated());
        evaluation_times.push_back(context->evaluation_time());
    }

    inline static std::vector<int> values;
    inline static std::vector<bg::EvaluationTime> sample_times;
    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

struct passive_reference_sink {
    bg::In<bg::TS<int>, bg::passive, bg::optional> input;
    bg::In<bg::TS<int>> trigger;

    void evaluate() const { values.push_back(input.value()); }

    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("changing a reference samples its target", "[evaluation][reference][sampling]") {
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

TEST_CASE("a passive dereferenced input observes its current target", "[evaluation][reference][activity]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<sampled_right_source>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto sink = bg::node<passive_reference_sink>();
    passive_reference_sink::values.clear();

    bg::run_graph([&] {
        const auto trigger = condition();
        sink(select(trigger, left(), right()), trigger);
    });

    CHECK(passive_reference_sink::values == std::vector{10, 20});
}

namespace {

struct invalid_reference_target {
    bg::Out<bg::TS<int>> output;
    void evaluate() const {}
};

struct required_reference_forward {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(input.value()); }
};

} // namespace

TEST_CASE("changing a reference to an invalid target invalidates required consumers",
          "[evaluation][reference][validity]") {
    const auto left = bg::node<sampled_left_source>();
    const auto right = bg::node<invalid_reference_target>();
    const auto condition = bg::node<sampled_condition_source>();
    const auto select = bg::node<reference_selector>();
    const auto forward = bg::node<required_reference_forward>();
    const auto sink = bg::node<optional_reference_sink>();
    optional_reference_sink::values.clear();

    bg::run_graph([&] { sink(forward(select(condition(), left(), right()))); });

    CHECK(optional_reference_sink::values == std::vector<std::optional<int>>{10, std::nullopt});
}

namespace {

struct referenced_list_source {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSL<bg::TS<int>, 2>> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[0].set(20); }
};

struct list_reference_pass {
    bg::In<bg::REF<bg::TSL<bg::TS<int>, 2>>> input;
    bg::Out<bg::REF<bg::TSL<bg::TS<int>, 2>>> output;

    void evaluate() const { output.set(input); }
};

struct referenced_list_sink {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;

    void evaluate() const { values.push_back(input[0].value()); }

    inline static std::vector<int> values;
};

struct generic_list_reference_pass {
    bg::In<bg::REF<bg::TSL<bg::TS<bg::SCALAR>, 2>>> input;
    bg::Out<bg::REF<bg::TSL<bg::TS<bg::SCALAR>, 2>>> output;

    void evaluate() const { output.set(input); }
};

struct generic_referenced_list_sink {
    bg::In<bg::TSL<bg::TS<bg::SCALAR>, 2>> input;

    void evaluate() const { values.push_back(bg::scalar_cast<int>(input[0].value_view())->value()); }

    inline static std::vector<int> values;
};

struct reference_list {
    bg::In<bg::REF<bg::TS<int>>> input;
    bg::Out<bg::TSL<bg::REF<bg::TS<int>>, 1>> output;

    void evaluate() const { output[0].set(input); }
};

} // namespace

TEST_CASE("a dereferenced input subscribes directly to a fixed collection item", "[evaluation][reference][tsl]") {
    const auto source = bg::node<referenced_list_source>();
    const auto pass = bg::node<reference_pass>();
    const auto sink = bg::node<required_reference_sink>();
    reference_pass::evaluation_count = 0;
    required_reference_sink::values.clear();

    bg::run_graph([&] { sink(pass(source()[0])); });

    CHECK(reference_pass::evaluation_count == 1);
    CHECK(required_reference_sink::values == std::vector{10, 20});
}

TEST_CASE("a fixed collection can pass through a reference", "[evaluation][reference][tsl]") {
    const auto source = bg::node<referenced_list_source>();
    const auto pass = bg::node<list_reference_pass>();
    const auto sink = bg::node<referenced_list_sink>();
    referenced_list_sink::values.clear();

    bg::run_graph([&] { sink(pass(source())); });

    CHECK(referenced_list_sink::values == std::vector{10, 20});
}

TEST_CASE("a generic fixed collection input uses the referenced target representation",
          "[evaluation][reference][tsl]") {
    const auto source = bg::node<referenced_list_source>();
    const auto pass = bg::node<generic_list_reference_pass>();
    const auto sink = bg::node<generic_referenced_list_sink>();
    generic_referenced_list_sink::values.clear();

    bg::run_graph([&] { sink(pass(source())); });

    CHECK(generic_referenced_list_sink::values == std::vector{10, 20});
}

TEST_CASE("a reference stored in a fixed collection item updates its consumer", "[evaluation][reference][tsl]") {
    const auto source = bg::node<changing_reference_target>();
    const auto list = bg::node<reference_list>();
    const auto sink = bg::node<optional_reference_sink>();
    optional_reference_sink::values.clear();

    bg::run_graph([&] { sink(list(source())[0]); });

    CHECK(optional_reference_sink::values == std::vector<std::optional<int>>{10, 20, std::nullopt, 30});
}

namespace {

struct invalidating_reference {
    static constexpr bool uses_future_scheduling = true;

    bg::In<bg::REF<bg::TS<int>>, bg::passive, bg::optional> target;
    bg::State<int> step;
    bg::Out<bg::REF<bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(target);
        (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.invalidate();
            (void)context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.set(target);
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("reference invalidation unbinds and revalidation resamples consumers", "[evaluation][reference][validity]") {
    const auto source = bg::node<sampled_left_source>();
    const auto reference = bg::node<invalidating_reference>();
    const auto optional_sink = bg::node<optional_reference_sink>();
    const auto required_sink = bg::node<required_reference_sink>();
    optional_reference_sink::values.clear();
    required_reference_sink::values.clear();

    bg::run_graph([&] {
        const auto value = reference(source());
        optional_sink(value);
        required_sink(value);
    });

    CHECK(optional_reference_sink::values == std::vector<std::optional<int>>{10, std::nullopt, 10});
    CHECK(required_reference_sink::values == std::vector{10, 10});
}

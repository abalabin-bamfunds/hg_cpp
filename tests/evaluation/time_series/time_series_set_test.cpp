#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using IntSet = bg::TSS<std::int64_t>;

struct set_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<IntSet> output;

    void start(bg::EvaluationContext *context) const {
        output.clear();
        schedule_next(context);
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output.insert(1);
            output.insert(2);
        } else {
            output.clear();
        }

        if (*step != 2) {
            schedule_next(context);
        }
        ++*step;
    }

  private:
    static void schedule_next(bg::EvaluationContext *context) {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

struct set_sink_definition {
    bg::In<IntSet, bg::optional> input;

    void evaluate() const {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_count = std::size_t{};
        input.for_each_added([&](const auto &) { ++added_count; });
        added.push_back(added_count);

        auto removed_count = std::size_t{};
        input.for_each_removed([&](const auto &) { ++removed_count; });
        removed.push_back(removed_count);
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::size_t> added;
    inline static std::vector<std::size_t> removed;
};

} // namespace

TEST_CASE("a time series set becomes valid on its first empty tick and remains valid", "[evaluation][tss][validity]") {
    const auto source = bg::node<set_source_definition>();
    const auto sink = bg::node<set_sink_definition>();
    set_sink_definition::validity.clear();
    set_sink_definition::sizes.clear();
    set_sink_definition::added.clear();
    set_sink_definition::removed.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(set_sink_definition::validity == std::vector{true, true, true});
    CHECK(set_sink_definition::sizes == std::vector<std::size_t>{0, 2, 0});
    CHECK(set_sink_definition::added == std::vector<std::size_t>{0, 2, 0});
    CHECK(set_sink_definition::removed == std::vector<std::size_t>{0, 0, 2});
}

namespace {

struct invalidating_value_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct set_from_value_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<IntSet> output;

    void evaluate() const { output.insert(input.value()); }
};

} // namespace

TEST_CASE("a time series set preserves its value when a required input becomes invalid",
          "[evaluation][tss][validity]") {
    const auto source = bg::node<invalidating_value_source_definition>();
    const auto set_from_value = bg::node<set_from_value_definition>();
    const auto sink = bg::node<set_sink_definition>();
    set_sink_definition::validity.clear();
    set_sink_definition::sizes.clear();
    set_sink_definition::added.clear();
    set_sink_definition::removed.clear();

    bg::run_graph([&] { sink(set_from_value(source())); });

    CHECK(set_sink_definition::validity == std::vector{true});
    CHECK(set_sink_definition::sizes == std::vector<std::size_t>{1});
}

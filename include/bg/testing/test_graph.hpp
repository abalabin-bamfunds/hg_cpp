#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/list.hpp>
#include <bg/scalar/value.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace bg::testing {
namespace details {

struct Source {
    static constexpr bool uses_future_scheduling = true;

    Scalar<List<SCALAR>> values;
    Scalar<List<EvaluationTime>> times;
    State<std::size_t> next;
    Out<TS<SCALAR>> output;

    void start(EvaluationContext *context) const { publish(context); }
    void evaluate(EvaluationContext *context) const { publish(context); }

  private:
    void publish(EvaluationContext *context) const {
        const auto scheduled_times = times->value();

        if (*next < scheduled_times.size() && scheduled_times[*next] == context->evaluation_time()) {
            output.set_value(values[*next]);
            ++*next;
        }

        if (*next < scheduled_times.size()) {
            (void)context->schedule_at(scheduled_times[*next]);
        }
    }
};

struct Sink {
    In<TS<SCALAR>> input;
    State<std::size_t> next;

    void evaluate(EvaluationContext *context) const {
        const auto key = util::symbol{"test_graph.output." + std::to_string(*next)};
        context->global_state().set(key, input.value_view());
        ++*next;
    }
};

inline const auto source = node<Source>();
inline const auto sink = node<Sink>();

template <scalar_source T> [[nodiscard]] WiringPoint wire_source(std::span<const T> ticks, EvaluationTime start_time) {
    using Value = canonical_scalar_t<T>;

    auto values = std::vector<Value>{};
    auto times = std::vector<EvaluationTime>{};
    values.reserve(ticks.size());
    times.reserve(ticks.size());

    for (auto i = std::size_t{}; i < ticks.size(); ++i) {
        values.push_back(canonical_scalar(ticks[i]));
        times.push_back(start_time + std::chrono::microseconds{i});
    }

    const auto value_list = ScalarValue<List<Value>>{values};
    const auto time_list = ScalarValue<List<EvaluationTime>>{times};

    return source(value_list, time_list);
}

template <scalar_source T>
[[nodiscard]] WiringPoint wire_source(std::span<const std::optional<T>> ticks, EvaluationTime start_time) {
    using Value = canonical_scalar_t<T>;

    auto values = std::vector<Value>{};
    auto times = std::vector<EvaluationTime>{};
    values.reserve(ticks.size());
    times.reserve(ticks.size());

    for (auto i = std::size_t{}; i < ticks.size(); ++i) {
        if (!ticks[i]) {
            continue;
        }

        values.push_back(canonical_scalar(*ticks[i]));
        times.push_back(start_time + std::chrono::microseconds{i});
    }

    const auto value_list = ScalarValue<List<Value>>{values};
    const auto time_list = ScalarValue<List<EvaluationTime>>{times};

    return source(value_list, time_list);
}

} // namespace details

// Each input range defines updates by outer cycle. An empty optional skips its cycle.
// The returned values own each update published by the tested template.
template <std::ranges::contiguous_range... Inputs>
[[nodiscard]] std::vector<ScalarValue<>> test_graph(const WiringTemplate &tested, const Inputs &...inputs) {
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto global_state = GlobalState{};
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        const auto sources =
            std::array<WiringPoint, sizeof...(Inputs)>{details::wire_source(std::span{inputs}, start_time)...};
        const auto output = tested(std::span<const WiringPoint>{sources});

        if (output) {
            details::sink(*output);
        }
    }

    auto compiled = CompiledGraph{wiring};
    auto runtime = RuntimeGraph{compiled, global_state};
    SimulationEvaluationEngine{}.run(runtime, start_time);

    auto result = std::vector<ScalarValue<>>{};
    for (auto i = std::size_t{};; ++i) {
        const auto key = "test_graph.output." + std::to_string(i);
        const auto *value = global_state.find(key);
        if (value == nullptr) {
            break;
        }

        result.emplace_back(global_state.at(key));
    }

    return result;
}

} // namespace bg::testing

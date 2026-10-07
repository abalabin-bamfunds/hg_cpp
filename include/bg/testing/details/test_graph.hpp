#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/testing/test_graph_input.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::testing::details {

struct Source {
    static constexpr bool uses_timed_scheduling = true;

    Scalar<List<SCALAR>> values;
    Scalar<List<EvaluationTime>> times;
    State<std::size_t> next;
    Out<TS<SCALAR>> output;

    void start(EvaluationContext *context) const { publish(context); }
    void evaluate(EvaluationContext *context) const { publish(context); }

  private:
    void publish(EvaluationContext *context) const {
        if (*next < times.size() && times[*next].value() == context->evaluation_time()) {
            output.set_value(values[*next]);
            ++*next;
        }

        if (*next < times.size()) {
            context->schedule_at(times[*next].value());
        }
    }
};

struct CapturedValues {
    EvaluationTime start_time;
    std::vector<std::optional<ScalarValue<>>> values;

    void set(EvaluationTime time, Scalar<SCALAR> value) {
        auto memory = allocator{};
        at(time).emplace(value, memory);
    }
    void reset(EvaluationTime time) { at(time).reset(); }

  private:
    std::optional<ScalarValue<>> &at(EvaluationTime time) {
        const auto index = static_cast<std::size_t>((time - start_time).count());
        if (index >= values.size()) {
            values.resize(index + 1);
        }

        return values[index];
    }
};

struct Sink {
    In<TS<SCALAR>> input;
    Scalar<Opaque<CapturedValues *>> captured;

    void evaluate(EvaluationContext *context) const {
        captured.value()->set(context->evaluation_time(), input.value_view());
    }

    void on_invalidation(EvaluationContext *context) const { captured.value()->reset(context->evaluation_time()); }
};

inline const auto source = node<Source>();
inline const auto sink = node<Sink>();

class RuntimeSource {
  public:
    struct CompiledData {
        std::span<const std::optional<ScalarValue<>>> ticks;
        EvaluationTime start_time;
    };

    using compiled_data_type = CompiledData;
    using state_type = std::size_t;
    using Invocation = NodeInvocation<CompiledData, state_type>;

    RuntimeSource(const TestGraphInput &input, EvaluationTime start_time)
        : input_{input}, start_time_{start_time},
          signature_{util::symbol{"test_source"},
                     {{.name = util::symbol{"runtime_type"},
                       .type = &input.runtime_type.type(),
                       .accepts_absent_scalar = true}},
                     time_series_value_type(input.runtime_type.type(), TimeSeriesGuarantee::ordinary)} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const noexcept { return {input_.ticks, start_time_}; }

    static void start(Invocation invocation) { publish(invocation); }
    static void evaluate(Invocation invocation) { publish(invocation); }

    static constexpr bool uses_timed_scheduling = true;

  private:
    static void publish(Invocation invocation) {
        const auto &data = invocation.compiled_data();
        auto &next = invocation.state();

        if (next < data.ticks.size() && data.ticks[next]) {
            invocation.output().visit([&](Out<TS<SCALAR>> *output) { output->set_value(data.ticks[next]->view()); },
                                      ddv::noop);
        }

        ++next;
        while (next < data.ticks.size() && !data.ticks[next]) {
            ++next;
        }
        if (next < data.ticks.size()) {
            invocation.context().schedule_at(data.start_time + EvaluationTime::duration{next});
        }
    }

    TestGraphInput input_;
    EvaluationTime start_time_;
    Signature signature_;
};

template <typename T> struct optional_value : std::false_type {};
template <typename T> struct optional_value<std::optional<T>> : std::true_type {};

template <typename T> struct canonical_tick {
    using type = canonical_scalar_t<T>;
};

template <typename T> struct canonical_tick<std::optional<T>> {
    using type = canonical_scalar_t<T>;
};

template <typename T> using canonical_tick_t = typename canonical_tick<T>::type;

template <typename TimeSeries> struct test_output_scalar {
    using type = void;
};

template <typename T, auto... Properties> struct test_output_scalar<TS<T, Properties...>> {
    using type = T;
};

template <typename Target, auto... Properties>
struct test_output_scalar<REF<Target, Properties...>> : test_output_scalar<Target> {};

template <typename Scalar, std::ranges::input_range Range>
[[nodiscard]] WiringPoint wire_source(const Range &ticks, EvaluationTime start_time) {
    using Tick = std::remove_cvref_t<std::ranges::range_value_t<Range>>;
    using Value = typename bg::scalar_storage_traits<Scalar>::value_type;
    using StoredValue =
        std::conditional_t<std::same_as<std::remove_cvref_t<Value>, bool>, std::uint8_t, std::remove_cvref_t<Value>>;

    auto values = std::vector<StoredValue>{};
    auto times = std::vector<EvaluationTime>{};
    values.reserve(std::ranges::size(ticks));
    times.reserve(std::ranges::size(ticks));

    auto index = std::size_t{};
    for (const auto &tick : ticks) {
        if constexpr (optional_value<Tick>::value) {
            if (tick) {
                values.emplace_back(static_cast<StoredValue>(*tick));
                times.push_back(start_time + std::chrono::microseconds{index});
            }
        } else {
            values.emplace_back(static_cast<StoredValue>(tick));
            times.push_back(start_time + std::chrono::microseconds{index});
        }

        ++index;
    }

    return source(ScalarValue<List<Scalar>>{values}, ScalarValue<List<EvaluationTime>>{times});
}

template <typename Function> struct test_function {
    using callable = bg::details::callable<std::remove_cvref_t<Function>>;
    using result = std::remove_cvref_t<typename callable::result_type>;
    using output_time_series = typename bg::details::typed_wiring_point<result>::time_series_type;
    using output_scalar = typename test_output_scalar<output_time_series>::type;

    template <std::size_t Index> using argument = std::remove_cvref_t<typename callable::template argument_type<Index>>;

    template <std::size_t Index>
    using input_time_series = typename bg::details::typed_wiring_point<argument<Index>>::time_series_type;

    template <std::size_t Index> using input_scalar = typename input_time_series<Index>::value_type;
};

template <typename Function, typename Inputs, std::size_t... Index>
[[nodiscard]] auto run_function(const Function &function, const ServiceCatalogue *service_catalogue,
                                const Inputs &inputs, std::index_sequence<Index...>) {
    using FunctionInfo = test_function<Function>;
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time, .values = {}};
    captured.values.resize(std::max({std::size_t{1}, std::ranges::size(std::get<Index>(inputs))...}));
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        [[maybe_unused]] const auto sources = std::tuple{
            wire_source<typename FunctionInfo::template input_scalar<Index>>(std::get<Index>(inputs), start_time)...};
        const auto output =
            std::invoke(function, typename FunctionInfo::template argument<Index>{std::get<Index>(sources)}...);
        sink(output, ScalarValue<Opaque<CapturedValues *>>{&captured});
    }

    if (service_catalogue != nullptr) {
        wiring.finalize(*service_catalogue);
    } else {
        wiring.finalize();
    }
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    SimulationEvaluationEngine{}.run(runtime, start_time);

    return std::move(captured.values);
}

template <std::ranges::contiguous_range... Inputs>
[[nodiscard]] auto run_template(const WiringTemplate &tested, const ServiceCatalogue *service_catalogue,
                                const Inputs &...inputs) {
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time, .values = {}};
    captured.values.resize(std::max({std::size_t{1}, std::ranges::size(inputs)...}));
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        const auto sources = std::array<WiringPoint, sizeof...(Inputs)>{
            wire_source<canonical_tick_t<std::ranges::range_value_t<Inputs>>>(inputs, start_time)...};
        const auto output = tested(std::span<const WiringPoint>{sources});

        if (output) {
            sink(*output, ScalarValue<Opaque<CapturedValues *>>{&captured});
        }
    }

    if (service_catalogue != nullptr) {
        wiring.finalize(*service_catalogue);
    } else {
        wiring.finalize();
    }

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    SimulationEvaluationEngine{}.run(runtime, start_time);

    return std::move(captured.values);
}

[[nodiscard]] inline auto run_runtime_template(const WiringTemplate &tested, std::span<const TestGraphInput> inputs) {
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time, .values = {}};
    auto cycle_count = std::size_t{1};
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        auto sources = std::vector<WiringPoint>{};
        sources.reserve(std::ranges::size(inputs));

        for (const auto &input : inputs) {
            cycle_count = std::max(cycle_count, input.ticks.size());

            auto definition = RuntimeSource{input, start_time};
            const auto &source_template = wiring.own_node_template(
                std::make_unique<RuntimeSignatureNodeTemplate<RuntimeSource>>(std::move(definition)));
            const auto arguments = std::array{
                WiringArgument::absent_scalar(input.runtime_type.type(), input.runtime_type),
            };
            sources.push_back(*static_cast<const WiringTemplate &>(source_template)(arguments));
        }

        const auto output = tested(std::span<const WiringPoint>{sources});
        if (output) {
            sink(*output, ScalarValue<Opaque<CapturedValues *>>{&captured});
        }
    }

    captured.values.resize(cycle_count);

    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    SimulationEvaluationEngine{}.run(runtime, start_time);

    return std::move(captured.values);
}

} // namespace bg::testing::details

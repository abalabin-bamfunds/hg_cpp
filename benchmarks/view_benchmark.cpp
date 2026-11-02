#include <hg/evaluation_context.hpp>
#include <hg/graph/compiled_graph.hpp>
#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/runtime_graph.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/node.hpp>
#include <hg/scalar/value.hpp>
#include <hg/time_series/time_series_runtime_type.hpp>
#include <hg/time_series/tsl/tsl.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

using clock_type = std::chrono::steady_clock;
constexpr auto ListSize = std::size_t{16};
constexpr auto SampleCount = std::size_t{7};
using IntList = hg::TSL<hg::TS<hg::i64>, ListSize>;

#if defined(_MSC_VER)
#define HG_BENCHMARK_NOINLINE __declspec(noinline)
#else
#define HG_BENCHMARK_NOINLINE __attribute__((noinline))
#endif

volatile std::size_t benchmark_result;

HG_BENCHMARK_NOINLINE void consume(std::size_t value) noexcept {
    benchmark_result = value;
}

HG_BENCHMARK_NOINLINE std::size_t read_runtime_scalar(const hg::Scalar<hg::SCALAR> &value) noexcept {
    return hg::Scalar<>::from(value).hash();
}

HG_BENCHMARK_NOINLINE std::size_t read_runtime_input(const hg::In<hg::TS_T> &input) noexcept {
    return reinterpret_cast<std::uintptr_t>(input.data()) ^ input.runtime_type().value_size();
}

#undef HG_BENCHMARK_NOINLINE

struct value_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    hg::Out<hg::TS<hg::i64>> output;

    void start(hg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(hg::EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + hg::EvaluationTime::duration{1});
    }
};

template <typename... Inputs> void consume_values(const Inputs &...inputs) noexcept {
    consume((std::size_t{} + ... + static_cast<std::size_t>(inputs.value())));
}

// Each input is a separate aggregate member so the benchmark follows normal aggregate node invocation.
struct value_sink_1 {
    hg::In<hg::TS<hg::i64>> input_0;

    void evaluate() const { consume_values(input_0); }
};

struct value_sink_4 {
    hg::In<hg::TS<hg::i64>> input_0;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_1;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_2;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_3;

    void evaluate() const { consume_values(input_0, input_1, input_2, input_3); }
};

struct value_sink_8 {
    hg::In<hg::TS<hg::i64>> input_0;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_1;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_2;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_3;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_4;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_5;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_6;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_7;

    void evaluate() const {
        consume_values(input_0, input_1, input_2, input_3, input_4, input_5, input_6, input_7);
    }
};

struct value_sink_16 {
    hg::In<hg::TS<hg::i64>> input_0;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_1;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_2;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_3;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_4;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_5;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_6;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_7;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_8;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_9;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_10;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_11;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_12;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_13;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_14;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_15;

    void evaluate() const {
        consume_values(input_0, input_1, input_2, input_3, input_4, input_5, input_6, input_7, input_8, input_9,
                       input_10, input_11, input_12, input_13, input_14, input_15);
    }
};

struct value_sink_32 {
    hg::In<hg::TS<hg::i64>> input_0;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_1;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_2;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_3;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_4;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_5;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_6;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_7;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_8;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_9;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_10;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_11;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_12;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_13;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_14;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_15;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_16;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_17;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_18;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_19;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_20;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_21;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_22;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_23;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_24;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_25;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_26;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_27;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_28;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_29;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_30;
    hg::In<hg::TS<hg::i64>, hg::passive, hg::optional> input_31;

    void evaluate() const {
        consume_values(input_0, input_1, input_2, input_3, input_4, input_5, input_6, input_7, input_8, input_9,
                       input_10, input_11, input_12, input_13, input_14, input_15, input_16, input_17, input_18,
                       input_19, input_20, input_21, input_22, input_23, input_24, input_25, input_26, input_27,
                       input_28, input_29, input_30, input_31);
    }
};

struct list_source_definition {
    hg::Out<IntList> output;

    void start() const {
        for (auto index = std::size_t{}; index < ListSize; ++index) {
            output[index].set(static_cast<hg::i64>(index));
        }
    }

    void evaluate() const {}
};

struct list_sink_definition {
    hg::In<hg::TS<hg::i64>> trigger;
    hg::In<IntList, hg::passive> input;

    void evaluate() const {
        auto sum = static_cast<std::size_t>(trigger.value());
        for (auto index = std::size_t{}; index < ListSize; ++index) {
            sum += static_cast<std::size_t>(input[index].value());
        }
        consume(sum);
    }
};

struct list_baseline_sink_definition {
    hg::In<hg::TS<hg::i64>> trigger;
    hg::In<IntList, hg::passive> input;

    void evaluate() const { consume(static_cast<std::size_t>(trigger.value())); }
};

struct constant_value_source_definition {
    hg::Out<hg::TS<hg::i64>> output;

    void start() const { output.set(1); }

    void evaluate() const {}
};

template <typename Node, typename Point, std::size_t... Index>
void wire_repeated(const Node &node, Point point, std::index_sequence<Index...>) {
    (void)node((static_cast<void>(Index), point)...);
}

template <typename Point, std::size_t... Index>
[[nodiscard]] auto repeated_list(Point point, std::index_sequence<Index...>) {
    return hg::tsl((static_cast<void>(Index), point)...);
}

struct measurement {
    double nanoseconds;
    std::size_t invocation_size;
};

[[nodiscard]] measurement measure(hg::GraphWiring &wiring, std::size_t invocation_size, std::size_t warmup_cycles,
                                  std::size_t measured_cycles) {
    wiring.finalize();
    const auto compiled = hg::CompiledGraph{wiring};
    auto global_state = hg::GlobalState{};
    auto runtime = hg::RuntimeGraph{compiled, global_state};
    auto evaluation_time = hg::MIN_DT + hg::EvaluationTime::duration{1};
    runtime.start(evaluation_time);

    for (auto cycle = std::size_t{}; cycle < warmup_cycles; ++cycle) {
        runtime.evaluate(evaluation_time);
        evaluation_time += hg::EvaluationTime::duration{1};
    }

    auto samples = std::array<double, SampleCount>{};
    for (auto &sample : samples) {
        const auto start = clock_type::now();
        for (auto cycle = std::size_t{}; cycle < measured_cycles; ++cycle) {
            runtime.evaluate(evaluation_time);
            evaluation_time += hg::EvaluationTime::duration{1};
        }
        sample = std::chrono::duration<double, std::nano>{clock_type::now() - start}.count() /
                 static_cast<double>(measured_cycles);
    }

    runtime.stop();
    std::ranges::sort(samples);
    return {samples[samples.size() / 2], invocation_size};
}

template <typename Definition, std::size_t InputCount>
[[nodiscard]] measurement benchmark_value_invocation(std::size_t warmup_cycles, std::size_t measured_cycles) {
    const auto source = hg::node<value_source_definition>();
    const auto sink = hg::node<Definition>();
    auto wiring = hg::GraphWiring{};
    {
        auto context = hg::WiringContext{wiring};
        const auto value = source();
        wire_repeated(sink, value, std::make_index_sequence<InputCount>{});
    }

    return measure(wiring, sizeof(Definition), warmup_cycles, measured_cycles);
}

[[nodiscard]] measurement benchmark_mixed_runtime_invocation(std::size_t warmup_cycles,
                                                              std::size_t measured_cycles) {
    const auto source = hg::node<value_source_definition>();
    const auto sink = hg::node([](hg::Scalar<hg::SCALAR> scalar_0, hg::In<hg::TS_T> input_1,
                                  hg::Scalar<hg::SCALAR> scalar_2, hg::In<hg::TS_T> input_3) {
        consume(read_runtime_scalar(scalar_0) + read_runtime_input(input_1) + read_runtime_scalar(scalar_2) +
                read_runtime_input(input_3));
    });
    auto scalar_0 = hg::ScalarValue<>{std::string{"runtime scalar one"}};
    auto scalar_2 = hg::ScalarValue<>{std::string{"runtime scalar two"}};
    auto wiring = hg::GraphWiring{};
    {
        auto context = hg::WiringContext{wiring};
        const auto value = source();
        (void)sink(std::move(scalar_0), value, std::move(scalar_2), value);
    }

    constexpr auto InvocationSize = 2 * sizeof(hg::Scalar<hg::SCALAR>) + 2 * sizeof(hg::In<hg::TS_T>);
    return measure(wiring, InvocationSize, warmup_cycles, measured_cycles);
}

template <typename Definition>
[[nodiscard]] measurement benchmark_list_access(bool free_list, std::size_t warmup_cycles,
                                                std::size_t measured_cycles) {
    const auto trigger = hg::node<value_source_definition>();
    const auto list = hg::node<list_source_definition>();
    const auto value = hg::node<constant_value_source_definition>();
    const auto sink = hg::node<Definition>();
    auto wiring = hg::GraphWiring{};
    {
        auto context = hg::WiringContext{wiring};
        if (free_list) {
            const auto item = value();
            sink(trigger(), repeated_list(item, std::make_index_sequence<ListSize>{}));
        } else {
            sink(trigger(), list());
        }
    }

    return measure(wiring, sizeof(Definition), warmup_cycles, measured_cycles);
}

void report(std::string_view name, std::string_view shape, measurement result) {
    std::cout << std::left << std::setw(24) << name << std::setw(12) << shape << std::right
              << std::setw(5) << result.invocation_size << " bytes  " << std::fixed << std::setprecision(2)
              << std::setw(9) << result.nanoseconds << " ns/evaluation\n";
}

} // namespace

int main() {
    constexpr auto WarmupCycles = std::size_t{10'000};
    constexpr auto MeasuredCycles = std::size_t{100'000};

    std::cout << "input view: " << sizeof(hg::In<hg::TS<hg::i64>>) << " bytes, output view: "
              << sizeof(hg::Out<hg::TS<hg::i64>>) << " bytes\n";
    report("value invocation", "1 input", benchmark_value_invocation<value_sink_1, 1>(WarmupCycles, MeasuredCycles));
    report("value invocation", "4 inputs", benchmark_value_invocation<value_sink_4, 4>(WarmupCycles, MeasuredCycles));
    report("mixed runtime input", "4 inputs",
           benchmark_mixed_runtime_invocation(WarmupCycles, MeasuredCycles));
    report("value invocation", "8 inputs", benchmark_value_invocation<value_sink_8, 8>(WarmupCycles, MeasuredCycles));
    report("value invocation", "16 inputs",
           benchmark_value_invocation<value_sink_16, 16>(WarmupCycles, MeasuredCycles));
    report("value invocation", "32 inputs",
           benchmark_value_invocation<value_sink_32, 32>(WarmupCycles, MeasuredCycles));
    report("fixed TSL baseline", "16 items",
           benchmark_list_access<list_baseline_sink_definition>(false, WarmupCycles, MeasuredCycles));
    report("fixed TSL access", "16 items",
           benchmark_list_access<list_sink_definition>(false, WarmupCycles, MeasuredCycles));
    report("free TSL baseline", "16 items",
           benchmark_list_access<list_baseline_sink_definition>(true, WarmupCycles, MeasuredCycles));
    report("free TSL access", "16 items",
           benchmark_list_access<list_sink_definition>(true, WarmupCycles, MeasuredCycles));
}

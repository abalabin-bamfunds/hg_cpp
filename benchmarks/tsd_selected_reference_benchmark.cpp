#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <string_view>
#include <vector>

namespace {

using Dictionary = bg::TSD<std::int64_t, bg::TS<bg::i64>>;
using ReferenceDictionary = bg::TSD<std::int64_t, bg::REF<bg::TS<bg::i64>>>;
constexpr auto MaximumKeyCount = std::size_t{128};

struct DictionarySource {
    bg::Out<Dictionary> output;

    void evaluate() const {
        const auto tick = ++ticks;
        for (auto key = std::size_t{}; key < MaximumKeyCount; ++key) {
            output[static_cast<std::int64_t>(key)].set(static_cast<std::int64_t>(tick * MaximumKeyCount + key));
        }
    }

    inline static std::uint64_t ticks{};
};

struct ReferenceDictionaryView {
    bg::In<bg::REF<ReferenceDictionary>> input;
    bg::Out<bg::REF<ReferenceDictionary>> output;

    void evaluate() const { output.copy(input); }
};

struct SelectedItemSink {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const {
        ++evaluations;
        checksum += static_cast<std::uint64_t>(*input);
    }

    inline static std::uint64_t evaluations{};
    inline static std::uint64_t checksum{};
};

struct Options {
    std::vector<std::size_t> selected_counts{0, 1, 16, 128};
    std::size_t cycles{20'000};
    std::size_t warmup_cycles{2'000};
    bool run_direct{true};
    bool run_representation{true};
};

std::size_t parse_size(std::string_view text) {
    auto parsed = std::size_t{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        std::cerr << "invalid integer: " << text << '\n';
        std::exit(2);
    }
    return static_cast<std::size_t>(parsed);
}

std::vector<std::size_t> parse_counts(std::string_view text) {
    auto counts = std::vector<std::size_t>{};
    auto start = std::size_t{};
    while (true) {
        const auto end = text.find(',', start);
        const auto count = parse_size(text.substr(start, end == text.npos ? text.size() - start : end - start));
        if (count > MaximumKeyCount) {
            std::cerr << "selected count must be at most " << MaximumKeyCount << '\n';
            std::exit(2);
        }
        counts.push_back(count);
        if (end == text.npos) {
            break;
        }
        start = end + 1;
    }
    return counts;
}

Options parse_options(int argc, char **argv) {
    auto options = Options{};
    for (auto index = 1; index < argc; ++index) {
        const auto option = std::string_view{argv[index]};
        if (option == "--selected" && index + 1 < argc) {
            options.selected_counts = parse_counts(argv[++index]);
        } else if (option == "--cycles" && index + 1 < argc) {
            options.cycles = parse_size(argv[++index]);
        } else if (option == "--warmup" && index + 1 < argc) {
            options.warmup_cycles = parse_size(argv[++index]);
        } else if (option == "--mode" && index + 1 < argc) {
            const auto mode = std::string_view{argv[++index]};
            options.run_direct = mode == "both" || mode == "direct";
            options.run_representation = mode == "both" || mode == "representation";
            if (!options.run_direct && !options.run_representation) {
                std::cerr << "mode must be direct, representation, or both\n";
                std::exit(2);
            }
        } else {
            std::cerr << "usage: bg_tsd_selected_reference_benchmark [--selected 0,1,16,128] "
                         "[--cycles N] [--warmup N] [--mode direct|representation|both]\n";
            std::exit(2);
        }
    }
    if (options.cycles == 0) {
        std::cerr << "cycles must be positive\n";
        std::exit(2);
    }
    return options;
}

std::uint64_t expected_checksum(std::size_t selected_count, std::size_t first_tick, std::size_t cycles) {
    const auto tick_sum = cycles * first_tick + (cycles * (cycles + 1)) / 2;
    const auto key_sum = (selected_count * (selected_count - (selected_count != 0))) / 2;
    return static_cast<std::uint64_t>(MaximumKeyCount * selected_count * tick_sum + cycles * key_sum);
}

template <typename DictionaryPoint, typename SinkTemplate>
void wire_selected_items(DictionaryPoint dictionary, const SinkTemplate &sink, std::size_t selected_count) {
    for (auto key = std::size_t{}; key < selected_count; ++key) {
        (void)sink(dictionary[static_cast<std::int64_t>(key)]);
    }
}

void run_case(std::string_view mode, std::size_t selected_count, std::size_t cycles, std::size_t warmup_cycles) {
    const auto source = bg::node<DictionarySource>();
    const auto reference_view = bg::node<ReferenceDictionaryView>();
    const auto sink = bg::node<SelectedItemSink>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        if (mode == "representation") {
            wire_selected_items(reference_view(source()), sink, selected_count);
        } else {
            wire_selected_items(source(), sink, selected_count);
        }
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    DictionarySource::ticks = 0;
    SelectedItemSink::evaluations = 0;
    SelectedItemSink::checksum = 0;

    const auto start_time = bg::MIN_DT + std::chrono::microseconds{1};
    runtime.start(start_time);
    const auto evaluate_cycles = [&](std::size_t first_cycle, std::size_t cycle_count) {
        for (auto cycle = first_cycle; cycle < first_cycle + cycle_count; ++cycle) {
            runtime.schedule(0);
            runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{static_cast<std::int64_t>(cycle + 1)});
        }
    };

    evaluate_cycles(1, warmup_cycles);
    const auto expected_warmup_evaluations = static_cast<std::uint64_t>(selected_count) * warmup_cycles;
    const auto expected_warmup_checksum = expected_checksum(selected_count, 0, warmup_cycles);
    if (SelectedItemSink::evaluations != expected_warmup_evaluations ||
        SelectedItemSink::checksum != expected_warmup_checksum) {
        std::cerr << "warmup correctness failure mode=" << mode << " selected=" << selected_count
                  << " evaluations=" << SelectedItemSink::evaluations
                  << " expected_evaluations=" << expected_warmup_evaluations
                  << " checksum=" << SelectedItemSink::checksum << " expected_checksum="
                  << expected_warmup_checksum << '\n';
        runtime.stop();
        std::exit(1);
    }

    SelectedItemSink::evaluations = 0;
    SelectedItemSink::checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    evaluate_cycles(warmup_cycles + 1, cycles);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const auto expected_evaluations = static_cast<std::uint64_t>(selected_count) * cycles;
    const auto expected = expected_checksum(selected_count, warmup_cycles, cycles);
    if (SelectedItemSink::evaluations != expected_evaluations || SelectedItemSink::checksum != expected) {
        std::cerr << "correctness failure mode=" << mode << " selected=" << selected_count
                  << " evaluations=" << SelectedItemSink::evaluations << " expected_evaluations="
                  << expected_evaluations << " checksum=" << SelectedItemSink::checksum
                  << " expected_checksum=" << expected << '\n';
        runtime.stop();
        std::exit(1);
    }

    runtime.stop();
    std::cout << "mode=" << mode << " selected=" << selected_count << " cycles=" << cycles
              << " elapsed_ns=" << nanoseconds << " ns_per_cycle=" << static_cast<double>(nanoseconds) / cycles
              << " evaluations=" << SelectedItemSink::evaluations << " checksum=" << SelectedItemSink::checksum
              << '\n';
}

} // namespace

int main(int argc, char **argv) {
    const auto options = parse_options(argc, argv);
    for (const auto selected_count : options.selected_counts) {
        if (options.run_direct) {
            run_case("direct", selected_count, options.cycles, options.warmup_cycles);
        }
        if (options.run_representation) {
            run_case("representation", selected_count, options.cycles, options.warmup_cycles);
        }
    }
}

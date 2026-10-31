#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory_resource>
#include <string_view>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

template <template <typename> typename Field> struct BenchmarkInputs {
    Field<TSD<i64, TS<i64>>> collection;
    Field<TSD<i64, TS<i64>>> query_keys;
};

struct BenchmarkSource {
    static constexpr bool schedule_on_start = true;
    static constexpr bool uses_timed_scheduling = true;

    Out<TSB<BenchmarkInputs>> output;

    void evaluate(EvaluationContext *context) const {
        if (cycle == 0) {
            for (auto index = std::size_t{}; index < consumer_count; ++index) {
                output->collection[static_cast<i64>(index)].set(static_cast<i64>(index));
                const auto key = missing_shared_key ? static_cast<i64>(consumer_count + 100)
                                : absent_key_churn ? i64{100}
                                : shared_key_mode ? i64{0}
                                                  : static_cast<i64>(index);
                output->query_keys[static_cast<i64>(index)].set(key);
            }
        } else if (absent_key_churn) {
            output->query_keys[0].set(static_cast<i64>(100 + cycle));
        } else if (shared_key_mode && !missing_shared_key) {
            key_is_absent[0] = !key_is_absent[0];
            if (key_is_absent[0]) {
                output->collection.erase(i64{0});
            } else {
                output->collection[0].set(0);
            }
            if (measuring && !key_is_absent[0]) {
                expected_checksum += static_cast<std::uint64_t>(consumer_count);
            }
        } else if (missing_shared_key) {
            const auto index = (cycle - 1) % consumer_count;
            key_is_absent[index] = !key_is_absent[index];
            if (key_is_absent[index]) {
                output->collection.erase(static_cast<i64>(index));
                --present_count;
            } else {
                output->collection[static_cast<i64>(index)].set(static_cast<i64>(index));
                ++present_count;
            }
        } else {
            update_collection_membership();
        }

        ++cycle;
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    static void reset(std::size_t count, std::size_t changes, bool selected, bool churn, bool shared,
                      bool missing) {
        consumer_count = count;
        changes_per_cycle = changes;
        selected_mode = selected;
        absent_key_churn = churn;
        shared_key_mode = shared;
        missing_shared_key = missing;
        cycle = 0;
        key_is_absent.assign(count, false);
        present_count = count;
        measuring = false;
        expected_checksum = 0;
    }

    static inline std::size_t consumer_count{};
    static inline std::size_t changes_per_cycle{};
    static inline bool selected_mode{};
    static inline bool absent_key_churn{};
    static inline bool shared_key_mode{};
    static inline bool missing_shared_key{};
    static inline bool measuring{};
    static inline std::size_t cycle{};
    static inline std::size_t present_count{};
    static inline std::vector<bool> key_is_absent;
    static inline std::uint64_t expected_checksum{};

  private:
    void update_collection_membership() const {
        for (auto offset = std::size_t{}; offset < changes_per_cycle; ++offset) {
            const auto index = ((cycle - 1) * changes_per_cycle + offset) % consumer_count;
            key_is_absent[index] = !key_is_absent[index];
            if (key_is_absent[index]) {
                output->collection.erase(static_cast<i64>(index));
                --present_count;
            } else {
                output->collection[static_cast<i64>(index)].set(static_cast<i64>(index));
                ++present_count;
            }
            if (measuring && selected_mode && !key_is_absent[index]) {
                ++expected_checksum;
            }
        }

        if (measuring && !selected_mode) {
            expected_checksum += static_cast<std::uint64_t>(present_count);
        }
    }
};

struct WholeRootContains {
    static constexpr bool deduplicate_wiring = false;

    In<TSD<i64, TS<i64>>> collection;
    In<TS<i64>> key;
    Out<TS<bool>> output;

    void evaluate() const {
        ++evaluation_count;
        output.set(collection.contains(key.value_view()));
    }

    static inline std::uint64_t evaluation_count{};
};

struct SelectedContains {
    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool deduplicate_wiring = false;

    In<REF<TSD<i64, TS<i64>>>> collection;
    In<TS<i64>> key;
    In<TS<bool>, optional, dynamic_binding> membership;
    Out<TS<bool>> output;

    void evaluate() const {
        ++evaluation_count;
        membership.bind(collection.membership(key.value_view()));
        output.copy(membership);
    }

    void on_invalidation() const {
        membership.unbind();
        output.invalidate();
    }

    static inline std::uint64_t evaluation_count{};
};

struct ResultSink {
    static constexpr bool deduplicate_wiring = false;

    In<TS<bool>> value;

    void evaluate() const {
        ++evaluation_count;
        checksum += static_cast<std::uint64_t>(*value);
    }

    static inline std::uint64_t evaluation_count{};
    static inline std::uint64_t checksum{};
};

class CountingMemoryResource final : public std::pmr::memory_resource {
  public:
    std::size_t live_bytes{};
    std::size_t peak_bytes{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live_bytes += bytes;
        peak_bytes = std::max(peak_bytes, live_bytes);
        return result;
    }

    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        live_bytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

void run_case(std::string_view mode, std::size_t count, std::size_t change_percent, std::size_t cycles,
              std::size_t warmup_cycles, bool absent_key_churn = false, bool shared_key_mode = false,
              bool missing_shared_key = false) {
    const auto changes_per_cycle = std::max<std::size_t>(1, count * change_percent / 100);
    const auto selected = mode == "selected";
    BenchmarkSource::reset(count, changes_per_cycle, selected, absent_key_churn, shared_key_mode,
                           missing_shared_key);
    WholeRootContains::evaluation_count = 0;
    SelectedContains::evaluation_count = 0;
    ResultSink::evaluation_count = 0;
    ResultSink::checksum = 0;

    const auto source = node<BenchmarkSource>();
    const auto whole_root_contains = node<WholeRootContains>();
    const auto selected_contains = node<SelectedContains>();
    const auto sink = node<ResultSink>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto inputs = source();
        const auto collection = inputs->collection;
        const auto query_keys = inputs->query_keys;
        for (auto index = std::size_t{}; index < count; ++index) {
            const auto key_index = shared_key_mode ? std::size_t{} : index;
            const auto key = query_keys[static_cast<i64>(key_index)];
            if (selected) {
                sink(selected_contains(collection, key));
            } else {
                sink(whole_root_contains(collection, key));
            }
        }
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto memory = CountingMemoryResource{};
    auto baseline_live_bytes = std::size_t{};
    auto after_start_live_bytes = std::size_t{};
    std::size_t bound_live_bytes{};
    struct Sample {
        std::int64_t elapsed_ns;
        std::uint64_t contains_evaluations;
        std::uint64_t sink_evaluations;
        std::uint64_t checksum;
        std::size_t maximum_runtime_live_bytes;
    };
    auto samples = std::vector<Sample>{};
    samples.reserve(3);
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        baseline_live_bytes = memory.live_bytes;
        runtime.start(MIN_DT + std::chrono::microseconds{1});
        after_start_live_bytes = memory.live_bytes;
        const auto run_cycles = [&](std::size_t first_cycle, std::size_t cycle_count,
                                    std::size_t &maximum_runtime_live_bytes) {
            for (auto cycle = first_cycle; cycle < first_cycle + cycle_count; ++cycle) {
                runtime.evaluate(MIN_DT + std::chrono::microseconds{static_cast<std::int64_t>(cycle + 1)});
                maximum_runtime_live_bytes = std::max(maximum_runtime_live_bytes, memory.live_bytes);
            }
        };

        auto warmup_maximum_runtime_live_bytes = memory.live_bytes;
        run_cycles(1, warmup_cycles, warmup_maximum_runtime_live_bytes);
        bound_live_bytes = memory.live_bytes;
        for (auto sample_index = std::size_t{}; sample_index < 3; ++sample_index) {
            WholeRootContains::evaluation_count = 0;
            SelectedContains::evaluation_count = 0;
            ResultSink::evaluation_count = 0;
            ResultSink::checksum = 0;
            BenchmarkSource::expected_checksum = 0;
            BenchmarkSource::measuring = true;
            auto sample_maximum_runtime_live_bytes = bound_live_bytes;
            const auto start = std::chrono::steady_clock::now();
            run_cycles(warmup_cycles + 1 + sample_index * cycles, cycles, sample_maximum_runtime_live_bytes);
            const auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            BenchmarkSource::measuring = false;

            auto expected_evaluations = static_cast<std::uint64_t>(count * cycles);
            if (absent_key_churn) {
                expected_evaluations = static_cast<std::uint64_t>(cycles);
            } else if (selected && missing_shared_key) {
                expected_evaluations = 0;
            } else if (selected && !shared_key_mode) {
                expected_evaluations = static_cast<std::uint64_t>(changes_per_cycle * cycles);
            }
            const auto actual_evaluations = selected ? SelectedContains::evaluation_count
                                                     : WholeRootContains::evaluation_count;
            if (actual_evaluations != expected_evaluations ||
                ResultSink::checksum != BenchmarkSource::expected_checksum) {
                std::cerr << "correctness failure mode=" << mode << " count=" << count
                          << " sample=" << sample_index + 1 << " evaluations=" << actual_evaluations
                          << " expected_evaluations=" << expected_evaluations << " checksum=" << ResultSink::checksum
                          << " expected_checksum=" << BenchmarkSource::expected_checksum << '\n';
                std::exit(1);
            }
            if (absent_key_churn && sample_maximum_runtime_live_bytes > bound_live_bytes + 256) {
                std::cerr << "membership growth failure: live bytes grew from " << bound_live_bytes << " to "
                          << sample_maximum_runtime_live_bytes << " during absent-key churn sample "
                          << sample_index + 1 << '\n';
                std::exit(1);
            }

            samples.push_back({elapsed_ns, actual_evaluations, ResultSink::evaluation_count, ResultSink::checksum,
                               sample_maximum_runtime_live_bytes});
        }
        runtime.stop();
    }

    if (memory.live_bytes != 0) {
        std::cerr << "membership cleanup failure mode=" << mode << " consumers=" << count
                  << " retains " << memory.live_bytes << " bytes after runtime destruction\n";
        std::exit(1);
    }

    const auto dynamic_live_delta = bound_live_bytes - baseline_live_bytes;
    const auto scenario = absent_key_churn ? std::string_view{"absent_key_churn"}
                         : shared_key_mode ? (missing_shared_key ? std::string_view{"shared_miss"}
                                                                 : std::string_view{"shared_hit"})
                                           : std::string_view{"unique_keys"};
    const auto changed_collection_keys_per_cycle = absent_key_churn ? std::size_t{}
                                              : shared_key_mode ? std::size_t{1}
                                                                : changes_per_cycle;
    for (auto sample_index = std::size_t{}; sample_index < samples.size(); ++sample_index) {
        const auto &sample = samples[sample_index];
        std::cout << "mode=" << mode << " scenario=" << scenario << " consumers=" << count;
        if (!shared_key_mode && !absent_key_churn) {
            std::cout << " collection_change_percent=" << change_percent;
        }
        std::cout << " changed_collection_keys_per_cycle=" << changed_collection_keys_per_cycle
                  << " sample=" << sample_index + 1 << " cycles_per_sample=" << cycles
                  << " elapsed_ns=" << sample.elapsed_ns
                  << " contains_evaluations=" << sample.contains_evaluations
                  << " sink_evaluations=" << sample.sink_evaluations << " checksum=" << sample.checksum
                  << " compiled_fixed_bytes=" << compiled.fixed_storage_size()
                  << " compiled_runtime_bytes=" << compiled.runtime_storage_size()
                  << " allocator_live_before_start_bytes=" << baseline_live_bytes
                  << " allocator_live_after_start_bytes=" << after_start_live_bytes
                  << " allocator_live_after_binding_bytes=" << bound_live_bytes
                  << " allocator_dynamic_live_delta_bytes=" << dynamic_live_delta
                  << " allocator_peak_bytes=" << memory.peak_bytes
                  << " allocator_max_runtime_live_bytes=" << sample.maximum_runtime_live_bytes
                  << " allocator_live_after_stop_bytes=" << memory.live_bytes;
        if (absent_key_churn) {
            std::cout << " absent_key_churn=verified";
        }
        std::cout << '\n' << std::flush;
    }
}

} // namespace

int main(int argc, char **argv) {
    constexpr auto consumers = std::size_t{10'000};
    constexpr auto cycles = std::size_t{100};
    constexpr auto warmup_cycles = std::size_t{20};

    if (argc == 3 && std::string_view{argv[1]} == "--scenario") {
        const auto scenario = std::string_view{argv[2]};
        if (scenario == "unique_1pct") {
            run_case("selected", consumers, 1, cycles, warmup_cycles);
            return 0;
        }
        if (scenario == "shared_hit") {
            run_case("selected", consumers, 100, cycles, warmup_cycles, false, true);
            return 0;
        }
        if (scenario == "absent_key_churn") {
            run_case("selected", 1, 100, 10'000, 20, true);
            return 0;
        }

        std::cerr << "unknown scenario: " << scenario << '\n';
        return 2;
    }
    if (argc != 1) {
        std::cerr << "usage: tsd_membership_benchmark [--scenario unique_1pct|shared_hit|absent_key_churn]\n";
        return 2;
    }

    for (const auto percent : {std::size_t{1}, std::size_t{10}}) {
        run_case("whole_root", consumers, percent, cycles, warmup_cycles);
        run_case("selected", consumers, percent, cycles, warmup_cycles);
    }
    run_case("whole_root", 64, 1, cycles, warmup_cycles);
    run_case("selected", 64, 1, cycles, warmup_cycles);
    run_case("whole_root", consumers, 100, cycles, warmup_cycles, false, true);
    run_case("selected", consumers, 100, cycles, warmup_cycles, false, true);
    run_case("whole_root", consumers, 100, cycles, warmup_cycles, false, true, true);
    run_case("selected", consumers, 100, cycles, warmup_cycles, false, true, true);
    run_case("selected", 1, 100, 10'000, 20, true);
}

#include <bg/node.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/tss/tss_output.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tick_serializer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct tick_delay_storage {
    tick_delay_storage(bg::util::buffer buffer, const bg::TimeSeriesType &type, bg::allocator memory = {})
        : reader{buffer, type}, writer{std::move(buffer), memory} {}

    bg::TimeSeriesTickReader reader;
    bg::TimeSeriesTickWriter writer;
};

struct scalar_to_set_definition {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TSS<bg::i64>> output;
    bg::State<bg::i64> previous;

    void evaluate() const {
        if (!input.modified()) {
            return;
        }

        if (*previous != 0) {
            output.erase(*previous);
        }
        output.insert(*input);
        *previous = *input;
    }
};

template <typename SERIES> struct one_tick_delay_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<SERIES, bg::optional> input;
    bg::Out<SERIES> output;
    bg::State<tick_delay_storage *> storage;

    void start(bg::EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        *storage = memory.new_object<tick_delay_storage>(bg::util::buffer::chain(256, memory), SERIES::type());
    }

    void evaluate(bg::EvaluationContext *context) const {
        if ((*storage)->reader.has_next()) {
            (*storage)->reader.replay(output);
        }

        if ((*storage)->writer.write(input, context->evaluation_time())) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        auto memory = context->runtime_graph().allocation();
        memory.delete_object(*storage);
        *storage = nullptr;
    }
};

template <typename SERIES> struct clocked_one_tick_delay_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<SERIES, bg::optional> input;
    bg::In<bg::TS<bg::i64>, bg::optional> clock;
    bg::Out<SERIES> output;
    bg::State<tick_delay_storage *> storage;

    void start(bg::EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        *storage = memory.new_object<tick_delay_storage>(bg::util::buffer::chain(256, memory), SERIES::type());
    }

    void evaluate(bg::EvaluationContext *context) const {
        if ((*storage)->reader.has_next()) {
            (*storage)->reader.replay(output);
        }

        if ((*storage)->writer.write(input, context->evaluation_time())) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        auto memory = context->runtime_graph().allocation();
        memory.delete_object(*storage);
        *storage = nullptr;
    }
};

struct set_sum_definition {
    bg::In<bg::TSS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        auto sum = bg::i64{};
        input.for_each([&](const auto &key) { sum += key.value(); });
        output.set(sum);
    }
};

struct fixed_list_sum_definition {
    bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        item_updates.push_back({input.item_updated(0), input.item_updated(1)});
        output.set(*input[0] + *input[1]);
    }

    inline static std::vector<std::array<bool, 2>> item_updates;
};

struct fixed_list_update_tracker_definition {
    bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        if (!input.modified()) {
            return;
        }

        item_updates.push_back({input.item_updated(0), input.item_updated(1)});
        output.set((input.item_updated(0) ? 100 : 0) + (input.item_updated(1) ? 10 : 0));
    }

    inline static std::vector<std::array<bool, 2>> item_updates;
};

template <template <typename, auto...> typename Field> struct TickPair {
    Field<bg::TS<bg::i64>> first;
    Field<bg::TS<bg::i64>> second;
};

struct bundle_sum_definition {
    bg::In<bg::TSB<TickPair>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input->first + *input->second); }
};

struct dictionary_sum_definition {
    bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        auto sum = bg::i64{};
        input.for_each_key_value([&](const auto &, const auto &value) { sum += *value; });
        output.set(sum);
    }
};

struct nested_dictionary_sum_definition {
    bg::In<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        auto sum = bg::i64{};
        input.for_each_value([&](const auto &inner) {
            inner.for_each_value([&](const auto &value) {
                if (value.valid()) {
                    sum += *value;
                }
            });
        });

        output.set(sum);
    }
};

struct set_record_size_definition {
    bg::In<bg::TSS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        const auto buffer = bg::util::buffer::contiguous(128);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (writer.write(input, input.last_updated())) {
            output.set(static_cast<bg::i64>(buffer.bytes().size()));
        }
    }
};

struct dictionary_record_size_definition {
    bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        const auto buffer = bg::util::buffer::contiguous(128);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (writer.write(input, input.last_updated())) {
            output.set(static_cast<bg::i64>(buffer.bytes().size()));
        }
    }
};

class allocation_counter : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t deallocations{};

  private:
    void *do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }

    void do_deallocate(void *storage, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(storage, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

allocation_counter retained_value_memory;

} // namespace

TEST_CASE("a node can delay time series ticks by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:value][scalar:i64]"
          "[shape:single-node][transition:update]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TS<bg::i64>>>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return delay(input); };
    const auto input = std::array<std::optional<bg::i64>, 5>{1, 2, std::nullopt, 3, std::nullopt};

    const auto actual = bg::testing::test_graph(graph, input);

    CHECK(actual.equals({std::nullopt, 1, 2, std::nullopt, 3}));
}

TEST_CASE("a scalar integer tick uses a nine-byte record", "[feature:time-series-storage][series:value][scalar:i64]") {
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto last_updated = bg::TimeSeriesState{time};
    const auto value = bg::i64{42};
    const auto input = bg::In<bg::TS<bg::i64>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>(), last_updated, &value, true}};
    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};

    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(value));

    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::i64>::type()};
    CHECK(reader.discard());
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("discarding a scalar tick consumes a chain record across blocks",
          "[feature:time-series-storage][series:value][scalar:i64][transition:update]") {
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto state = bg::TimeSeriesState{};
    state.publish(time);
    const auto value = bg::i64{42};
    const auto input = bg::In<bg::TS<bg::i64>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>(), state, &value, true}};
    const auto buffer = bg::util::buffer::chain(3);
    auto writer = bg::TimeSeriesTickWriter{buffer};

    REQUIRE(writer.write(input, time));
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::i64>::type()};

    CHECK(reader.discard());
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("discarding a collection tick consumes keys across chain blocks",
          "[evaluation][feature:test-graph][feature:time-series-storage][series:tss][scalar:i64]"
          "[shape:chain][transition:addition]") {
    const auto to_set = bg::node<scalar_to_set_definition>();
    const auto discard_record = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bool>> output) {
        const auto buffer = bg::util::buffer::chain(3);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (!writer.write(input, input.last_updated())) {
            return;
        }

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TSS<bg::i64>::type()};
        output.set(reader.discard());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return discard_record(to_set(input)); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 1>{42});

    CHECK(actual.equals({true}));
}

TEST_CASE("a set tick stores only added and removed keys",
          "[feature:time-series-storage][series:tss][scalar:i64][transition:update]") {
    const auto to_set = bg::node<scalar_to_set_definition>();
    const auto measure_set = bg::node<set_record_size_definition>();
    const auto set_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return measure_set(to_set(input)); };
    const auto set_sizes = bg::testing::test_graph(set_graph, std::array<bg::i64, 2>{1, 2});

    CHECK(set_sizes.equals({11, 20}));
}

TEST_CASE("a dictionary tick stores only its added or modified item",
          "[feature:time-series-storage][series:tsd][scalar:i64][transition:update]") {
    const auto make_dictionary = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                             bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>> output) {
        output[1].set(*input);
    });
    const auto measure_dictionary = bg::node<dictionary_record_size_definition>();
    const auto dictionary_graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return measure_dictionary(make_dictionary(input));
    };
    const auto dictionary_sizes = bg::testing::test_graph(dictionary_graph, std::array<bg::i64, 2>{1, 2});

    CHECK(dictionary_sizes.equals({19, 19}));
}

TEST_CASE("an empty TSS tick contains a terminator", "[feature:time-series-storage][series:tss][scalar:i64]") {
    const auto clear_set = bg::node([](bg::In<bg::TS<bg::i64>>, bg::Out<bg::TSS<bg::i64>> output) {
        output.clear();
    });
    const auto measure_set = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        const auto buffer = bg::util::buffer::contiguous(16);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (!writer.write(input, input.last_updated())) {
            return;
        }

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TSS<bg::i64>::type()};
        output.set(reader.discard() && !reader.has_next() ? static_cast<bg::i64>(buffer.bytes().size()) : -1);
    });
    const auto set_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return measure_set(clear_set(input)); };

    CHECK(bg::testing::test_graph(set_graph, std::array<bg::i64, 1>{1}).equals({2}));
}

TEST_CASE("an empty TSD tick contains a terminator", "[feature:time-series-storage][series:tsd][scalar:i64]") {
    const auto clear_dictionary = bg::node(
        [](bg::In<bg::TS<bg::i64>>, bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>> output) { output.clear(); });
    const auto measure_dictionary =
        bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input, bg::Out<bg::TS<bg::i64>> output) {
            const auto buffer = bg::util::buffer::contiguous(16);
            auto writer = bg::TimeSeriesTickWriter{buffer};
            if (!writer.write(input, input.last_updated())) {
                return;
            }

            auto reader = bg::TimeSeriesTickReader{buffer, bg::TSD<bg::i64, bg::TS<bg::i64>>::type()};
            output.set(reader.discard() && !reader.has_next() ? static_cast<bg::i64>(buffer.bytes().size()) : -1);
        });
    const auto dictionary_graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return measure_dictionary(clear_dictionary(input));
    };

    CHECK(bg::testing::test_graph(dictionary_graph, std::array<bg::i64, 1>{1}).equals({2}));
}

TEST_CASE("a node delays time series invalidation and revalidation by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[feature:validity][series:value][scalar:i64][shape:chain]"
          "[transition:invalidation][transition:revalidation]") {
    const auto invalidate_on_zero = bg::node(
        [](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
            if (*input == 0) {
                output.invalidate();
                return;
            }

            output.set(*input);
        });
    const auto delay = bg::node<one_tick_delay_definition<bg::TS<bg::i64>>>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return delay(invalidate_on_zero(input)); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 4>{1, 2, 0, 3});

    CHECK(actual.equals({std::nullopt, 1, 2, std::nullopt, 3}));
}

TEST_CASE("an older invalidation is not retained again when another input ticks",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[feature:validity][series:value][scalar:i64][shape:chain][transition:invalidation]") {
    const auto invalidate_on_zero = bg::node(
        [](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
            if (*input == 0) {
                output.invalidate();
                return;
            }

            output.set(*input);
        });
    const auto delay = bg::node<clocked_one_tick_delay_definition<bg::TS<bg::i64>>>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input, bg::wp<bg::TS<bg::i64>> clock) {
        return delay(invalidate_on_zero(input), clock);
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<bg::i64, 2>{1, 0},
        std::array<std::optional<bg::i64>, 3>{std::nullopt, 1, 2});

    CHECK(actual.equals({std::nullopt, 1, std::nullopt}));
}

TEST_CASE("a node delays time series set changes by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tss][scalar:i64][shape:chain][transition:addition][transition:removal]") {
    const auto to_set = bg::node<scalar_to_set_definition>();
    const auto delay = bg::node<one_tick_delay_definition<bg::TSS<bg::i64>>>();
    const auto sum = bg::node<set_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return sum(delay(to_set(input))); };
    const auto input = std::array<std::optional<bg::i64>, 5>{1, 2, std::nullopt, 3, std::nullopt};

    const auto actual = bg::testing::test_graph(graph, input);

    CHECK(actual.equals({std::nullopt, 1, 2, std::nullopt, 3}));
}

TEST_CASE("a node delays fixed list item changes by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:fixed-tsl][scalar:i64][shape:chain][transition:update]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSL<bg::TS<bg::i64>, 2>>>();
    const auto sum = bg::node<fixed_list_sum_definition>();
    fixed_list_sum_definition::item_updates.clear();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second) {
        return sum(delay(bg::tsl(first, second)));
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<std::optional<bg::i64>, 4>{1, 2, 3, std::nullopt},
        std::array<std::optional<bg::i64>, 4>{10, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({std::nullopt, 11, 12, 13}));
    CHECK(fixed_list_sum_definition::item_updates ==
          std::vector<std::array<bool, 2>>{{true, true}, {true, false}, {true, false}});
}

TEST_CASE("a first fixed list snapshot can contain a never-valid item",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:fixed-tsl][scalar:i64][shape:chain][transition:first-tick]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSL<bg::TS<bg::i64>, 2>>>();
    const auto track_updates = bg::node<fixed_list_update_tracker_definition>();
    fixed_list_update_tracker_definition::item_updates.clear();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second) {
        return track_updates(delay(bg::tsl(first, second)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 1>{7},
                                                std::array<std::optional<bg::i64>, 1>{std::nullopt});

    CHECK(actual.equals({std::nullopt, 100}));
    CHECK(fixed_list_update_tracker_definition::item_updates == std::vector<std::array<bool, 2>>{{true, false}});
}

TEST_CASE("a later fixed list item tick does not repeat an earlier invalidation",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:fixed-tsl][scalar:i64][shape:chain][transition:invalidation][transition:update]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSL<bg::TS<bg::i64>, 2>>>();
    const auto track_updates = bg::node<fixed_list_update_tracker_definition>();
    const auto invalidate_on_zero = bg::node(
        [](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
            if (*input == 0) {
                output.invalidate();
                return;
            }

            output.set(*input);
        });
    fixed_list_update_tracker_definition::item_updates.clear();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second) {
        return track_updates(delay(bg::tsl(invalidate_on_zero(first), second)));
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<bg::i64, 2>{1, 0},
        std::array<std::optional<bg::i64>, 4>{10, 11, 12, 13});

    CHECK(actual.equals({std::nullopt, 110, 110, 10, 10}));
    CHECK(fixed_list_update_tracker_definition::item_updates ==
          std::vector<std::array<bool, 2>>{{true, true}, {true, true}, {false, true}, {false, true}});
}

TEST_CASE("a node delays bundle field changes by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsb][scalar:i64][shape:chain][transition:update]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSB<TickPair>>>();
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return sum(delay(bg::bundle<bg::TSB<TickPair>>(input, input)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array<std::optional<bg::i64>, 4>{1, 2, 3, std::nullopt});

    CHECK(actual.equals({std::nullopt, 2, 4, 6}));
}

TEST_CASE("a node delays dictionary item changes by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:addition][transition:update]") {
    const auto set_item = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                      bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>> output) {
        if (*input == 0) {
            output.erase(1);
            return;
        }

        output[1].set(*input);
    });
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TS<bg::i64>>>>();
    const auto sum = bg::node<dictionary_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return sum(delay(set_item(input))); };

    const auto actual = bg::testing::test_graph(graph, std::array<std::optional<bg::i64>, 4>{1, 2, 0, std::nullopt});

    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("a node delays nested dictionary changes by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:addition][transition:update][transition:removal]") {
    const auto update = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                    bg::Out<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        if (*input == 0) {
            output.erase(1);
            return;
        }

        output[1][2].set(*input);
    });
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>>>();
    const auto sum = bg::node<nested_dictionary_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return sum(delay(update(input))); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 3>{1, 2, 0});

    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("discarding an unread tick record releases its scalar payload", "[coverage-exempt]") {
    retained_value_memory.allocations = 0;
    retained_value_memory.deallocations = 0;

    {
        auto value_memory = bg::allocator{retained_value_memory};
        const auto value = bg::ScalarValue<bg::string>{
            std::string{"a retained string payload long enough to require allocated storage"}, value_memory};
        const auto scalar = value.view();
        const auto time = bg::MIN_DT + std::chrono::microseconds{1};
        const auto last_updated = bg::TimeSeriesState{time};
        const auto input_data = bg::TimeSeriesInputData{
            bg::cpp_time_series_runtime_type<bg::TS<bg::string>>(), last_updated, scalar.data(), true};
        const auto input = bg::In<bg::TS<bg::string>>{input_data};
        const auto buffer = bg::util::buffer::chain(256);
        auto writer = bg::TimeSeriesTickWriter{buffer, value_memory};
        const auto allocations_before_write = retained_value_memory.allocations;

        REQUIRE(writer.write(input, time));
        REQUIRE(writer.write(input, time));
        CHECK(retained_value_memory.allocations > allocations_before_write);

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::string>::type()};
        CHECK(reader.discard());
    }

    CHECK(retained_value_memory.allocations == retained_value_memory.deallocations);
}

TEST_CASE("destroying a tick reader releases unread scalar payloads", "[coverage-exempt]") {
    retained_value_memory.allocations = 0;
    retained_value_memory.deallocations = 0;

    {
        auto value_memory = bg::allocator{retained_value_memory};
        const auto value = bg::ScalarValue<bg::string>{
            std::string{"a retained string payload long enough to require allocated storage"}, value_memory};
        const auto scalar = value.view();
        const auto time = bg::MIN_DT + std::chrono::microseconds{1};
        const auto last_updated = bg::TimeSeriesState{time};
        const auto input_data = bg::TimeSeriesInputData{
            bg::cpp_time_series_runtime_type<bg::TS<bg::string>>(), last_updated, scalar.data(), true};
        const auto input = bg::In<bg::TS<bg::string>>{input_data};
        const auto buffer = bg::util::buffer::chain(256);
        auto writer = bg::TimeSeriesTickWriter{buffer, value_memory};

        REQUIRE(writer.write(input, time));
        REQUIRE(writer.write(input, time));
        CHECK(retained_value_memory.allocations > 0);

        { auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::string>::type()}; }
    }

    CHECK(retained_value_memory.allocations == retained_value_memory.deallocations);
}

TEST_CASE("a tick writer rejects reference time series",
          "[feature:time-series-storage][series:reference][binding:direct]") {
    const auto input = bg::In<bg::REF<bg::TS<bg::i64>>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::REF<bg::TS<bg::i64>>>(), bg::initial_time_series_state, nullptr}};
    const auto buffer = bg::util::buffer::chain(128);
    auto writer = bg::TimeSeriesTickWriter{buffer};

    CHECK_THROWS_AS(writer.write(input, bg::MIN_DT), std::invalid_argument);
}

TEST_CASE("tick replay rejects an incompatible output without consuming its record",
          "[evaluation][feature:test-graph][feature:time-series-storage][series:tss][scalar:i64]"
          "[transition:update][shape:chain]") {
    auto record_remains = false;
    const auto to_set = bg::node<scalar_to_set_definition>();
    const auto replay_as_value = bg::node([&](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        const auto buffer = bg::util::buffer::chain(128);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        REQUIRE(writer.write(input, input.last_updated()));

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TSS<bg::i64>::type()};
        try {
            reader.replay(output);
        } catch (const std::invalid_argument &) {
            record_remains = reader.has_next();
            throw;
        }
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return replay_as_value(to_set(input)); };

    CHECK_THROWS_AS(bg::testing::test_graph(graph, std::array<bg::i64, 1>{1}), std::invalid_argument);
    CHECK(record_remains);
}

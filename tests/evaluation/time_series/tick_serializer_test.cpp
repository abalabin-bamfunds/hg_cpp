#include <bg/node.hpp>
#include <bg/scalar/bundle/details/allocation_owner.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/output_state.hpp>
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
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct tick_delay_storage {
    tick_delay_storage(bg::util::buffer buffer, const bg::TimeSeriesType &type, bg::allocator memory = {})
        : reader{buffer, type}, writer{std::move(buffer), memory} {}

    bg::TimeSeriesTickReader reader;
    bg::TimeSeriesTickWriter writer;
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

template <typename SERIES> struct collection_size_definition {
    bg::In<SERIES> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        if (input.valid()) {
            output.set(static_cast<bg::i64>(input.size()));
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

template <typename T> void check_builtin_scalar_tick(const typename bg::scalar_storage_traits<T>::stored_type &value) {
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto input_state = bg::TimeSeriesState{time};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<T>>();
    const auto input = bg::In<bg::TS<T>>{bg::TimeSeriesInputData{runtime_type, input_state, &value, true}};
    const auto buffer = bg::util::buffer::contiguous(256);
    auto writer = bg::TimeSeriesTickWriter{buffer};

    REQUIRE(writer.write(input, time));

    const auto replay_time = time + std::chrono::microseconds{1};
    auto output_state = bg::TimeSeriesState{};
    auto output_value = typename bg::scalar_storage_traits<T>::stored_type{};
    auto output_data = bg::OutputState{runtime_type};
    output_data.bind(output_state, &output_value, replay_time);
    auto output = bg::Out<bg::TS<T>>{bg::TimeSeriesOutputBinding{output_data}};
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<T>::type()};

    REQUIRE(reader.replay(output));
    CHECK_FALSE(reader.has_next());
    CHECK(output.valid());
    CHECK(output.value() == bg::scalar_storage_traits<T>::read(value));
}

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

TEST_CASE("built-in scalar ticks replay through their typed storage",
          "[feature:time-series-storage][series:value][scalar:generic][transition:update]") {
    check_builtin_scalar_tick<bg::byte>(bg::byte{0x2a});
    check_builtin_scalar_tick<bool>(true);
    check_builtin_scalar_tick<bg::i64>(42);
    check_builtin_scalar_tick<double>(42.5);
    check_builtin_scalar_tick<bg::date_time_t>(bg::MIN_DT + std::chrono::microseconds{42});
    check_builtin_scalar_tick<bg::timedelta_t>(bg::timedelta_t{42});
    check_builtin_scalar_tick<bg::symbol>(bg::symbol{"value"});
}

TEST_CASE("a string tick keeps its text after the source value is released",
          "[feature:time-series-storage][series:value][scalar:string][transition:update]") {
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto input_state = bg::TimeSeriesState{time};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::string>>();
    const auto &scalar_runtime_type = bg::cpp_scalar_runtime_type<bg::string>();
    const auto buffer = bg::util::buffer::contiguous(128);
    auto writer = bg::TimeSeriesTickWriter{buffer};

    {
        auto source = bg::ScalarValue<bg::string>{"value"};
        auto *stored = bg::details::StringStorage::from(bg::details::scalar_data(source.view()));
        const auto input = bg::In<bg::TS<bg::string>>{
            bg::TimeSeriesInputData{runtime_type, input_state, &stored, true}};
        REQUIRE(writer.write(input, time));
    }

    auto output_state = bg::TimeSeriesState{};
    auto *output_value = static_cast<bg::details::StringStorage *>(nullptr);
    auto output_data = bg::OutputState{runtime_type};
    output_data.bind(output_state, &output_value, time + std::chrono::microseconds{1});
    auto output = bg::Out<bg::TS<bg::string>>{bg::TimeSeriesOutputBinding{output_data}};
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::string>::type()};

    REQUIRE(reader.replay(output));
    CHECK(output.valid());
    CHECK(output.value() == "value");
    CHECK_FALSE(reader.has_next());

    auto memory = bg::allocator{};
    scalar_runtime_type.destroy(&output_value, memory);
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
    const auto discard_record = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bool>> output) {
        const auto buffer = bg::util::buffer::chain(3);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (!writer.write(input, input.last_updated())) {
            return;
        }

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TSS<bg::i64>::type()};
        output.set(reader.discard());
    });
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return discard_record(input); };

    const auto actual = bg::testing::test_graph(graph, {{42}});

    CHECK(actual.equals({true}));
}

TEST_CASE("a set tick stores only added and removed keys",
          "[feature:time-series-storage][series:tss][scalar:i64][transition:update]") {
    const auto measure_set = bg::node<set_record_size_definition>();
    const auto set_graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return measure_set(input); };
    const auto set_sizes = bg::testing::test_graph(set_graph, {{1}, {literal::REMOVED(1), 2}});

    CHECK(set_sizes.equals({11, 20}));
}

TEST_CASE("a dictionary tick stores only its added or modified item",
          "[feature:time-series-storage][series:tsd][scalar:i64][transition:update]") {
    const auto measure_dictionary = bg::node<dictionary_record_size_definition>();
    const auto dictionary_graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        return measure_dictionary(input);
    };
    const auto dictionary_sizes = bg::testing::test_graph(dictionary_graph, {{{1, 1}}, {{1, 2}}});

    CHECK(dictionary_sizes.equals({19, 19}));
}

TEST_CASE("an empty TSS tick contains a terminator", "[feature:time-series-storage][series:tss][scalar:i64]") {
    const auto measure_set = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        const auto buffer = bg::util::buffer::contiguous(16);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (!writer.write(input, input.last_updated())) {
            return;
        }

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TSS<bg::i64>::type()};
        output.set(reader.discard() && !reader.has_next() ? static_cast<bg::i64>(buffer.bytes().size()) : -1);
    });
    const auto set_graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return measure_set(input); };

    CHECK(bg::testing::test_graph(set_graph, {{}}).equals({2}));
}

TEST_CASE("an empty TSD tick contains a terminator", "[feature:time-series-storage][series:tsd][scalar:i64]") {
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
    const auto dictionary_graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        return measure_dictionary(input);
    };

    CHECK(bg::testing::test_graph(dictionary_graph, {{}}).equals({2}));
}

TEST_CASE("a node delays the first valid empty TSS tick by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tss][scalar:i64][shape:chain][transition:first-tick]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSS<bg::i64>>>();
    const auto size = bg::node<collection_size_definition<bg::TSS<bg::i64>>>();
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return size(delay(input)); };

    const auto actual = bg::testing::test_graph(graph, {{}});

    CHECK(actual.equals({std::nullopt, 0}));
}

TEST_CASE("a node delays the first valid empty TSD tick by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:first-tick]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TS<bg::i64>>>>();
    const auto size = bg::node<collection_size_definition<bg::TSD<bg::i64, bg::TS<bg::i64>>>>();
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return size(delay(input)); };

    const auto actual = bg::testing::test_graph(graph, {{}});

    CHECK(actual.equals({std::nullopt, 0}));
}

TEST_CASE("a node delays a new TSD key with an invalid TSS child",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:addition]") {
    const auto add_key = bg::node([](bg::In<bg::TS<bg::i64>> trigger,
                                     bg::Out<bg::TSD<bg::i64, bg::TSS<bg::i64>>> output) {
        if (trigger.modified()) {
            (void)output[1];
        }
    });
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TSS<bg::i64>>>>();
    const auto observe_child = bg::node([](bg::In<bg::TSD<bg::i64, bg::TSS<bg::i64>>> input,
                                           bg::Out<bg::TS<bg::i64>> output) {
        if (input.valid()) {
            output.set(input.contains(1) && !input[1].valid() ? 1 : 0);
        }
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return observe_child(delay(add_key(input))); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 1>{1});

    CHECK(actual.equals({std::nullopt, 1}));
}

TEST_CASE("a node delays a new TSD key with an invalid TSD child",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:addition]") {
    const auto add_key = bg::node([](bg::In<bg::TS<bg::i64>> trigger,
                                     bg::Out<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        if (trigger.modified()) {
            (void)output[1];
        }
    });
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>>>();
    const auto observe_child = bg::node([](bg::In<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> input,
                                           bg::Out<bg::TS<bg::i64>> output) {
        if (input.valid()) {
            output.set(input.contains(1) && !input[1].valid() ? 1 : 0);
        }
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return observe_child(delay(add_key(input))); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 1>{1});

    CHECK(actual.equals({std::nullopt, 1}));
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
    const auto delay = bg::node<one_tick_delay_definition<bg::TSS<bg::i64>>>();
    const auto sum = bg::node<set_sum_definition>();
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return sum(delay(input)); };

    const auto actual = bg::testing::test_graph(
        graph, {{1}, {literal::REMOVED(1), 2}, literal::SKIP, {literal::REMOVED(2), 3}, literal::SKIP});

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

TEST_CASE("a first fixed list tick can contain a never-valid item",
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
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TS<bg::i64>>>>();
    const auto sum = bg::node<dictionary_sum_definition>();
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return sum(delay(input)); };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, 1}}, {{1, 2}}, {{1, literal::REMOVE}}, literal::SKIP});

    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("a node delays nested dictionary changes by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:addition][transition:update][transition:removal]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>>>();
    const auto sum = bg::node<nested_dictionary_sum_definition>();
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> input) {
        return sum(delay(input));
    };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, {{2, 1}}}}, {{1, {{2, 2}}}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals({std::nullopt, 1, 2, 0}));
}

TEST_CASE("a node delays changes to nested dictionary keys by one evaluation cycle",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:tsd][scalar:i64][shape:chain][transition:addition][transition:removal]") {
    const auto delay = bg::node<one_tick_delay_definition<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>>>();
    const auto sum = bg::node<nested_dictionary_sum_definition>();
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> input) {
        return sum(delay(input));
    };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, {{10, 5}}}}, {{1, {{10, literal::REMOVE}, {20, 7}}}}});

    CHECK(actual.equals({std::nullopt, 5, 7}));
}

TEST_CASE("discarding an unread tick record releases its scalar payload", "[coverage-exempt]") {
    retained_value_memory.allocations = 0;
    retained_value_memory.deallocations = 0;

    {
        auto value_memory = bg::allocator{retained_value_memory};
        const auto buffer = bg::util::buffer::chain(256);
        auto writer = bg::TimeSeriesTickWriter{buffer, value_memory};

        {
            const auto value = bg::ScalarValue<bg::string>{
                std::string{"a retained string payload long enough to require allocated storage"}, value_memory};
            const auto scalar = value.view();
            const auto time = bg::MIN_DT + std::chrono::microseconds{1};
            const auto last_updated = bg::TimeSeriesState{time};
            const auto input_data = bg::TimeSeriesInputData{
                bg::cpp_time_series_runtime_type<bg::TS<bg::string>>(), last_updated, scalar.data(), true};
            const auto input = bg::In<bg::TS<bg::string>>{input_data};

            REQUIRE(writer.write(input, time));
            REQUIRE(writer.write(input, time));
        }

        CHECK(retained_value_memory.allocations > 0);
        CHECK(retained_value_memory.deallocations == 0);

        {
            auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::string>::type()};
            CHECK(reader.discard());
            CHECK(retained_value_memory.deallocations == 0);
        }

        CHECK(retained_value_memory.allocations == retained_value_memory.deallocations);
    }

    CHECK(retained_value_memory.allocations == retained_value_memory.deallocations);
}

TEST_CASE("destroying a tick reader releases unread scalar payloads", "[coverage-exempt]") {
    retained_value_memory.allocations = 0;
    retained_value_memory.deallocations = 0;

    {
        auto value_memory = bg::allocator{retained_value_memory};
        const auto buffer = bg::util::buffer::chain(256);
        auto writer = bg::TimeSeriesTickWriter{buffer, value_memory};

        {
            const auto value = bg::ScalarValue<bg::string>{
                std::string{"a retained string payload long enough to require allocated storage"}, value_memory};
            const auto scalar = value.view();
            const auto time = bg::MIN_DT + std::chrono::microseconds{1};
            const auto last_updated = bg::TimeSeriesState{time};
            const auto input_data = bg::TimeSeriesInputData{
                bg::cpp_time_series_runtime_type<bg::TS<bg::string>>(), last_updated, scalar.data(), true};
            const auto input = bg::In<bg::TS<bg::string>>{input_data};

            REQUIRE(writer.write(input, time));
            REQUIRE(writer.write(input, time));
        }

        CHECK(retained_value_memory.allocations > 0);
        CHECK(retained_value_memory.deallocations == 0);

        { auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<bg::string>::type()}; }
        CHECK(retained_value_memory.allocations == retained_value_memory.deallocations);
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

TEST_CASE("a queue message writer frames and replays scalar literals",
          "[feature:time-series-storage][series:value][scalar:i64]") {
    const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    auto writer = bg::details::QueueMessageWriter{
        buffer, 17, runtime_type};

    REQUIRE(writer.write(literal::TimeSeriesLiteral(42)));

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    const auto frame = reader.read<bg::details::QueueMessageHeader>();
    const auto unaligned_size = sizeof(bg::details::QueueMessageHeader) + 1 + sizeof(bg::i64);
    const auto expected_size = (unaligned_size + alignof(bg::details::QueueMessageHeader) - 1) /
                              alignof(bg::details::QueueMessageHeader) * alignof(bg::details::QueueMessageHeader);
    CHECK(frame.target == 17);
    CHECK(frame.size == expected_size);
    const auto payload = reader.current_buffer().chunk(0, 1 + sizeof(bg::i64));
    const auto header = bg::details::TickHeader::read(reader);
    CHECK(header.operation == bg::details::TickOperation::tick);
    CHECK(header.valid);
    CHECK(reader.read_raw<bg::i64>() == 42);
    CHECK(reader.readable().size() == frame.size - sizeof(frame) - 1 - sizeof(bg::i64));
    for (const auto value : reader.readable()) {
        CHECK(value == std::byte{});
    }

    const auto replay_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto output_state = bg::TimeSeriesState{};
    auto output_value = bg::i64{};
    auto output_data = bg::OutputState{runtime_type};
    output_data.bind(output_state, &output_value, replay_time);
    auto output = bg::Out<bg::TS<bg::i64>>{bg::TimeSeriesOutputBinding{output_data}};
    auto tick_reader = bg::TimeSeriesTickReader{payload, bg::TS<bg::i64>::type()};

    REQUIRE(tick_reader.replay(output));
    CHECK(output.value() == 42);
    CHECK_FALSE(tick_reader.has_next());
}

TEST_CASE("scalar buffer projection tracks aligned fields from a nonzero position",
          "[feature:scalar-storage]") {
    struct alignas(std::max_align_t) aligned_value {
        std::byte bytes[3];
    };
    struct alignas(2 * alignof(std::max_align_t)) over_aligned_value {};

    auto projection = bg::ScalarBufferProjectionContext{3};
    projection.write_bytes(5);
    const auto aligned_position =
        (projection.position() + alignof(aligned_value) - 1) / alignof(aligned_value) * alignof(aligned_value);
    projection.emplace<aligned_value>();
    projection.write_bytes(2);

    CHECK(projection.position() == aligned_position + sizeof(aligned_value) + 2);
    CHECK(projection.size() == projection.position() - 3);

    auto overflow = bg::ScalarBufferProjectionContext{std::numeric_limits<std::size_t>::max() - 1};
    CHECK_THROWS_AS(overflow.write_bytes(2), std::length_error);
    CHECK(overflow.position() == std::numeric_limits<std::size_t>::max() - 1);
    CHECK_THROWS_AS(overflow.emplace<over_aligned_value>(), std::invalid_argument);
}

TEST_CASE("a queue message writer stores a scalar sequence as one owning value",
          "[feature:time-series-storage][series:value][scalar:list]") {
    using List = bg::List<bg::i64>;
    const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<List>>();
    auto writer = bg::details::QueueMessageWriter{buffer, 23, runtime_type};

    REQUIRE(writer.write(literal::TimeSeriesLiteral{3, 5, 8}));

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    const auto frame = reader.read<bg::details::QueueMessageHeader>();
    CHECK(frame.target == 23);
    constexpr auto value_offset = (1 + alignof(bg::ScalarValue<>) - 1) / alignof(bg::ScalarValue<>) *
                                  alignof(bg::ScalarValue<>);
    const auto payload_size = value_offset + sizeof(bg::ScalarValue<>);
    const auto payload = reader.current_buffer().chunk(0, payload_size);

    const auto replay_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto output_state = bg::TimeSeriesState{};
    auto output_value = bg::scalar_storage_traits<List>::stored_type{};
    auto output_data = bg::OutputState{runtime_type};
    output_data.bind(output_state, &output_value, replay_time);
    auto output = bg::Out<bg::TS<List>>{bg::TimeSeriesOutputBinding{output_data}};
    auto tick_reader = bg::TimeSeriesTickReader{payload, bg::TS<List>::type()};

    auto memory = bg::allocator{};
    auto *allocation_owner = bg::details::BundleAllocationOwner::create(memory);
    auto release_allocation_owner = bg::util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
    {
        const auto allocation_context = bg::details::ScalarAllocationContext{memory, *allocation_owner, &buffer, 0};
        auto output_initialized = false;
        auto destroy_output = bg::util::scope_exit{[&]() noexcept {
            if (output_initialized) {
                bg::cpp_scalar_runtime_type<List>().destroy(&output_value, memory);
            }
        }};
        REQUIRE(tick_reader.replay(output));
        output_initialized = true;
        const auto value = bg::Scalar<List>::from(output_value);
        REQUIRE(value.size() == 3);
        CHECK(value[0].value() == 3);
        CHECK(value[1].value() == 5);
        CHECK(value[2].value() == 8);
        CHECK_FALSE(tick_reader.has_next());
        CHECK(frame.size >= sizeof(frame) + payload_size);
    }
}

TEST_CASE("queue message writers skip root SKIP and reject invalid targets and markers",
          "[feature:time-series-storage]") {
    const auto buffer = bg::util::buffer::concurrent_chain(128, 2);
    auto writer = bg::details::QueueMessageWriter{
        buffer, 1, bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()};

    CHECK_FALSE(writer.write(literal::TimeSeriesLiteral(literal::SKIP)));
    CHECK(bg::util::concurrent_buffer_reader{buffer}.readable().empty());
    CHECK_THROWS_AS(writer.write(literal::TimeSeriesLiteral(literal::REMOVE)), std::invalid_argument);
    CHECK(bg::util::concurrent_buffer_reader{buffer}.readable().empty());
    CHECK_THROWS_AS((bg::details::QueueMessageWriter{
                        buffer, std::numeric_limits<std::uint32_t>::max(),
                        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()}),
                    std::invalid_argument);

    const auto reference_buffer = bg::util::buffer::concurrent_chain(128, 2);
    auto reference_writer = bg::details::QueueMessageWriter{
        reference_buffer, 1,
        bg::cpp_time_series_runtime_type<bg::TSL<bg::REF<bg::TS<bg::i64>>, 2>>()};
    CHECK_THROWS_AS(reference_writer.write(literal::TimeSeriesLiteral{{0, literal::INVALID}}), std::invalid_argument);
    CHECK(bg::util::concurrent_buffer_reader{reference_buffer}.readable().empty());
    CHECK_THROWS_AS((bg::details::QueueMessageWriter{
                        buffer, std::uint64_t{1} + std::numeric_limits<std::uint32_t>::max(),
                        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()}),
                    std::invalid_argument);
}

TEST_CASE("queue message projection rejects malformed collection selectors",
          "[feature:time-series-storage][series:fixed-tsl][series:tsb]") {
    const auto list_buffer = bg::util::buffer::concurrent_chain(128, 2);
    auto list_writer = bg::details::QueueMessageWriter{
        list_buffer, 1, bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 2>>()};

    CHECK_THROWS_AS(list_writer.write(literal::TimeSeriesLiteral{{0, 1}, {0, 2}}), std::invalid_argument);
    CHECK_THROWS_AS(list_writer.write(literal::TimeSeriesLiteral{{2, 1}}), std::invalid_argument);
    CHECK(bg::util::concurrent_buffer_reader{list_buffer}.readable().empty());

    const auto bundle_buffer = bg::util::buffer::concurrent_chain(128, 2);
    auto bundle_writer = bg::details::QueueMessageWriter{
        bundle_buffer, 2, bg::cpp_time_series_runtime_type<bg::TSB<TickPair>>()};
    CHECK_THROWS_AS(bundle_writer.write(literal::TimeSeriesLiteral{{"missing", 1}}), std::invalid_argument);
    CHECK(bg::util::concurrent_buffer_reader{bundle_buffer}.readable().empty());
}

TEST_CASE("queue message nested dictionaries replay into output storage",
          "[evaluation][feature:test-graph][feature:time-series-storage][series:tsd][scalar:i64]") {
    const auto replay = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                    bg::Out<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        const auto &runtime_type =
            bg::cpp_time_series_runtime_type<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>>();
        const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
        auto writer = bg::details::QueueMessageWriter{buffer, 31, runtime_type};
        if (!writer.write(literal::TimeSeriesLiteral{{1, {{2, *input}}}})) {
            return;
        }

        auto reader = bg::util::concurrent_buffer_reader{buffer};
        const auto frame = reader.read<bg::details::QueueMessageHeader>();
        if (frame.target != 31) {
            return;
        }
        const auto body = reader.current_buffer().chunk(0, frame.size - sizeof(frame));
        auto measure = bg::util::buffer_reader{body};
        bg::details::TimeSeriesTickDiscard{measure, bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance()}
            .apply(runtime_type.type());
        const auto payload_size = body.size() - measure.current_buffer().size();
        auto tick_reader = bg::TimeSeriesTickReader{body.chunk(0, payload_size), runtime_type.type()};
        tick_reader.replay(output);
    });
    const auto sum = bg::node<nested_dictionary_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return sum(replay(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 1>{22}).equals({22}));
}

TEST_CASE("a queue message writer preserves collection order and TSD tick operations",
          "[feature:time-series-storage][series:fixed-tsl][series:tsd]") {
    const auto discard_following_frame = [](bg::util::concurrent_buffer_reader &reader, std::uint32_t target,
                                            std::size_t first_frame_size, const bg::TimeSeriesType &type) {
        const auto first_body = reader.current_buffer();
        auto measurement = bg::util::buffer_reader{first_body};
        bg::details::TimeSeriesTickDiscard{measurement, bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance()}
            .apply(type);
        const auto payload_size = first_body.size() - measurement.current_buffer().size();

        reader.consume(first_frame_size - sizeof(bg::details::QueueMessageHeader));
        const auto second_frame = reader.read<bg::details::QueueMessageHeader>();
        CHECK(second_frame.target == target);
        const auto second_payload = reader.current_buffer().chunk(0, payload_size);
        auto tick_reader = bg::TimeSeriesTickReader{second_payload, type};
        CHECK(tick_reader.discard());
        CHECK_FALSE(tick_reader.has_next());
        reader.consume(second_frame.size - sizeof(second_frame));
    };

    const auto list_buffer = bg::util::buffer::concurrent_chain(256, 4);
    auto list_writer = bg::details::QueueMessageWriter{
        list_buffer, 2, bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 2>>()};

    REQUIRE(list_writer.write(literal::TimeSeriesLiteral{{1, 11}, {0, literal::INVALID}}));
    REQUIRE(list_writer.write(literal::TimeSeriesLiteral{{1, 11}, {0, literal::INVALID}}));

    auto list_reader = bg::util::concurrent_buffer_reader{list_buffer};
    const auto list_frame = list_reader.read<bg::details::QueueMessageHeader>();
    CHECK(list_frame.target == 2);
    const auto list_body = list_reader.current_buffer();
    auto list_payload_reader = bg::util::buffer_reader{list_body};
    CHECK(bg::details::TickHeader::read(list_payload_reader).valid);
    const auto first = bg::details::TickHeader::read(list_payload_reader);
    CHECK(first.operation == bg::details::TickOperation::tick);
    CHECK(first.valid);
    CHECK(list_payload_reader.read_raw<bg::item_id_t>() == 1);
    CHECK(list_payload_reader.read_raw<bg::i64>() == 11);
    const auto second = bg::details::TickHeader::read(list_payload_reader);
    CHECK(second.operation == bg::details::TickOperation::tick);
    CHECK_FALSE(second.valid);
    CHECK(list_payload_reader.read_raw<bg::item_id_t>() == 0);
    CHECK(bg::details::TickHeader::read(list_payload_reader).operation == bg::details::TickOperation::end);
    CHECK(list_frame.size >= sizeof(list_frame) + 1 + 2 * (1 + sizeof(bg::item_id_t)) + sizeof(bg::i64) + 1);
    discard_following_frame(list_reader, 2, list_frame.size, bg::TSL<bg::TS<bg::i64>, 2>::type());

    const auto dict_buffer = bg::util::buffer::concurrent_chain(256, 4);
    auto dict_writer = bg::details::QueueMessageWriter{
        dict_buffer, 3, bg::cpp_time_series_runtime_type<bg::TSD<bg::i64, bg::TS<bg::i64>>>()};

    REQUIRE(dict_writer.write(literal::TimeSeriesLiteral{{4, 44}, {7, literal::INVALID}}));
    REQUIRE(dict_writer.write(literal::TimeSeriesLiteral{{4, 44}, {7, literal::INVALID}}));

    auto dict_reader = bg::util::concurrent_buffer_reader{dict_buffer};
    const auto dict_frame = dict_reader.read<bg::details::QueueMessageHeader>();
    CHECK(dict_frame.target == 3);
    const auto dict_body = dict_reader.current_buffer();
    auto dict_payload_reader = bg::util::buffer_reader{dict_body};
    CHECK(bg::details::TickHeader::read(dict_payload_reader).valid);
    const auto value_entry = bg::details::TickHeader::read(dict_payload_reader);
    CHECK(value_entry.operation == bg::details::TickOperation::tick);
    CHECK(value_entry.valid);
    CHECK(dict_payload_reader.read_raw<bg::i64>() == 4);
    CHECK(dict_payload_reader.read_raw<bg::i64>() == 44);
    const auto invalid_entry = bg::details::TickHeader::read(dict_payload_reader);
    CHECK(invalid_entry.operation == bg::details::TickOperation::tick);
    CHECK_FALSE(invalid_entry.valid);
    CHECK(dict_payload_reader.read_raw<bg::i64>() == 7);
    CHECK(bg::details::TickHeader::read(dict_payload_reader).operation == bg::details::TickOperation::end);
    discard_following_frame(dict_reader, 3, dict_frame.size, bg::TSD<bg::i64, bg::TS<bg::i64>>::type());
}

TEST_CASE("queue message collection payloads use the tick reader discard grammar",
          "[feature:time-series-storage][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    const auto verify = [](const bg::TimeSeriesType &type, const literal::TimeSeriesLiteral &value) {
        const auto buffer = bg::util::buffer::concurrent_chain(512, 4);
        auto writer = bg::details::QueueMessageWriter{buffer, 6, bg::resolve_time_series_runtime_type(type)};
        REQUIRE(writer.write(value));
        REQUIRE(writer.write(value));

        auto reader = bg::util::concurrent_buffer_reader{buffer};
        const auto frame = reader.read<bg::details::QueueMessageHeader>();
        CHECK(frame.target == 6);
        const auto body = reader.current_buffer().chunk(0, frame.size - sizeof(frame));
        auto measure = bg::util::buffer_reader{body};
        bg::details::TimeSeriesTickDiscard{measure, bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance()}
            .apply(type);
        const auto payload_size = body.size() - measure.current_buffer().size();

        reader.consume(frame.size - sizeof(frame));
        const auto next_frame = reader.read<bg::details::QueueMessageHeader>();
        CHECK(next_frame.target == 6);
        auto tick_reader = bg::TimeSeriesTickReader{reader.current_buffer().chunk(0, payload_size), type};
        CHECK(tick_reader.discard());
        CHECK_FALSE(tick_reader.has_next());
    };

    verify(bg::TSL<bg::TS<bg::i64>, 2>::type(), literal::TimeSeriesLiteral{{0, literal::SKIP}, {1, 11}});
    const auto removed_key = literal::ScalarLiteral(2);
    verify(bg::TSS<bg::i64>::type(), literal::TimeSeriesLiteral{1, literal::REMOVED(removed_key)});
    verify(bg::TSD<bg::i64, bg::TS<bg::i64>>::type(),
           literal::TimeSeriesLiteral{{4, 44}, {7, literal::REMOVE}, {9, literal::INVALID}});
    verify(bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>::type(),
           literal::TimeSeriesLiteral{{1, {{2, 22}}}});
    verify(bg::TSB<TickPair>::type(), literal::TimeSeriesLiteral{{"first", 10}, {"second", 20}});
}

TEST_CASE("queue message writes clean up a partial typed payload and commit a sentinel frame",
          "[feature:time-series-storage][series:fixed-tsl][scalar:string]") {
    allocation_counter resource;
    auto value_memory = bg::allocator{resource};
    const auto buffer = bg::util::buffer::concurrent_chain(1, 1);
    auto writer = bg::details::QueueMessageWriter{
        buffer, 5, bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::string>, 2>>(), value_memory};

    CHECK_THROWS_AS(writer.write(literal::TimeSeriesLiteral{
                        {0, "a long owned string value that requires allocated scalar storage"}, {1, 42}}),
                    std::invalid_argument);
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);

    REQUIRE(writer.write(literal::TimeSeriesLiteral{
        {0, "another long owned string value that requires allocated scalar storage"}}));

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    const auto failed_frame = reader.read<bg::details::QueueMessageHeader>();
    CHECK(failed_frame.target == std::numeric_limits<std::uint32_t>::max());
    CHECK(failed_frame.size > sizeof(failed_frame));
    reader.consume(failed_frame.size - sizeof(failed_frame));
    const auto valid_frame = reader.read<bg::details::QueueMessageHeader>();
    CHECK(valid_frame.target == 5);
    bg::details::TimeSeriesTickDiscard{reader, bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance()}
        .apply(bg::TSL<bg::TS<bg::string>, 2>::type());
    CHECK(resource.allocations > 0);
    reader.consume(reader.readable().size());
    CHECK(resource.allocations == resource.deallocations);
    CHECK(valid_frame.size > sizeof(valid_frame));
}

TEST_CASE("queue message writers roll over with a one-slot concurrent buffer",
          "[feature:time-series-storage]") {
    const auto buffer = bg::util::buffer::concurrent_chain(1, 1);
    auto writer = bg::details::QueueMessageWriter{
        buffer, 9, bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()};

    REQUIRE(writer.write(literal::TimeSeriesLiteral(1)));
    REQUIRE(writer.write(literal::TimeSeriesLiteral(2)));
    REQUIRE(writer.write(literal::TimeSeriesLiteral(3)));

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    const auto unaligned_size = sizeof(bg::details::QueueMessageHeader) + 1 + sizeof(bg::i64);
    const auto expected_size = (unaligned_size + alignof(bg::details::QueueMessageHeader) - 1) /
                              alignof(bg::details::QueueMessageHeader) * alignof(bg::details::QueueMessageHeader);
    for (const auto expected : std::array<bg::i64, 3>{1, 2, 3}) {
        const auto frame = reader.read<bg::details::QueueMessageHeader>();
        CHECK(frame.target == 9);
        CHECK(frame.size == expected_size);
        const auto header = bg::details::TickHeader::read(reader);
        CHECK(header.operation == bg::details::TickOperation::tick);
        CHECK(header.valid);
        CHECK(reader.read_raw<bg::i64>() == expected);
        reader.consume(frame.size - sizeof(frame) - 1 - sizeof(bg::i64));
    }
    CHECK(reader.readable().empty());
}

TEST_CASE("concurrent queue message writers preserve each writer's order",
          "[feature:time-series-storage][series:value][scalar:i64]") {
    const auto buffer = bg::util::buffer::concurrent_chain(1, 1);
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    auto first_writer = bg::details::QueueMessageWriter{buffer, 41, runtime_type};
    auto second_writer = bg::details::QueueMessageWriter{buffer, 42, runtime_type};
    constexpr auto value_count = 32;
    auto start = std::barrier{3};
    auto first_ok = true;
    auto second_ok = true;

    auto first_thread = std::thread{[&] {
        start.arrive_and_wait();
        for (auto index = 0; index < value_count; ++index) {
            first_ok = first_ok && first_writer.write(literal::TimeSeriesLiteral(10 + index));
        }
    }};
    auto second_thread = std::thread{[&] {
        start.arrive_and_wait();
        for (auto index = 0; index < value_count; ++index) {
            second_ok = second_ok && second_writer.write(literal::TimeSeriesLiteral(20 + index));
        }
    }};
    start.arrive_and_wait();
    first_thread.join();
    second_thread.join();
    CHECK(first_ok);
    CHECK(second_ok);

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    auto first_values = std::vector<bg::i64>{};
    auto second_values = std::vector<bg::i64>{};
    const auto unaligned_size = sizeof(bg::details::QueueMessageHeader) + 1 + sizeof(bg::i64);
    const auto expected_size = (unaligned_size + alignof(bg::details::QueueMessageHeader) - 1) /
                              alignof(bg::details::QueueMessageHeader) * alignof(bg::details::QueueMessageHeader);
    while (!reader.readable().empty()) {
        const auto frame = reader.read<bg::details::QueueMessageHeader>();
        CHECK(frame.size == expected_size);
        const auto header = bg::details::TickHeader::read(reader);
        REQUIRE(header.operation == bg::details::TickOperation::tick);
        REQUIRE(header.valid);
        const auto value = reader.read_raw<bg::i64>();
        if (frame.target == 41) {
            first_values.push_back(value);
        } else {
            REQUIRE(frame.target == 42);
            second_values.push_back(value);
        }
        reader.consume(frame.size - sizeof(frame) - 1 - sizeof(bg::i64));
    }

    auto expected_first = std::vector<bg::i64>{};
    auto expected_second = std::vector<bg::i64>{};
    for (auto index = 0; index < value_count; ++index) {
        expected_first.push_back(10 + index);
        expected_second.push_back(20 + index);
    }
    CHECK(first_values == expected_first);
    CHECK(second_values == expected_second);
}

#include "test_scalar_provider.hpp"

#include <bg/scalar/value.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <concepts>
#include <memory>
#include <memory_resource>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Value = test_scalar_provider::value;
using ReadValue = test_scalar_provider::value_view;
using Storage = test_scalar_provider::storage;
using CopyOnlyValue = test_scalar_provider::copy_only_value;

class LegacyIntegerRuntimeType final : public bg::ScalarRuntimeType {
  public:
    LegacyIntegerRuntimeType()
        : ScalarRuntimeType{bg::cpp_scalar_type<bg::i64>(), sizeof(bg::i64), alignof(bg::i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::i64 *>(destination));
    }

    void read_value(bg::ScalarDataMode, std::uintptr_t data, void *destination) const override {
        std::construct_at(static_cast<bg::i64 *>(destination), *reinterpret_cast<const bg::i64 *>(data));
    }

    void copy_construct(void *destination, const void *source, bg::allocator &) const override {
        std::construct_at(static_cast<bg::i64 *>(destination), *static_cast<const bg::i64 *>(source));
    }

    std::size_t hash(const void *value) const noexcept override {
        return std::hash<bg::i64>{}(*static_cast<const bg::i64 *>(value));
    }

    void destroy(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<bg::i64 *>(value));
    }

  private:
    void do_construct_from_input(void *destination, const bg::ScalarData &source, bg::allocator &) const override {
        std::construct_at(static_cast<bg::i64 *>(destination), bg::details::scalar_data_value<bg::i64>(source));
    }

    void do_assign_from_input(void *destination, const bg::ScalarData &source) const override {
        *static_cast<bg::i64 *>(destination) = bg::details::scalar_data_value<bg::i64>(source);
    }

    bool equals_same_type(const void *value, const bg::ScalarData &other) const noexcept override {
        return *static_cast<const bg::i64 *>(value) == bg::details::scalar_data_value<bg::i64>(other);
    }
};

const LegacyIntegerRuntimeType &legacy_integer_runtime() {
    static const auto runtime = LegacyIntegerRuntimeType{};
    return runtime;
}

struct ProviderScalarAggregateReader {
    static constexpr bool schedule_on_start = true;

    bg::Scalar<Value> first;
    bg::Scalar<Value> second;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(first.value().get() + second.value().get()); }
};

struct MixedProviderPromotedReader {
    static constexpr bool schedule_on_start = true;

    Value first;
    Value second;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(first.number + second.number); }
};

struct OptionalProviderPromotedReader {
    static constexpr bool schedule_on_start = true;

    std::optional<Value> value;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(value ? value->number : 0); }
};

struct CopyOnlyProviderAggregateReader {
    static constexpr bool schedule_on_start = true;

    bg::Scalar<CopyOnlyValue> view;
    CopyOnlyValue promoted;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(view.value().number + promoted.number); }
};

static_assert(bg::AggregateNodeClass<ProviderScalarAggregateReader>);
static_assert(bg::AggregateNodeClass<MixedProviderPromotedReader>);
static_assert(bg::AggregateNodeClass<OptionalProviderPromotedReader>);
static_assert(bg::AggregateNodeClass<CopyOnlyProviderAggregateReader>);

} // namespace

TEST_CASE("a scalar provider read type controls typed, erased, and time series reads",
          "[feature:scalar-api][feature:scalar-storage][series:value][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(std::same_as<typename bg::scalar_storage_traits<Value>::stored_type, Storage>);
    STATIC_CHECK(std::same_as<typename bg::scalar_storage_traits<Value>::value_type, ReadValue>);
    STATIC_CHECK(std::same_as<decltype(std::declval<const bg::Scalar<Value> &>().value()), ReadValue>);
    STATIC_CHECK(std::same_as<decltype(std::declval<const bg::In<bg::TS<Value>> &>().value()), ReadValue>);

    auto storage = Storage{42};
    const auto typed = bg::Scalar<Value>::from(storage);
    const auto typed_read = typed.value();
    CHECK(typed_read.get() == 42);
    CHECK(typed_read.number == std::addressof(storage.number));

    const auto owned = bg::ScalarValue<Value>{typed};
    storage.number = 7;
    const auto owned_read = owned.view().value();
    CHECK(owned_read.get() == 42);
    CHECK(owned_read.number != std::addressof(storage.number));

    const auto erased = bg::ScalarValue<>{owned.view()};
    const auto erased_read = bg::unchecked_scalar_cast<Value>(erased.view()).value();
    CHECK(erased_read.get() == 42);
    CHECK(erased_read.number != owned_read.number);

    const auto state = bg::TimeSeriesState{bg::MIN_DT + bg::EvaluationTime::duration{1}};
    const auto input = bg::In<bg::TS<Value>>{state, std::addressof(storage)};
    const auto input_read = input.value();
    CHECK(input_read.get() == 7);
    CHECK(input_read.number == std::addressof(storage.number));

    const auto generic = bg::Scalar<bg::SCALAR>::from(owned);
    auto generic_read = false;
    generic.visit(
        [&](const bg::Scalar<> *value) {
            STATIC_CHECK(std::same_as<decltype(bg::unchecked_scalar_cast<Value>(*value).value()), ReadValue>);
            CHECK(bg::unchecked_scalar_cast<Value>(*value).value().get() == 42);
            generic_read = true;
        },
        ddv::noop);
    CHECK(generic_read);
}

TEST_CASE("a scalar provider can own a value through its borrowed read view",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(std::constructible_from<bg::ScalarValue<Value>, test_scalar_provider::explicit_value_source>);
    STATIC_CHECK(!noexcept(bg::details::make_scalar_source<Value>(std::declval<const ReadValue &>())));

    const auto owned = [] {
        const auto source = test_scalar_provider::explicit_value_source{.number = 42};
        return bg::ScalarValue<Value>{source};
    }();

    CHECK(&owned.runtime_type() == &bg::cpp_scalar_runtime_type<Value>());
    CHECK(&bg::resolve_scalar_runtime_type(bg::cpp_scalar_type<Value>(), {}) ==
          &bg::cpp_scalar_runtime_type<Value>());
    CHECK(owned.view().value().get() == 42);

    const auto empty_view = ReadValue{};
    CHECK_THROWS_WITH(bg::details::make_scalar_source<Value>(empty_view),
                      "a provider value view must have a source");
}

TEST_CASE("a scalar provider canonical runtime must match its storage layout",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(!noexcept(bg::details::make_scalar_source<test_scalar_provider::layout_value>(
        std::declval<const test_scalar_provider::layout_value &>())));

    CHECK_THROWS_WITH(bg::cpp_scalar_runtime_type<test_scalar_provider::layout_value>(),
                      "a scalar provider runtime must match its C++ storage layout");

    const auto value = test_scalar_provider::layout_value{.number = 3};
    CHECK_THROWS_WITH(bg::details::make_scalar_source<test_scalar_provider::layout_value>(value),
                      "a scalar provider runtime must match its C++ storage layout");
}

TEST_CASE("a scalar provider validates semantic type before selecting a copy runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    const auto value = bg::i64{7};
    const auto incompatible = bg::Scalar<Value>::from(bg::cpp_scalar_runtime_type<bg::i64>(), &value);
    const auto copy_calls = test_scalar_provider::copy_runtime_type_call_count();

    CHECK_THROWS_AS(bg::ScalarValue<Value>{incompatible}, std::invalid_argument);
    CHECK(test_scalar_provider::copy_runtime_type_call_count() == copy_calls);
}

TEST_CASE("a scalar provider read type controls collection aliases and reads",
          "[feature:scalar-collection][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(std::same_as<bg::List<Value>::value_type, ReadValue>);
    STATIC_CHECK(std::same_as<bg::Set<Value>::value_type, ReadValue>);
    STATIC_CHECK(std::same_as<bg::Dict<bg::i64, Value>::mapped_value_type, ReadValue>);

    const auto list = bg::ScalarValue<bg::List<Value>>{std::vector<Value>{{1}}};
    CHECK(list.view()[0].value().get() == 1);

    const auto dictionary = bg::ScalarValue<bg::Dict<bg::i64, Value>>{std::vector<std::pair<bg::i64, Value>>{{3, {9}}}};
    CHECK(dictionary.view().at(bg::i64{3}).value().get() == 9);
}

TEST_CASE("a scalar provider read type passes through a time series output",
          "[feature:test-graph][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:concrete][shape:single-node][binding:direct][transition:first-tick]") {
    const auto copy =
        bg::node([](bg::In<bg::TS<Value>> input, bg::Out<bg::TS<Value>> output) { output.set(input.value()); });
    const auto graph = [&](bg::wp<bg::TS<Value>> input) { return copy(input); };
    const auto source = std::vector<Value>{{2}, {5}};

    const auto result = bg::testing::test_graph(graph, source);

    REQUIRE(result[0]);
    REQUIRE(result[1]);
    CHECK(result[0]->value().get() == 2);
    CHECK(result[1]->value().get() == 5);
}

TEST_CASE("a typed time-series output reads its provider value",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:concrete]") {
    using Series = bg::TS<Value>;

    const auto update_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{update_time};
    auto storage = Storage{42};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<Series>()};
    state.bind(last_updated, &storage, update_time + std::chrono::microseconds{1});
    const auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{state}};

    const auto value = output.value();

    CHECK(value.get() == 42);
    CHECK(value.number == std::addressof(storage.number));
}

TEST_CASE("typed time-series leaf reads use provider hooks for alternative runtimes",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:runtime]") {
    using Series = bg::TS<Value>;

    const auto &scalar_runtime = test_scalar_provider::preserving_runtime_type();
    const auto &runtime = bg::time_series_value_runtime_type(Series::type(), scalar_runtime);
    const auto update_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{update_time};
    auto input_storage = Storage{42};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{runtime, input_state, &input_storage}};

    auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto input_value = input.value();
    CHECK(input_value.get() == 42);
    CHECK(input_value.number == std::addressof(input_storage.number));
    CHECK(test_scalar_provider::alternative_runtime_read_count() == reads_before + 1);

    auto output_state = bg::details::OutputState{runtime};
    auto output_time = bg::TimeSeriesState{update_time};
    auto output_storage = Storage{43};
    output_state.bind(output_time, &output_storage, update_time + std::chrono::microseconds{1});
    const auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};

    reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto output_value = output.value();
    CHECK(output_value.get() == 43);
    CHECK(output_value.number == std::addressof(output_storage.number));
    CHECK(test_scalar_provider::alternative_runtime_read_count() == reads_before + 1);
}

TEST_CASE("callable promoted values and aggregate scalar views preserve provider runtime reads",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][definition:function][definition:aggregate]"
          "[declaration:concrete]") {
    auto first_storage = Storage{20};
    auto second_storage = Storage{22};
    const auto first_source = bg::Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &first_storage);
    const auto second_source = bg::Scalar<Value>::from(test_scalar_provider::canonicalizing_runtime_type(),
                                                       &second_storage);
    const auto first = bg::ScalarValue<Value>{first_source};
    const auto second = bg::ScalarValue<Value>{second_source};

    const auto callable_reader = bg::node([](Value value, bg::Out<bg::TS<bg::i64>> output) {
        output.set(value.number);
    });
    const auto callable_graph = [&] { return callable_reader(first); };
    auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto callable_result = bg::testing::test_graph(callable_graph);
    REQUIRE(callable_result[0]);
    CHECK(callable_result[0]->value() == 20);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);

    const auto aggregate_reader = bg::node<ProviderScalarAggregateReader>();
    const auto aggregate_graph = [&] { return aggregate_reader(first, second); };
    reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto aggregate_result = bg::testing::test_graph(aggregate_graph);
    REQUIRE(aggregate_result[0]);
    CHECK(aggregate_result[0]->value() == 42);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);
}

TEST_CASE("aggregate promoted scalars select runtime metadata for scalar and time-series bindings",
          "[feature:node-api][feature:scalar-storage][definition:aggregate][declaration:runtime]") {
    auto storage = Storage{41};
    const auto source = bg::Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &storage);
    const auto scalar = bg::ScalarValue<Value>{source};
    const auto reader = bg::node<MixedProviderPromotedReader>();

    const auto scalar_graph = [&] { return reader(scalar, Value{1}); };
    auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto scalar_result = bg::testing::test_graph(scalar_graph);
    REQUIRE(scalar_result[0]);
    CHECK(scalar_result[0]->value() == 42);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);

    const auto time_series_graph = [&](bg::wp<bg::TS<Value>> input) { return reader(input, scalar); };
    reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto time_series_result = bg::testing::test_graph(time_series_graph, std::array{Value{3}});
    REQUIRE(time_series_result[0]);
    CHECK(time_series_result[0]->value() == 44);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);
}

TEST_CASE("aggregate scalar views and promoted inputs preserve a copy-only provider runtime",
          "[feature:node-api][feature:scalar-storage][definition:aggregate][declaration:concrete]") {
    STATIC_CHECK(sizeof(test_scalar_provider::copy_only_runtime_storage) >
                 sizeof(test_scalar_provider::copy_only_storage));

    auto storage = test_scalar_provider::copy_only_runtime_storage{
        .prefix = {0xBAD0, 0xBAD1, 0xBAD2, 0xBAD3}, .value = {.number = 20}};
    const auto source = bg::Scalar<CopyOnlyValue>::from(test_scalar_provider::preserving_copy_only_runtime_type(),
                                                        &storage);
    const auto preserved = bg::ScalarValue<CopyOnlyValue>{source};

    CHECK(&preserved.runtime_type() == &test_scalar_provider::preserving_copy_only_runtime_type());
    CHECK(preserved.view().value().number == 20);

    const auto reader = bg::node<CopyOnlyProviderAggregateReader>();
    const auto graph = [&] { return reader(preserved, preserved); };
    const auto result = bg::testing::test_graph(graph);

    REQUIRE(result[0]);
    CHECK(result[0]->value() == 40);
}

TEST_CASE("aggregate optional provider scalars handle present and absent values",
          "[feature:node-api][feature:scalar-storage][definition:aggregate][declaration:concrete]"
          "[validity:optional]") {
    const auto reader = bg::node<OptionalProviderPromotedReader>();

    const auto present_graph = [&] { return reader(std::optional<Value>{Value{17}}); };
    const auto present = bg::testing::test_graph(present_graph);
    REQUIRE(present[0]);
    CHECK(present[0]->value() == 17);

    const auto absent_graph = [&] { return reader(std::optional<Value>{}); };
    const auto absent = bg::testing::test_graph(absent_graph);
    REQUIRE(absent[0]);
    CHECK(absent[0]->value() == 0);
}

TEST_CASE("erased scalar borrowing uses the composed bundle runtime",
          "[feature:scalar-api][scalar:hierarchical][declaration:runtime][storage:variable]") {
    using Base = test_scalar_provider::bundle_provider_base_type;
    using Derived = test_scalar_provider::bundle_provider_derived_type;
    using Stored = bg::details::scalar_stored_t<Base>;

    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto memory = bg::allocator{memory_resource};
    const auto value = bg::bundle<test_scalar_provider::bundle_provider_derived>(
        memory, bg::i64{73}, bg::i64{91});
    const auto &source = bg::details::scalar_data(value);
    const auto stored = *static_cast<const Stored *>(source.data());

    const auto borrowed = bg::Scalar<>::from<Base>(stored);

    CHECK(bg::details::scalar_data(borrowed).mode() == bg::ScalarDataMode::shared_view);
    CHECK(&borrowed.runtime_type() == &value.runtime_type());
    CHECK(&borrowed.type() == &bg::cpp_scalar_type<Derived>());
    const auto view = bg::scalar_cast<Base>(borrowed);
    CHECK(view.field_count() == 2);
    CHECK(view.field_name(1) == "extra");
    CHECK(bg::scalar_cast<bg::i64>(view[0]).value() == 73);
    CHECK(bg::scalar_cast<bg::i64>(view[1]).value() == 91);
}

TEST_CASE("a provider borrowed bundle view preserves derived identity and publishes through its output",
          "[feature:scalar-api][feature:scalar-storage][scalar:hierarchical][series:value]"
          "[declaration:runtime][storage:variable]") {
    using Base = test_scalar_provider::bundle_provider_base_type;
    using Derived = test_scalar_provider::bundle_provider_derived_type;
    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto memory = bg::allocator{memory_resource};
    const auto update_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto output_state = bg::details::OutputState{
        bg::cpp_time_series_runtime_type<bg::TS<test_scalar_provider::bundle_provider_value>>()};
    auto output_time = bg::TimeSeriesState{update_time};
    auto output_storage = bg::details::scalar_bundle_storage{};
    output_state.bind(output_time, &output_storage, update_time + std::chrono::microseconds{1});
    const auto &runtime =
        static_cast<const bg::TimeSeriesValueRuntimeType &>(output_state.runtime_type()).scalar_runtime_type();
    auto cleanup = bg::util::scope_exit{[&]() noexcept { runtime.destroy(&output_storage, memory); }};
    const auto output =
        bg::Out<bg::TS<test_scalar_provider::bundle_provider_value>>{bg::TimeSeriesOutputBinding{output_state}};
    const auto [owner, actual_type, actual_runtime] = [&] {
        const auto source = bg::bundle<test_scalar_provider::bundle_provider_derived>(
            memory, bg::i64{73}, bg::i64{91});
        const auto source_view = source.view();
        const auto view = test_scalar_provider::bundle_provider_view{
            .value = bg::Scalar<Base>::from(source_view),
        };
        const auto *type = &source.type();
        const auto *source_runtime = &source.runtime_type();
        output.set(view);
        auto owner = bg::ScalarValue<test_scalar_provider::bundle_provider_value>{view};
        return std::tuple{std::move(owner), type, source_runtime};
    }();

    CHECK(&owner.runtime_type() == actual_runtime);
    CHECK(&owner.type() == actual_type);
    CHECK(bg::scalar_cast<bg::i64>(owner.view().value().value[1]).value() == 91);

    const auto published = bg::Scalar<Base>::from(runtime, &output_storage);
    CHECK(&published.type() == actual_type);
    CHECK(bg::scalar_cast<bg::i64>(published[1]).value() == 91);
}

TEST_CASE("a generic provider uses its borrowed source copy policy",
          "[feature:scalar-api][feature:scalar-storage][scalar:hierarchical]"
          "[declaration:generic][storage:variable]") {
    using Generic = test_scalar_provider::generic_bundle_provider_type;
    using Derived = test_scalar_provider::bundle_provider_derived_type;

    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto memory = bg::allocator{memory_resource};
    const auto generic_copy_calls = test_scalar_provider::generic_copy_runtime_type_call_count();
    const auto generic_runtime_calls = test_scalar_provider::generic_runtime_type_call_count();
    const auto [owner, actual_type, actual_runtime] = [&] {
        const auto source = bg::bundle<test_scalar_provider::bundle_provider_derived>(
            memory, bg::i64{73}, bg::i64{91});
        const auto source_view = source.view();
        auto view = test_scalar_provider::generic_bundle_provider_view{
            .value = bg::Scalar<Generic>::from(source_view),
        };
        const auto *type = &source.type();
        const auto *runtime = &source.runtime_type();
        auto owner = bg::ScalarValue<test_scalar_provider::generic_bundle_provider_value>{view};
        return std::tuple{std::move(owner), type, runtime};
    }();

    CHECK(test_scalar_provider::generic_copy_runtime_type_call_count() == generic_copy_calls + 1);
    CHECK(test_scalar_provider::generic_runtime_type_call_count() == generic_runtime_calls);
    CHECK(&owner.type() == actual_type);
    CHECK(&owner.runtime_type() == actual_runtime);

    const auto owner_view = owner.view();
    const auto resolved = bg::Scalar<Generic>::from(owner_view);
    CHECK(&resolved.type() == &bg::cpp_scalar_type<Derived>());
    CHECK(resolved.field_count() == 2);
    CHECK(resolved.field_name(1) == "extra");
    CHECK(bg::scalar_cast<bg::i64>(resolved[1]).value() == 91);
}

TEST_CASE("a deduplicated time-series output suppresses equal values and publishes changes",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:runtime][transition:update][guarantee:deduplicated]") {
    using DeduplicatedSeries = bg::TS<Value, bg::TimeSeriesGuarantee::deduplicated>;

    const auto &scalar_runtime = test_scalar_provider::preserving_runtime_type();
    const auto &deduplicated_type = DeduplicatedSeries::type();
    const auto &deduplicated_runtime =
        bg::time_series_value_runtime_type(deduplicated_type, scalar_runtime);
    const auto update_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto deduplicated_output_state = bg::details::OutputState{deduplicated_runtime};
    auto deduplicated_output_time = bg::TimeSeriesState{update_time};
    auto deduplicated_output_storage = Storage{42};
    auto requested_storage = Storage{42};
    deduplicated_output_state.bind(deduplicated_output_time, &deduplicated_output_storage,
                                   update_time + std::chrono::microseconds{1});
    const auto deduplicated_output =
        bg::Out<DeduplicatedSeries>{bg::TimeSeriesOutputBinding{deduplicated_output_state}};

    deduplicated_output.set(ReadValue{std::addressof(requested_storage.number)});

    CHECK(deduplicated_output_time.time() == update_time);
    CHECK(deduplicated_output_storage.number == 42);

    requested_storage.number = 43;
    deduplicated_output.set(ReadValue{std::addressof(requested_storage.number)});

    CHECK(deduplicated_output_time.time() == update_time + std::chrono::microseconds{1});
    CHECK(deduplicated_output_storage.number == 43);
}

TEST_CASE("an alternative runtime writes a provider read value",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    STATIC_CHECK(sizeof(test_scalar_provider::large_source_view) > sizeof(ReadValue));

    auto storage = Storage{42};
    const auto &runtime = test_scalar_provider::preserving_runtime_type();
    const auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto value = bg::Scalar<Value>::from(runtime, std::addressof(storage));

    const auto read = value.value();
    CHECK(read.get() == 42);
    CHECK(read.number == std::addressof(storage.number));
    CHECK(test_scalar_provider::alternative_runtime_read_count() == reads_before + 1);
}

TEST_CASE("a typed owner can retain an extension-selected source runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    auto storage = Storage{42};
    const auto &runtime = test_scalar_provider::preserving_runtime_type();
    const auto source = bg::Scalar<Value>::from(runtime, std::addressof(storage));
    const auto copy = bg::ScalarValue<Value>{source};

    CHECK(&copy.runtime_type() == &runtime);
    storage.number = 7;
    CHECK(copy.view().value().get() == 42);
}

TEST_CASE("a typed owner can copy an alternative runtime into the canonical runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    auto storage = Storage{42};
    const auto &source_runtime = test_scalar_provider::canonicalizing_runtime_type();
    const auto source = bg::Scalar<Value>::from(source_runtime, std::addressof(storage));
    const auto copy = bg::ScalarValue<Value>{source};

    CHECK(&copy.runtime_type() == &bg::cpp_scalar_runtime_type<Value>());
    CHECK(&copy.type() == &source.type());
    storage.number = 7;
    CHECK(copy.view().value().get() == 42);
}

TEST_CASE("a provider without source-aware hooks keeps its alternative-runtime behavior",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    const bg::i64 source_value{42};
    const auto source = bg::Scalar<bg::i64>::from(legacy_integer_runtime(), std::addressof(source_value));
    const auto copy = bg::ScalarValue<bg::i64>{source};

    CHECK(source.value() == 42);
    CHECK(&copy.runtime_type() == &bg::cpp_scalar_runtime_type<bg::i64>());
    CHECK(copy.view().value() == 42);
}

TEST_CASE("an opaque scalar containing ScalarData keeps its ordinary stored-value read",
          "[feature:scalar-api][feature:scalar-storage][scalar:opaque][storage:fixed]") {
    const bg::i64 payload{42};
    const auto descriptor = bg::details::ScalarData{bg::cpp_scalar_runtime_type<bg::i64>(), &payload};
    const auto value = bg::ScalarValue<bg::Opaque<bg::ScalarData>>{descriptor};

    CHECK(&value.view().value().runtime_type() == &bg::cpp_scalar_runtime_type<bg::i64>());
    CHECK(value.view().value().data() == &payload);
}

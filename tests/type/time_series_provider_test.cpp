#include "../value/scalar/test_scalar_provider.hpp"
#include "../utils/counting_memory_resource.hpp"
#include "test_shared_literal_sentinel.hpp"

#include <bg/feedback.hpp>
#include <bg/graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/node.hpp>
#include <bg/scalar/bundle/details/allocation_owner.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/literal_operations.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/literal_operations.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/tick_operations.hpp>
#include <bg/time_series/time_series_type_traits.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/declaration_variables.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace provider {

inline bool *view_invalidation_observer{};
inline int copied_series_count{};
inline int written_tick_count{};
inline int replayed_tick_count{};
inline int discarded_tick_count{};

struct ScalarTickState {
    int writes{};
    int reads{};
    int discards{};
    int fail_on_write{};
};

inline ScalarTickState scalar_tick_state;
inline int failed_tsd_write_count{};
inline int payload_write_count{};
inline bool incompatible_replay_called{};
inline bool throw_before_scalar_read_callback{};
inline bool throw_after_scalar_read_callback{};
inline bool throw_after_bundle_key_read_callback{};
inline int bundle_key_read_count{};
inline int bundle_key_discard_count{};

template <template <typename, auto...> typename Field> struct QueueConflictBundle {
    Field<bg::TS<bg::i64>> first;
    Field<bg::TS<test_scalar_provider::value>> later;
};

using QueueConflictSeries = bg::TSB<QueueConflictBundle>;
inline int scalar_literal_semantic_calls{};
inline int scalar_literal_runtime_calls{};
inline const literal::ScalarLiteral *expected_scalar_literal{};
inline bool scalar_literal_address_matches{};
inline int time_series_literal_semantic_calls{};
inline int time_series_literal_runtime_calls{};
inline int time_series_literal_skip_calls{};
inline int time_series_literal_invalid_calls{};

template <typename Input>
void write_tick(const Input &input, bg::ScalarBufferWriteContext &writer, bg::allocator &) {
    ++written_tick_count;
    const auto value = input.value();
    writer.write_bytes(std::as_bytes(std::span{&value, std::size_t{1}}));
}

template <typename Item>
struct Number {};

class NumberType final : public bg::ScalarType {
  public:
    explicit NumberType(const bg::ScalarType &item) : item_{&item} {}

    const bg::ScalarType &item_type() const noexcept { return *item_; }
    bool is_resolved() const noexcept override { return item_->is_resolved(); }
    const bg::Type &resolve(const bg::TypeMap &type_map) const override;

  protected:
    bool do_match(const bg::Type &supplied, bg::TypeMap &type_map) const override;

  private:
    const bg::ScalarType *item_;
};

const NumberType &number_type(const bg::ScalarType &item) {
    static auto types = std::map<const bg::ScalarType *, const NumberType *>{};
    const auto [entry, inserted] = types.try_emplace(&item, nullptr);
    if (inserted) {
        try {
            entry->second = &bg::TypeRegistry::instance().register_owned(std::make_unique<NumberType>(item));
        } catch (...) {
            types.erase(entry);
            throw;
        }
    }
    return *entry->second;
}

const bg::Type &NumberType::resolve(const bg::TypeMap &type_map) const {
    const auto *item = bg::type_cast<bg::ScalarType>(&item_type().resolve(type_map));
    if (item == nullptr) {
        throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map, bg::util::symbol{"Number"},
                                      "number item type did not resolve to a scalar type"};
    }
    return number_type(*item);
}

bool NumberType::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *number = dynamic_cast<const NumberType *>(&supplied);
    if (number == nullptr || !number->is_resolved()) {
        return false;
    }

    return item_type().match(number->item_type(), type_map);
}

template <typename Item, auto... Properties>
struct Series : bg::TimeSeriesDeclaration<Series<Item, Properties...>, Properties...> {};

using OwnedPayloadSeries = Series<Number<bool>>;
using TsdFailureSeries = Series<Number<bg::byte>>;
using FailBeforePayloadSeries = Series<Number<double>>;
using UnreadPayloadSeries = Series<Number<bg::symbol>>;
using ThrowingReplaySeries = Series<Number<bg::string>>;
using ReplayRegistrationSeries = Series<Number<bg::date_time_t>>;
using IncompatibleReplaySeries = Series<Number<bg::i64>>;
using WriteRegistrationSeries = Series<Number<bg::i64>, bg::TimeSeriesGuarantee::final>;
using RuntimeReplaySeries = Series<Number<bg::timedelta_t>, bg::TimeSeriesGuarantee::deduplicated>;
using RuntimeOutputSeries = Series<Number<bg::date_time_t>, bg::TimeSeriesGuarantee::deduplicated>;
using FeedbackSeries = Series<Number<bg::i64>, bg::TimeSeriesGuarantee::deduplicated>;

enum class OutputFormat { view };
inline constexpr auto view = OutputFormat::view;

class SeriesType final : public bg::TimeSeriesType {
  public:
    SeriesType(const bg::ScalarType &item, bg::GuaranteeParameter guarantee)
        : TimeSeriesType{guarantee}, item_{&item} {}

    const bg::ScalarType &item_type() const noexcept { return *item_; }
    bool is_resolved() const noexcept override { return item_->is_resolved() && guarantee().is_resolved(); }
    const bg::TimeSeriesType &with_guarantee(bg::TimeSeriesGuarantee guarantee) const override;
    const bg::Type &resolve(const bg::TypeMap &type_map) const override;

  protected:
    bool do_match(const bg::Type &supplied, bg::TypeMap &type_map) const override;

  private:
    const bg::ScalarType *item_;
};

const bg::TimeSeriesRuntimeType &series_runtime_type(const bg::TimeSeriesType &type);

const SeriesType &series_type(const bg::ScalarType &item, bg::TimeSeriesGuarantee guarantee) {
    using Key = std::pair<const bg::ScalarType *, bg::TimeSeriesGuarantee>;
    static auto types = std::map<Key, const SeriesType *>{};
    const auto key = Key{&item, guarantee};
    const auto [entry, inserted] = types.try_emplace(key, nullptr);
    if (inserted) {
        try {
            entry->second = &bg::TypeRegistry::instance().register_owned(std::make_unique<SeriesType>(item, guarantee));
        } catch (...) {
            types.erase(entry);
            throw;
        }
        bg::register_time_series_runtime_type(series_runtime_type(*entry->second));
    }
    return *entry->second;
}

const bg::TimeSeriesType &SeriesType::with_guarantee(bg::TimeSeriesGuarantee guarantee) const {
    return series_type(item_type(), guarantee);
}

const bg::Type &SeriesType::resolve(const bg::TypeMap &type_map) const {
    const auto *item = bg::type_cast<bg::ScalarType>(&item_type().resolve(type_map));
    if (item == nullptr) {
        throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map, bg::util::symbol{"Series"},
                                      "series item type did not resolve to a scalar type"};
    }
    return series_type(*item, guarantee().resolve(type_map));
}

bool SeriesType::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *series = dynamic_cast<const SeriesType *>(&supplied);
    if (series == nullptr || !series->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    if (!item_type().match(series->item_type(), trial) ||
        !guarantee().match(*series->guarantee().value(), trial)) {
        return false;
    }
    type_map.apply(std::move(trial));
    return true;
}

class SeriesRuntimeType final : public bg::TimeSeriesRuntimeType {
  public:
    explicit SeriesRuntimeType(const bg::TimeSeriesType &type)
        : TimeSeriesRuntimeType{type, sizeof(bg::i64), alignof(bg::i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::i64 *>(destination));
    }

    void destroy_value(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<bg::i64 *>(value));
    }

    void copy(const bg::TimeSeriesInputData &input, bg::OutputState &output) const override {
        ++copied_series_count;
        if (!input.valid()) {
            output.invalidate();
            return;
        }

        *static_cast<bg::i64 *>(output.mutable_data()) = *static_cast<const bg::i64 *>(input.data());
        output.publish();
    }
};

const bg::TimeSeriesRuntimeType &series_runtime_type(const bg::TimeSeriesType &type) {
    static auto types = std::map<const bg::TimeSeriesType *, std::unique_ptr<SeriesRuntimeType>>{};
    const auto [entry, inserted] = types.try_emplace(&type);
    if (inserted) {
        entry->second = std::make_unique<SeriesRuntimeType>(type);
    }
    return *entry->second;
}

struct SeriesViewStorage {
    bg::i64 value{};
    bg::i64 guard{};
};

bg::i64 scalar_literal_number(const literal::ScalarLiteral &literal, bg::allocator &memory) {
    const auto fields = literal.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"provider scalar literal requires a one-field sequence"};
        },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();
    if (fields.size() != 1) {
        throw std::invalid_argument{"provider scalar literal requires exactly one field"};
    }

    const auto tagged = literal::TaggedLiteral::from(fields.front());
    if (tagged) {
        if (tagged->sentinel != *test_shared_literal_sentinel::extension() ||
            &tagged->argument != test_shared_literal_sentinel::extension_argument()) {
            throw std::invalid_argument{"provider received an unsupported literal sentinel"};
        }
        const auto value = bg::ScalarValue<bg::i64>::from_literal(tagged->argument, memory);
        return value.view().value();
    }
    const auto value = bg::ScalarValue<bg::i64>::from_literal(fields.front(), memory);
    return value.view().value();
}

void construct_scalar_literal_semantic(const literal::ScalarLiteral &literal,
                                       const bg::ScalarRuntimeType &runtime_type, void *destination,
                                       bg::allocator &memory) {
    ++scalar_literal_semantic_calls;
    scalar_literal_address_matches = &literal == expected_scalar_literal;
    const auto storage =
        test_scalar_provider::storage{static_cast<int>(scalar_literal_number(literal, memory) + 10)};
    runtime_type.construct_from_input(destination, bg::Scalar<test_scalar_provider::value>::from(storage), memory);
}

void construct_scalar_literal_runtime(const literal::ScalarLiteral &literal,
                                      const bg::ScalarRuntimeType &runtime_type, void *destination,
                                      bg::allocator &memory) {
    ++scalar_literal_runtime_calls;
    scalar_literal_address_matches = &literal == expected_scalar_literal;
    const auto storage =
        test_scalar_provider::storage{static_cast<int>(scalar_literal_number(literal, memory) + 100)};
    runtime_type.construct_from_input(destination, bg::Scalar<test_scalar_provider::value>::from(storage), memory);
}

void fail_scalar_literal_construction(const literal::ScalarLiteral &, const bg::ScalarRuntimeType &, void *,
                                      bg::allocator &) {
    throw std::runtime_error{"provider scalar literal construction failed"};
}

bool register_scalar_literal_handlers() {
    static const auto registered = [] {
        auto &dispatch = bg::ScalarDispatch<bg::ScalarLiteralOperations>::instance();
        return dispatch.add(bg::cpp_scalar_type<test_scalar_provider::value>(),
                            bg::ScalarLiteralOperations{&construct_scalar_literal_semantic}) &&
               dispatch.add(test_scalar_provider::canonicalizing_runtime_type(),
                            bg::ScalarLiteralOperations{&construct_scalar_literal_runtime}) &&
               dispatch.add(test_scalar_provider::copy_only_semantic_type(),
                            bg::ScalarLiteralOperations{&fail_scalar_literal_construction});
    }();
    return registered;
}

struct SeriesLiteralUpdate {
    enum class Kind { value, skip, invalid } kind;
    bg::i64 value;
};

bg::i64 time_series_literal_number(const literal::ScalarLiteralItem &literal) {
    if (const auto tagged = literal::TaggedLiteral::from(literal)) {
        if (tagged->sentinel != *test_shared_literal_sentinel::extension() ||
            &tagged->argument != test_shared_literal_sentinel::extension_argument()) {
            throw std::invalid_argument{"provider received an unsupported literal sentinel"};
        }
        const auto value = bg::ScalarValue<bg::i64>::from_literal(tagged->argument);
        return value.view().value();
    }
    return literal.visit_scalar([](const auto *value) -> bg::i64 {
        using Value = std::remove_cvref_t<decltype(*value)>;
        if constexpr (std::same_as<Value, bg::Scalar<bg::i64>>) {
            return value->value();
        } else {
            throw std::invalid_argument{"provider time-series literal requires an i64 value"};
        }
    }).value();
}

SeriesLiteralUpdate read_series_literal(const literal::TimeSeriesLiteral &literal) {
    if (const auto sentinel = literal::Sentinel::from(literal)) {
        if (*sentinel == literal::SKIP) {
            return {SeriesLiteralUpdate::Kind::skip, 0};
        }
        if (*sentinel == literal::INVALID) {
            return {SeriesLiteralUpdate::Kind::invalid, 0};
        }
        if (*sentinel == literal::REMOVE) {
            throw std::invalid_argument{"REMOVE is not valid for a provider scalar time series"};
        }
        if (*sentinel == literal::REMOVED) {
            throw std::invalid_argument{"REMOVED is not valid for a provider scalar time series"};
        }
        throw std::invalid_argument{"provider scalar time series literal must contain one item"};
    }
    if (const auto tagged = literal::TaggedLiteral::from(literal)) {
        if (tagged->sentinel != *test_shared_literal_sentinel::extension() ||
            &tagged->argument != test_shared_literal_sentinel::extension_argument()) {
            throw std::invalid_argument{"provider received an unsupported literal sentinel"};
        }
        const auto value = bg::ScalarValue<bg::i64>::from_literal(tagged->argument);
        return {SeriesLiteralUpdate::Kind::value, value.view().value()};
    }
    return literal.visit(
        [](const literal::ScalarLiteralItem &item) {
            return SeriesLiteralUpdate{SeriesLiteralUpdate::Kind::value, time_series_literal_number(item)};
        },
        [](std::span<const literal::ScalarLiteral>) -> SeriesLiteralUpdate {
            throw std::invalid_argument{"provider scalar time series literal must contain one item"};
        }).value();
}

class SeriesViewRuntimeType final : public bg::TimeSeriesRuntimeType {
  public:
    explicit SeriesViewRuntimeType(const bg::TimeSeriesType &type)
        : TimeSeriesRuntimeType{type, sizeof(SeriesViewStorage), alignof(SeriesViewStorage), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<SeriesViewStorage *>(destination));
    }

    void destroy_value(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<SeriesViewStorage *>(value));
    }
};

const bg::TimeSeriesRuntimeType &series_view_runtime_type(const bg::TimeSeriesType &type) {
    static auto types = std::map<const bg::TimeSeriesType *, std::unique_ptr<SeriesViewRuntimeType>>{};
    const auto [entry, inserted] = types.try_emplace(&type);
    if (inserted) {
        entry->second = std::make_unique<SeriesViewRuntimeType>(type);
    }
    return *entry->second;
}

class UnsupportedCopyRuntimeType final : public bg::TimeSeriesRuntimeType {
  public:
    explicit UnsupportedCopyRuntimeType(const bg::TimeSeriesType &type) : TimeSeriesRuntimeType{type} {}

    void construct_initial_value(void *) const noexcept override {}
    void destroy_value(void *, bg::allocator &) const noexcept override {}
};

} // namespace provider

namespace bg {

template <typename Item, auto... Properties> struct time_series_type_traits<provider::Series<Item, Properties...>> {
    using semantic_type = provider::SeriesType;

    static constexpr bool generic = scalar_declaration_traits<Item>::generic;
    static constexpr bool supported = scalar_declaration_traits<Item>::generic || scalar_type_mapped<Item>;

    template <typename, std::size_t, TimeSeriesTypeRole> static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &item = cpp_scalar_type<Item>();
        if constexpr (generic) {
            static const auto type = provider::SeriesType{item, guarantee};
            return type;
        } else if (!guarantee.is_resolved()) {
            static const auto type = provider::SeriesType{item, guarantee};
            return type;
        }
        return provider::series_type(item, *guarantee.value());
    }
};

template <typename Item> struct scalar_type_traits<provider::Number<Item>> {
    static const ScalarType &type() { return provider::number_type(cpp_scalar_type<Item>()); }
};

template <typename Item> struct scalar_declaration_traits<provider::Number<Item>> {
    static constexpr bool generic = scalar_declaration_traits<Item>::generic;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = scalar_declaration_traits<Item>::supports_implicit_argument;
    static constexpr bool supports_time_series = true;
};

template <typename Item> struct declaration_variable_traits<provider::Number<Item>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        ::bg::for_each_declaration_variable<Item>(std::forward<Visitor>(visitor));
    }
};

template <typename Item, auto... Properties> struct time_series_runtime_traits<provider::Series<Item, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return provider::series_runtime_type(type);
    }
};

template <typename Item, auto... SeriesProperties>
struct output_declaration_traits<Out<provider::Series<Item, SeriesProperties...>, provider::view>> {
    using time_series_type = provider::Series<Item, SeriesProperties...>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return provider::series_view_runtime_type(type);
    }
};

template <>
struct output_declaration_traits<Out<provider::Series<i64, TimeSeriesGuarantee::final>, provider::view>> {
    using time_series_type = provider::Series<i64, TimeSeriesGuarantee::final>;

    // This declaration's view API uses the same runtime representation as its default API.
    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return provider::series_runtime_type(type);
    }
};

template <>
struct output_declaration_traits<Out<provider::Series<string>, provider::view>> {
    using time_series_type = provider::Series<string>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &) {
        return provider::series_view_runtime_type(provider::Series<i64>::type());
    }
};

template <typename Item, auto... SeriesProperties>
struct declaration_variable_traits<provider::Series<Item, SeriesProperties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        ::bg::for_each_declaration_variable<Item>(std::forward<Visitor>(visitor));
        TimeSeriesDeclarationProperties<SeriesProperties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

template <typename Item, auto... SeriesProperties, auto... Properties>
class In<provider::Series<Item, SeriesProperties...>, Properties...>
    : public InputActivityControl<InputProperties<Properties...>::dynamic_activity> {
    using ParsedProperties = InputProperties<Properties...>;
    using ActivityControl = InputActivityControl<ParsedProperties::dynamic_activity>;

  public:
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : ActivityControl{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    i64 value() const noexcept { return *static_cast<const i64 *>(data_.data()); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_}.visit(std::forward<Functions>(functions)...);
    }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    TimeSeriesInputData data_;
};

template <typename Item, bool DirectStorage, auto... Properties> class SeriesOutput {
  public:
    using storage_type = i64;

    explicit SeriesOutput(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    SeriesOutput(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(DirectStorage && !scalar_declaration_traits<Item>::generic)
        : binding_{last_updated, &value, cpp_time_series_runtime_type<provider::Series<Item, Properties...>>(),
                   context} {}

    void set(i64 value) const {
        *static_cast<i64 *>(binding_->mutable_data()) = value;
        binding_->publish();
    }

    void invalidate() const {
        if constexpr (!DirectStorage) {
            if (provider::view_invalidation_observer != nullptr) {
                *provider::view_invalidation_observer = true;
            }
        }
        binding_->invalidate();
    }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }
    OutputState &output_state() const noexcept { return binding_.get(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto erased = Out<TS_T>{TimeSeriesOutputBinding{binding_.get()}};
        return erased.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = provider::Series<Item, Properties...>;

  private:
    TimeSeriesOutputBinding binding_;
};

template <typename Item, auto... Properties>
class Out<provider::Series<Item, Properties...>> : public SeriesOutput<Item, true, Properties...> {
    using Base = SeriesOutput<Item, true, Properties...>;

  public:
    using Base::Base;
};

template <typename Item, auto... Properties>
class Out<provider::Series<Item, Properties...>, provider::view>
    : public SeriesOutput<Item, false, Properties...> {
    using Base = SeriesOutput<Item, false, Properties...>;

  public:
    using Base::Base;
};

} // namespace bg

namespace provider {

template <typename Output> void apply_provider_literal(const literal::TimeSeriesLiteral &literal, bg::OutputState &state) {
    auto erased = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{state}};
    auto output = erased.as_output<Output>();
    const auto update = read_series_literal(literal);
    if (update.kind == SeriesLiteralUpdate::Kind::skip) {
        ++time_series_literal_skip_calls;
        return;
    }
    if (update.kind == SeriesLiteralUpdate::Kind::invalid) {
        ++time_series_literal_invalid_calls;
        output.invalidate();
        return;
    }

    output.set(update.value);
}

void apply_series_literal_semantic(const literal::TimeSeriesLiteral &literal, bg::OutputState &output) {
    ++time_series_literal_semantic_calls;
    apply_provider_literal<bg::Out<RuntimeOutputSeries>>(literal, output);
}

void apply_series_literal_runtime(const literal::TimeSeriesLiteral &literal, bg::OutputState &output) {
    ++time_series_literal_runtime_calls;
    apply_provider_literal<bg::Out<RuntimeOutputSeries, view>>(literal, output);
}

bool register_time_series_literal_handlers() {
    using Series = RuntimeOutputSeries;
    static const auto registered = [] {
        auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesLiteralOperations>::instance();
        return dispatch.add(Series::type(),
                            bg::TimeSeriesLiteralOperations{&apply_series_literal_semantic}) &&
               dispatch.add(series_view_runtime_type(Series::type()),
                            bg::TimeSeriesLiteralOperations{&apply_series_literal_runtime});
    }();
    return registered;
}

} // namespace provider

TEST_CASE("generic output copy uses the provider runtime for its output layout",
          "[feature:node-api][feature:test-graph][series:generic][declaration:runtime][transition:update]") {
    using Series = provider::Series<bg::i64>;
    provider::copied_series_count = 0;

    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                     bg::Out<Series, provider::view> output) { output.set(*input); });
    const auto copy = bg::node([](bg::In<bg::TS_T> input, bg::Out<bg::TS_T> output) { output.copy(input); });
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(copy(provide(input))); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{41, 82}).equals<bg::i64>({41, 82}));
    CHECK(provider::copied_series_count == 2);
}

TEST_CASE("a provider runtime without a copy implementation rejects copying",
          "[feature:node-api][series:value][declaration:runtime][transition:update]") {
    using Series = provider::Series<bg::i64>;
    const auto &input_runtime = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    const auto runtime = provider::UnsupportedCopyRuntimeType{Series::type()};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(bg::MIN_DT + std::chrono::microseconds{1});
    auto input_value = bg::i64{41};
    const auto input = bg::TimeSeriesInputData{input_runtime, input_state, &input_value};
    auto output = bg::OutputState{runtime};

    CHECK_THROWS_AS(runtime.copy(input, output), std::logic_error);
}

struct RuntimeProviderViewDefinition {
    using output_time_series_type = provider::Series<bg::i64>;
    using output_declaration_type = bg::Out<output_time_series_type, provider::view>;

    RuntimeProviderViewDefinition()
        : signature_{bg::util::symbol{"runtime_provider_view"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}},
                     output_time_series_type::type()} {}

    const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto input = invocation.input<bg::In<bg::TS<bg::i64>>>(0);
        const auto output = invocation.output_as<output_declaration_type>();
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    }

    bg::Signature signature_;
};

namespace provider {

template <typename Series>
void replay_tick_view(bg::util::buffer_reader &reader, const bg::Out<Series, view> &output) {
    ++replayed_tick_count;
    output.set(reader.read_raw<bg::i64>());
}

template <typename Series>
void replay_tick_default(bg::util::buffer_reader &reader, const bg::Out<Series> &output) {
    ++replayed_tick_count;
    output.set(reader.read_raw<bg::i64>());
}

template <typename Series>
void replay_tick_view_override(bg::util::buffer_reader &reader, const bg::Out<Series, view> &output) {
    output.set(reader.read_raw<bg::i64>() + 100);
}

void discard_tick(bg::util::buffer_reader &reader) {
    (void)reader.read_raw<bg::i64>();
    ++discarded_tick_count;
}

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

struct alignas(std::max_align_t) owned_provider_payload {
    bg::ScalarValue<> value;
};

void write_owned_payload_value(bg::ScalarBufferWriteContext &writer, bg::allocator &memory, std::string value) {
    auto count = std::uint32_t{1};
    writer.write_bytes(std::as_bytes(std::span{&count, std::size_t{1}}));
    const auto marker = bg::i64{17};
    writer.write_bytes(std::as_bytes(std::span{&marker, std::size_t{1}}));
    const auto stored = bg::ScalarValue<bg::string>{std::move(value), memory};
    auto payload = bg::ScalarValue<>{stored, memory};
    writer.write(std::move(payload));
}

template <typename Series>
void write_owned_payload(const bg::In<Series> &, bg::ScalarBufferWriteContext &writer, bg::allocator &memory) {
    write_owned_payload_value(writer, memory, "provider-owned retained payload that needs allocated storage");
}

template <typename Series>
void write_two_owned_payloads_then_fail(const bg::In<Series> &, bg::ScalarBufferWriteContext &writer,
                                        bg::allocator &memory) {
    auto count = std::uint32_t{2};
    writer.write_bytes(std::as_bytes(std::span{&count, std::size_t{1}}));
    const auto marker = bg::i64{29};
    writer.write_bytes(std::as_bytes(std::span{&marker, std::size_t{1}}));
    const auto first_value = bg::ScalarValue<bg::string>{
        std::string{"first provider-owned payload with allocated storage"}, memory};
    const auto second_value = bg::ScalarValue<bg::string>{
        std::string{"second provider-owned payload with allocated storage"}, memory};
    auto first = bg::ScalarValue<>{first_value, memory};
    auto second = bg::ScalarValue<>{second_value, memory};
    writer.write(std::move(first));
    writer.write(std::move(second));
    throw std::runtime_error{"provider write failed after writing two payloads"};
}

template <typename Series>
void replay_owned_payload_record(bg::util::buffer_reader &reader, const bg::Out<Series> &output) {
    const auto count = reader.read_raw<std::uint32_t>();
    output.set(reader.read_raw<bg::i64>());
    for (auto index = std::uint32_t{}; index < count; ++index) {
        (void)reader.peek<bg::ScalarValue<>>();
        reader.consume<bg::ScalarValue<>>();
    }
}

template <typename Series>
void replay_owned_payload_then_fail(bg::util::buffer_reader &reader, const bg::Out<Series> &) {
    (void)reader.read_raw<std::uint32_t>();
    (void)reader.read_raw<bg::i64>();
    (void)reader.peek<bg::ScalarValue<>>();
    reader.consume<bg::ScalarValue<>>();
    throw std::runtime_error{"provider replay failed after reading its payload"};
}

void discard_owned_payload(bg::util::buffer_reader &reader) {
    (void)reader.read_raw<std::uint32_t>();
    (void)reader.read_raw<bg::i64>();
    reader.skip<bg::ScalarValue<>>();
}

void discard_owned_payload_record(bg::util::buffer_reader &reader) {
    const auto count = reader.read_raw<std::uint32_t>();
    (void)reader.read_raw<bg::i64>();
    for (auto index = std::uint32_t{}; index < count; ++index) {
        reader.skip<bg::ScalarValue<>>();
    }
}

template <typename Series> void write_second_payload_then_fail(const bg::In<Series> &input,
                                                                 bg::ScalarBufferWriteContext &writer,
                                                                 bg::allocator &memory) {
    if (++payload_write_count == 2) {
        write_two_owned_payloads_then_fail(input, writer, memory);
    }
    write_owned_payload(input, writer, memory);
}

template <typename Series> void write_tsd_payload_then_fail(const bg::In<Series> &,
                                                             bg::ScalarBufferWriteContext &writer,
                                                             bg::allocator &memory) {
    if (++failed_tsd_write_count == 2) {
        throw std::runtime_error{"provider write failed in a later TSD entry"};
    }
    write_owned_payload_value(writer, memory, "owned payload in the first completed TSD entry");
}

template <typename Series> void fail_before_payload(const bg::In<Series> &, bg::ScalarBufferWriteContext &,
                                                     bg::allocator &) {
    throw std::runtime_error{"provider write failed before producing a value"};
}

void write_provider_scalar(bg::Scalar<test_scalar_provider::value> value,
                           bg::ScalarBufferWriteContext &writer, bg::allocator &memory) {
    const auto number = value.value().get();
    writer.write_bytes(std::as_bytes(std::span{&number, std::size_t{1}}));
    if (number == 79) {
        const auto payload_storage = test_scalar_provider::storage{number};
        auto payload = bg::ScalarValue<>{bg::Scalar<test_scalar_provider::value>::from(
                                             payload_storage),
                                         memory};
        writer.write(owned_provider_payload{std::move(payload)});
    }
    if (number == 73 || number == 91) {
        const auto text = number == 73 ? "provider payload with value-dependent size"
                                       : "provider payload emitted before a deliberate write failure";
        const auto stored = bg::ScalarValue<bg::string>{text, memory};
        auto payload = bg::ScalarValue<>{stored, memory};
        writer.write(owned_provider_payload{std::move(payload)});
    }
    const auto write = ++scalar_tick_state.writes;
    if (number == 91) {
        throw std::runtime_error{"provider scalar write failed after an owned payload"};
    }
    if (write == scalar_tick_state.fail_on_write) {
        const auto partial = 29;
        writer.write_bytes(std::as_bytes(std::span{&partial, std::size_t{1}}));
        throw std::runtime_error{"scalar handler write failed"};
    }
}

void project_provider_scalar(bg::Scalar<test_scalar_provider::value> value,
                             bg::ScalarBufferProjectionContext &projection) {
    projection.write_bytes(sizeof(int));
    const auto number = value.value().get();
    if (number == 73 || number == 79 || number == 91) {
        projection.emplace<owned_provider_payload>();
    }
}

void project_provider_scalar_semantic(bg::Scalar<test_scalar_provider::value> value,
                                      bg::ScalarBufferProjectionContext &projection) {
    (void)value;
    projection.write_bytes(sizeof(int) + 32);
}

void read_provider_scalar(bg::util::buffer_reader &reader, auto consume) {
    ++scalar_tick_state.reads;
    const auto number = reader.read_raw<int>();
    if (number == 79) {
        const auto &payload = reader.peek<owned_provider_payload>();
        const auto value = bg::scalar_cast<test_scalar_provider::value>(payload.value.view());
        reader.consume<owned_provider_payload>();
        if (throw_before_scalar_read_callback) {
            throw std::runtime_error{"provider scalar read failed before callback"};
        }
        consume(value);
        if (throw_after_scalar_read_callback) {
            throw std::runtime_error{"provider scalar read failed after callback"};
        }
        return;
    }
    if (number == 73 || number == 91) {
        reader.consume<owned_provider_payload>();
    }
    const auto storage = test_scalar_provider::storage{number};
    consume(bg::Scalar<test_scalar_provider::value>::from(storage));
}

void discard_provider_scalar(bg::util::buffer_reader &reader) {
    const auto number = reader.read_raw<int>();
    if (number == 73 || number == 79 || number == 91) {
        reader.skip<owned_provider_payload>();
    }
    ++scalar_tick_state.discards;
}

void discard_owned_payload_and_count(bg::util::buffer_reader &reader) {
    discard_owned_payload_record(reader);
    ++discarded_tick_count;
}

template <typename Series>
void fail_incompatible_replay(bg::util::buffer_reader &, const bg::Out<Series> &) {
    incompatible_replay_called = true;
}

bool register_provider_scalar_buffer_handlers() {
    static const auto registered = [] {
        auto semantic = bg::ScalarBufferOperations::for_read<test_scalar_provider::value>(
            [](auto &reader, auto consume) { read_provider_scalar(reader, consume); });
        semantic.set_discard(discard_provider_scalar);
        semantic.set_project<test_scalar_provider::value>(project_provider_scalar_semantic);
        auto runtime = bg::ScalarBufferOperations{};
        runtime.set_write<test_scalar_provider::value>(write_provider_scalar);
        runtime.set_project<test_scalar_provider::value>(project_provider_scalar);
        auto unprojected_runtime = bg::ScalarBufferOperations{};
        unprojected_runtime.set_write<test_scalar_provider::value>(write_provider_scalar);
        auto &dispatch = bg::ScalarDispatch<bg::ScalarBufferOperations>::instance();
        return dispatch.add(bg::cpp_scalar_type<test_scalar_provider::value>(), std::move(semantic)) &&
               dispatch.add(bg::cpp_scalar_runtime_type<test_scalar_provider::value>(), std::move(runtime)) &&
               dispatch.add(test_scalar_provider::canonicalizing_runtime_type(), std::move(unprojected_runtime));
    }();
    return registered;
}

void register_default_tick_handlers() {
    static const auto registered = [] {
        using Series = provider::Series<bg::i64>;
        auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
        auto semantic = bg::TimeSeriesTickOperations::for_type<Series>(
            write_tick<bg::In<Series>>, replay_tick_default<Series>, discard_tick);
        auto runtime = bg::TimeSeriesTickOperations::for_write<Series>(write_tick<bg::In<Series>>);
        runtime.set_replay<bg::Out<Series, provider::view>>(replay_tick_view<Series>);
        return dispatch.add(Series::type(), std::move(semantic)) &&
               dispatch.add(provider::series_view_runtime_type(Series::type()), std::move(runtime));
    }();
    if (!registered) {
        throw std::logic_error{"provider tick handlers could not be registered"};
    }
}

struct TickDelayStorage {
    explicit TickDelayStorage(bg::allocator memory)
        : buffer{bg::util::buffer::chain(256, memory)}, reader{buffer, Series<bg::i64>::type()},
          writer{buffer, memory} {
        register_default_tick_handlers();
    }

    bg::util::buffer buffer;
    bg::TimeSeriesTickReader reader;
    bg::TimeSeriesTickWriter writer;
};

struct OneTickDelay {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<Series<bg::i64>, bg::optional> input;
    bg::Out<Series<bg::i64>, provider::view> output;
    bg::State<TickDelayStorage *> storage;

    void start(bg::EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        *storage = memory.new_object<TickDelayStorage>(memory);
    }

    void evaluate(bg::EvaluationContext *context) const {
        if ((*storage)->reader.has_next()) {
            (*storage)->reader.replay(bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output.output_state()}});
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

struct Aggregate {
    bg::In<Series<bg::i64>> input;
    bg::Out<Series<bg::i64>> output;

    void evaluate() const { output.set(input.value()); }
};

struct ViewAggregate {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<Series<bg::i64>, provider::view> output;

    void evaluate() const { output.set(*input); }
};

using ContextAction = std::function<void(bg::EvaluationContext &)>;

struct ContextActionNode {
    bg::Scalar<bg::Opaque<ContextAction *>> action;
    bg::In<bg::TS<bg::i64>> trigger;
    bg::Out<bg::TS<bg::i64>> result;

    void evaluate(bg::EvaluationContext *context) const {
        (**action)(*context);
        result.set(*trigger);
    }
};

} // namespace provider

TEST_CASE("a provider declaration exposes public semantic and runtime customizations",
          "[type][time_series][feature:type-system][declaration:runtime]") {
    static_assert(!bg::details::direct_time_series_output<bg::Out<provider::Series<bg::i64>, provider::view>>);
    using Series = provider::Series<bg::i64>;
    const auto &semantic_type = Series::type();
    const auto &runtime_type = bg::cpp_time_series_runtime_type<Series>();

    CHECK(bg::TypeRegistry::instance().contains(provider::number_type(bg::int_type())));
    CHECK(bg::TypeRegistry::instance().contains(semantic_type));
    CHECK(&runtime_type.type() == &semantic_type);
    CHECK(&bg::resolve_time_series_runtime_type(semantic_type) == &runtime_type);
    CHECK_NOTHROW(bg::register_time_series_runtime_type(runtime_type));

    const auto &canonical_value = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    const auto &unrelated_value = bg::cpp_time_series_runtime_type<bg::TS<bg::string>>();
    const auto unrelated_runtime_types = std::array<const bg::TimeSeriesRuntimeType *, 1>{&unrelated_value};
    CHECK(&bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type(), unrelated_runtime_types) ==
          &canonical_value);

    const auto supplied = provider::SeriesRuntimeType{semantic_type};
    const auto supplied_runtime_types = std::array<const bg::TimeSeriesRuntimeType *, 1>{&supplied};
    CHECK(&bg::resolve_time_series_runtime_type(semantic_type, supplied_runtime_types) == &supplied);
    CHECK_THROWS_AS(bg::register_time_series_runtime_type(supplied), std::invalid_argument);

    const auto unresolved = provider::SeriesRuntimeType{provider::Series<bg::SCALAR>::type()};
    CHECK_THROWS_AS(bg::register_time_series_runtime_type(unresolved), std::invalid_argument);

    const auto wrong_value_family = provider::SeriesRuntimeType{bg::TS<bg::i64>::type()};
    CHECK_THROWS_AS(bg::register_time_series_runtime_type(wrong_value_family), std::invalid_argument);

    const auto &wrong_value_component =
        bg::time_series_value_runtime_type(bg::TS<bg::string>::type(), bg::cpp_scalar_runtime_type<bg::i64>());
    CHECK_THROWS_AS(bg::register_time_series_runtime_type(wrong_value_component), std::invalid_argument);

    const auto &wrong_set_component =
        bg::details::time_series_set_runtime_type(bg::TSS<bg::string>::type(), bg::cpp_scalar_runtime_type<bg::i64>());
    CHECK_THROWS_AS(bg::register_time_series_runtime_type(wrong_set_component), std::invalid_argument);

    const auto &wrong_dictionary_component = bg::details::time_series_dictionary_runtime_type(
        bg::TSD<bg::i64, bg::TS<bg::string>>::type(), bg::cpp_scalar_runtime_type<bg::string>(),
        bg::cpp_time_series_runtime_type<bg::TS<bg::string>>());
    CHECK_THROWS_AS(bg::register_time_series_runtime_type(wrong_dictionary_component), std::invalid_argument);
}

TEST_CASE("scalar literal construction dispatches by runtime then semantic type",
          "[feature:scalar-api][scalar:opaque][declaration:runtime]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_scalar_literal_handlers());

    const auto source = bg::i64{7};
    const auto literal_fields = std::array{literal::ScalarLiteral(source)};
    const auto literal = literal::ScalarLiteral{std::span{literal_fields}};
    provider::scalar_literal_semantic_calls = 0;
    provider::scalar_literal_runtime_calls = 0;

    const auto semantic = bg::ScalarValue<>::from_literal(test_scalar_provider::preserving_runtime_type(), literal);
    CHECK(bg::scalar_cast<Value>(semantic.view()).value().get() == 17);
    CHECK(provider::scalar_literal_semantic_calls == 1);
    CHECK(provider::scalar_literal_runtime_calls == 0);

    const auto exact = bg::ScalarValue<>::from_literal(test_scalar_provider::canonicalizing_runtime_type(), literal);
    CHECK(bg::scalar_cast<Value>(exact.view()).value().get() == 107);
    CHECK(provider::scalar_literal_semantic_calls == 1);
    CHECK(provider::scalar_literal_runtime_calls == 1);

    const auto typed = bg::ScalarValue<Value>::from_literal(literal);
    CHECK(typed.view().value().get() == 17);
    CHECK(provider::scalar_literal_semantic_calls == 2);
}

TEST_CASE("scalar literal extension handlers construct nested collection items",
          "[feature:scalar-api][scalar:list][declaration:runtime]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_scalar_literal_handlers());

    const auto values = bg::ScalarValue<bg::List<Value>>::from_literal({{bg::i64{3}}, {bg::i64{9}}});
    const auto list = values.view();

    REQUIRE(list.size() == 2);
    CHECK(list[0].value().get() == 13);
    CHECK(list[1].value().get() == 19);
}

TEST_CASE("extension sentinels reach scalar literal providers through time-series and nested payloads",
          "[feature:scalar-api][feature:time-series-storage][series:value][scalar:opaque]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_scalar_literal_handlers());

    const auto instruction = literal::ScalarLiteral(*test_shared_literal_sentinel::extension_call());
    const auto extension_fields = std::array{instruction};
    const auto extension_literal = literal::ScalarLiteral{std::span{extension_fields}};
    provider::expected_scalar_literal = &extension_literal;
    provider::scalar_literal_address_matches = false;
    const auto scalar = bg::ScalarValue<Value>::from_literal(extension_literal);
    CHECK(scalar.view().value().get() == 81);
    CHECK(provider::scalar_literal_address_matches);

    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    auto last_updated = bg::TimeSeriesState{};
    auto output_storage = test_scalar_provider::storage{};
    auto output_state = bg::OutputState{runtime_type};
    output_state.bind(last_updated, &output_storage, bg::MIN_DT + std::chrono::microseconds{1});
    auto output = bg::Out<bg::TS<Value>>{bg::TimeSeriesOutputBinding{output_state}};
    provider::expected_scalar_literal = &extension_literal;
    provider::scalar_literal_address_matches = false;
    auto memory = bg::allocator{};
    auto *allocation_owner = bg::details::BundleAllocationOwner::create(memory);
    auto release_allocation_owner = bg::util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
    const auto allocation_context = bg::details::ScalarAllocationContext{memory, *allocation_owner, &output_storage, 0};
    output.apply(extension_literal);
    CHECK(output_storage.number == 81);
    CHECK(provider::scalar_literal_address_matches);

    const auto nested_instruction = literal::ScalarLiteral(*test_shared_literal_sentinel::extension_call());
    const auto nested_payload_fields = std::array{nested_instruction};
    const auto nested_payload = literal::ScalarLiteral{std::span{nested_payload_fields}};
    const auto nested_fields = std::array{nested_payload};
    const auto nested_literal = literal::ScalarLiteral{std::span{nested_fields}};
    provider::expected_scalar_literal = &nested_fields.front();
    provider::scalar_literal_address_matches = false;
    const auto nested = bg::ScalarValue<bg::List<Value>>::from_literal(nested_literal);
    REQUIRE(nested.view().size() == 1);
    CHECK(nested.view()[0].value().get() == 81);
    CHECK(provider::scalar_literal_address_matches);
}

TEST_CASE("extension time-series handlers consume shared sentinel instructions",
          "[feature:time-series-view][feature:test-graph][series:value]") {
    using Series = provider::RuntimeOutputSeries;
    REQUIRE(provider::register_time_series_literal_handlers());

    const auto &runtime_type = provider::series_view_runtime_type(Series::type());
    auto last_updated = bg::TimeSeriesState{};
    auto value = provider::SeriesViewStorage{};
    auto state = bg::OutputState{runtime_type};
    state.bind(last_updated, &value, bg::MIN_DT + std::chrono::microseconds{1});
    auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{state}};

    provider::time_series_literal_runtime_calls = 0;
    output.apply(literal::TimeSeriesLiteral(*test_shared_literal_sentinel::extension_call()));

    CHECK(value.value == 71);
    CHECK(last_updated.valid());
    CHECK(provider::time_series_literal_runtime_calls == 1);
}

TEST_CASE("scalar literal construction rejects unsupported destinations and releases failed allocations",
          "[feature:scalar-api][scalar:opaque][declaration:runtime]") {
    REQUIRE(provider::register_scalar_literal_handlers());
    const auto source = bg::i64{7};
    const auto fields = std::array{literal::ScalarLiteral(source)};
    const auto literal = literal::ScalarLiteral{std::span{fields}};

    CHECK_THROWS_AS((bg::ScalarValue<>::from_literal(bg::cpp_scalar_runtime_type<bg::i64>(), literal)),
                    std::invalid_argument);

    auto resource = counting_memory_resource{};
    auto memory = bg::allocator{resource};
    CHECK_THROWS_AS((bg::ScalarValue<>::from_literal(test_scalar_provider::preserving_copy_only_runtime_type(),
                                                      literal, memory)),
                    std::runtime_error);
    CHECK(resource.allocations() == 0);
}

TEST_CASE("erased time-series literal updates use runtime handlers and publish through collections",
          "[feature:time-series-view][feature:test-graph][series:fixed-tsl][declaration:runtime]") {
    using Series = provider::RuntimeOutputSeries;
    REQUIRE(provider::register_time_series_literal_handlers());

    const auto &runtime_type = provider::series_view_runtime_type(Series::type());
    auto last_updated = bg::TimeSeriesState{};
    auto value = provider::SeriesViewStorage{};
    auto state = bg::OutputState{runtime_type};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    state.bind(last_updated, &value, time);
    auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{state}};

    provider::time_series_literal_runtime_calls = 0;
    provider::time_series_literal_semantic_calls = 0;
    output.apply(literal::TimeSeriesLiteral(bg::i64{12}));

    CHECK(value.value == 12);
    CHECK(last_updated.valid());
    CHECK(last_updated.time() == time);
    CHECK(provider::time_series_literal_runtime_calls == 1);
    CHECK(provider::time_series_literal_semantic_calls == 0);

    const auto make_list = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                       bg::Out<bg::TSL<Series, 1>> output) {
        output.apply({{bg::i64{0}, input.value()}});
    });
    const auto consume = bg::node([](bg::In<bg::TSL<Series, 1>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input[0].value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(make_list(input)); };

    provider::time_series_literal_semantic_calls = 0;
    const auto result = bg::testing::test_graph(graph, std::array<bg::i64, 2>{4, 8});
    CHECK(result.equals<bg::i64>({4, 8}));
    CHECK(provider::time_series_literal_semantic_calls == 2);
}

TEST_CASE("time-series literal providers receive markers and missing handlers fail",
          "[feature:time-series-view][series:value][declaration:runtime]") {
    using Series = provider::RuntimeOutputSeries;
    REQUIRE(provider::register_time_series_literal_handlers());

    auto last_updated = bg::TimeSeriesState{};
    auto value = bg::i64{};
    auto state = bg::OutputState{provider::series_runtime_type(Series::type())};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    state.bind(last_updated, &value, time);
    const auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{state}};

    provider::time_series_literal_skip_calls = 0;
    provider::time_series_literal_invalid_calls = 0;
    output.apply(literal::TimeSeriesLiteral(literal::SKIP));
    CHECK(provider::time_series_literal_skip_calls == 1);
    CHECK_FALSE(last_updated.valid());

    output.apply(literal::TimeSeriesLiteral(bg::i64{12}));
    CHECK(last_updated.valid());
    CHECK(value == 12);
    output.apply(literal::TimeSeriesLiteral(literal::INVALID));
    CHECK(provider::time_series_literal_invalid_calls == 1);
    CHECK_FALSE(last_updated.valid());

    using UnsupportedSeries = provider::Series<provider::Number<double>>;
    auto unsupported_value = bg::i64{};
    auto unsupported_state = bg::OutputState{provider::series_runtime_type(UnsupportedSeries::type())};
    unsupported_state.bind(last_updated, &unsupported_value, time);
    const auto unsupported = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{unsupported_state}};
    CHECK_THROWS_AS(unsupported.apply(literal::TimeSeriesLiteral(bg::i64{1})), std::invalid_argument);
}

TEST_CASE("an extension time series limits its leaf guarantee through its factory",
          "[type][time_series][feature:type-system][declaration:runtime]"
          "[guarantee:final][guarantee:const]") {
    const auto &source = provider::series_type(bg::int64_type(), bg::TimeSeriesGuarantee::const_);

    CHECK(&source.with_guarantee_limit(bg::TimeSeriesGuarantee::final) ==
          &provider::series_type(bg::int64_type(), bg::TimeSeriesGuarantee::final));
    CHECK(&source.with_guarantee_limit(bg::TimeSeriesGuarantee::const_) == &source);
}

TEST_CASE("provider inputs and configured outputs bind through public owners",
          "[value][time_series][feature:time-series-view]") {
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_value = bg::i64{17};
    const auto input_state = bg::TimeSeriesState{time};
    const auto input_data =
        bg::TimeSeriesInputData{bg::cpp_time_series_runtime_type<Series>(), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    CHECK(input.valid());
    CHECK(input.modified());
    CHECK(input.value() == 17);

    auto output_time = bg::TimeSeriesState{};
    auto output_value = bg::i64{};
    auto state = bg::OutputState{bg::cpp_time_series_runtime_type<Series>()};
    state.bind(output_time, &output_value, time);
    auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{state}};
    output.set(23);

    CHECK(output_value == 23);
    CHECK(output_time.time() == time);

    const auto erased_input = bg::In<bg::TS_T>{input_data};
    CHECK_THROWS_AS(erased_input.as<bg::TS<bg::string>>(), std::bad_cast);
    auto input_visited = false;
    erased_input.visit(
        [&](const bg::In<bg::TS_T> *value) {
            input_visited = &value->runtime_type() == &bg::cpp_time_series_runtime_type<Series>();
        },
        ddv::noop);
    const auto input_result = erased_input.visit(
        [](const bg::In<bg::TS_T> *value) { return value->valid(); }, ddv::noop);

    auto erased_output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{state}};
    auto output_visited = false;
    erased_output.visit(
        [&](const bg::Out<bg::TS_T> *value) {
            output_visited = &value->runtime_type() == &bg::cpp_time_series_runtime_type<Series>();
        },
        ddv::noop);
    const auto output_result = erased_output.visit(
        [](const bg::Out<bg::TS_T> *value) { return value->valid(); }, ddv::noop);

    CHECK(input_visited);
    CHECK(output_visited);
    REQUIRE(input_result);
    REQUIRE(output_result);
    CHECK(*input_result);
    CHECK(*output_result);
}

TEST_CASE("tick write dispatch reaches provider-defined time series",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    provider::register_default_tick_handlers();
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input_data = bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<Series>(), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    const auto buffer = bg::util::buffer::chain(256);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    provider::written_tick_count = 0;

    CHECK(writer.write(input, time));
    CHECK(provider::written_tick_count == 1);
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};
    const auto output_time = time + std::chrono::microseconds{1};
    auto output_last_updated = bg::TimeSeriesState{};
    auto output_value = provider::SeriesViewStorage{};
    auto output_state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    output_state.bind(output_last_updated, &output_value, output_time);
    auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};

    CHECK(reader.replay(output));
    CHECK(output_last_updated.valid());
    CHECK(output_last_updated.time() == output_time);
    CHECK(output_value.value == 17);
}

TEST_CASE("a provider i64 tick uses a nine-byte record",
          "[feature:time-series-storage][series:generic][declaration:concrete][scalar:i64]") {
    using Series = provider::Series<bg::i64>;
    provider::register_default_tick_handlers();
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};

    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};

    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(input_value));
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};
    CHECK(reader.discard());
}

TEST_CASE("provider scalar buffer actions can use different exact type registrations",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    const auto &dispatch = bg::ScalarDispatch<bg::ScalarBufferOperations>::instance();
    const auto *semantic = dispatch.find_exact(bg::cpp_scalar_type<Value>());
    const auto *runtime = dispatch.find_exact(bg::cpp_scalar_runtime_type<Value>());
    REQUIRE(semantic != nullptr);
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(semantic->has_write());
    CHECK(semantic->has_read());
    CHECK(semantic->has_discard());
    CHECK(runtime->has_write());
    CHECK_FALSE(runtime->has_read());
    CHECK_FALSE(runtime->has_discard());

    provider::scalar_tick_state = {};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto input_state = bg::TimeSeriesState{time};
    const auto input_storage = test_scalar_provider::storage{17};
    const auto input = bg::In<bg::TS<Value>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<Value>>(), input_state, &input_storage, true}};
    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(int));

    auto output_time = bg::TimeSeriesState{};
    auto output_storage = test_scalar_provider::storage{};
    auto output_state = bg::OutputState{bg::cpp_time_series_runtime_type<bg::TS<Value>>()};
    output_state.bind(output_time, &output_storage, time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS<Value>>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<Value>::type()};

    CHECK(reader.replay(output));
    CHECK(output_storage.number == 17);
    CHECK(provider::scalar_tick_state.discards == 1);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("queue collection inspection stops at its first conflict",
          "[feature:time-series-storage][series:tsb][queue][transition:update]") {
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {};

    using Series = provider::QueueConflictSeries;
    const auto previous_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = previous_time + std::chrono::microseconds{1};
    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto allocator = bg::allocator{memory_resource};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<Series>();

    auto *input_storage = allocator.allocate_bytes(runtime_type.value_size(), runtime_type.value_alignment());
    runtime_type.construct_runtime_value(input_storage, allocator);
    auto destroy_input = bg::util::scope_exit{[&]() noexcept {
        runtime_type.destroy_value(input_storage, allocator);
        allocator.deallocate_bytes(input_storage, runtime_type.value_size(), runtime_type.value_alignment());
    }};
    auto input_time = bg::TimeSeriesState{evaluation_time};
    auto first_input = runtime_type.item(input_storage, 0);
    first_input.last_updated.publish(evaluation_time);
    *static_cast<bg::i64 *>(first_input.value) = 42;
    auto later_input = runtime_type.item(input_storage, 1);
    later_input.last_updated.publish(evaluation_time);
    static_cast<test_scalar_provider::storage *>(later_input.value)->number = 17;
    REQUIRE(runtime_type.mark_item_modified(input_storage, 0, evaluation_time, input_time, allocator));
    REQUIRE(runtime_type.mark_item_modified(input_storage, 1, evaluation_time, input_time, allocator));

    const auto input = bg::In<Series>{bg::TimeSeriesInputData{runtime_type, input_time, input_storage, true}};
    const auto buffer = bg::util::buffer::contiguous(128);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    REQUIRE(writer.write(input, evaluation_time));

    auto *output_storage = allocator.allocate_bytes(runtime_type.value_size(), runtime_type.value_alignment());
    runtime_type.construct_runtime_value(output_storage, allocator);
    auto destroy_output = bg::util::scope_exit{[&]() noexcept {
        runtime_type.destroy_value(output_storage, allocator);
        allocator.deallocate_bytes(output_storage, runtime_type.value_size(), runtime_type.value_alignment());
    }};
    auto output_time = bg::TimeSeriesState{evaluation_time};
    auto first_output = runtime_type.item(output_storage, 0);
    first_output.last_updated.publish(evaluation_time);
    auto later_output = runtime_type.item(output_storage, 1);
    later_output.last_updated.publish(previous_time);

    auto output = bg::OutputState{runtime_type};
    output.bind(output_time, output_storage, evaluation_time);
    auto reader = bg::util::buffer_reader{buffer};
    const auto pending_size = reader.readable().size();
    const auto reads_before = provider::scalar_tick_state.reads;

    CHECK_FALSE(bg::details::QueueTickApply{reader, bg::QueueMode::standard}.apply(output));
    CHECK(reader.readable().size() == pending_size);
    CHECK(provider::scalar_tick_state.reads == reads_before);
}

TEST_CASE("queue message projection selects the runtime provider projection and follows its value",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {};

    const auto align_up = [](std::size_t position, std::size_t alignment) {
        return (position + alignment - 1) / alignment * alignment;
    };
    using Header = bg::details::QueueMessageHeader;
    const auto simple_size = align_up(sizeof(Header) + 1 + sizeof(int), alignof(Header));
    const auto owned_payload_end =
        align_up(sizeof(Header) + 1 + sizeof(int), alignof(provider::owned_provider_payload)) +
        sizeof(provider::owned_provider_payload);
    const auto owned_payload_size = align_up(owned_payload_end, alignof(Header));

    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto buffer = bg::util::buffer::concurrent_chain(1, 1);
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    auto writer = bg::details::QueueMessageWriter{buffer, 17, runtime_type, memory};

    REQUIRE(writer.write(literal::TimeSeriesLiteral(Value{17})));
    REQUIRE(writer.write(literal::TimeSeriesLiteral(Value{73})));

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    const auto replay = [&](int expected_number, std::size_t expected_size, std::size_t payload_size) {
        const auto frame = reader.read<Header>();
        CHECK(frame.target == 17);
        CHECK(frame.size == expected_size);
        const auto payload = reader.current_buffer().chunk(0, payload_size);
        auto output_time = bg::TimeSeriesState{};
        auto output_storage = test_scalar_provider::storage{};
        auto output_state = bg::OutputState{runtime_type};
        const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
        output_state.bind(output_time, &output_storage, evaluation_time);
        const auto output = bg::Out<bg::TS<Value>>{bg::TimeSeriesOutputBinding{output_state}};
        auto tick_reader = bg::TimeSeriesTickReader{payload, bg::TS<Value>::type()};
        REQUIRE(tick_reader.replay(output));
        CHECK(output_storage.number == expected_number);
        CHECK_FALSE(tick_reader.has_next());
        reader.consume(frame.size - sizeof(Header));
    };

    replay(17, simple_size, 1 + sizeof(int));
    replay(73, owned_payload_size, owned_payload_end - sizeof(Header));
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
    CHECK(reader.readable().empty());
}

TEST_CASE("consuming queue replay keeps a custom scalar owner alive through its read callback",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete][queue]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {};

    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
    auto writer = bg::details::QueueMessageWriter{buffer, 29, runtime_type, memory};
    REQUIRE(writer.write(literal::TimeSeriesLiteral(Value{79})));

    auto message_reader = bg::util::concurrent_buffer_reader{buffer};
    const auto frame = message_reader.read<bg::details::QueueMessageHeader>();
    constexpr auto value_offset = (1 + alignof(provider::owned_provider_payload) - 1) /
                                  alignof(provider::owned_provider_payload) *
                                  alignof(provider::owned_provider_payload);
    constexpr auto payload_size = value_offset + sizeof(provider::owned_provider_payload);
    auto tick_reader = bg::util::buffer_reader{
        message_reader.current_buffer().chunk(0, payload_size)};

    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto output_time = bg::TimeSeriesState{};
    auto output_storage = test_scalar_provider::storage{};
    auto output_state = bg::OutputState{runtime_type};
    output_state.bind(output_time, &output_storage, evaluation_time);

    REQUIRE(bg::details::QueueTickApply{tick_reader, bg::QueueMode::elide}.apply(output_state));
    CHECK(output_storage.number == 79);
    CHECK(tick_reader.readable().empty());
    CHECK(provider::scalar_tick_state.reads == 1);
    CHECK(provider::scalar_tick_state.discards == 1);
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
    CHECK(frame.target == 29);
}

TEST_CASE("consuming queue replay discards custom scalar payloads when the read hook throws around its callback",
          "[feature:time-series-storage][series:value][scalar:generic][queue]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());

    const auto replay_with_failure = [](bool throw_before_callback) {
        provider::scalar_tick_state = {};
        provider::throw_before_scalar_read_callback = throw_before_callback;
        provider::throw_after_scalar_read_callback = !throw_before_callback;
        auto reset_read_failure_flags = bg::util::scope_exit{[]() noexcept {
            provider::throw_before_scalar_read_callback = false;
            provider::throw_after_scalar_read_callback = false;
        }};

        auto resource = provider::allocation_counter{};
        auto memory = bg::allocator{resource};
        const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
        const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
        auto writer = bg::details::QueueMessageWriter{buffer, 29, runtime_type, memory};
        REQUIRE(writer.write(literal::TimeSeriesLiteral(Value{79})));

        auto message_reader = bg::util::concurrent_buffer_reader{buffer};
        const auto frame = message_reader.read<bg::details::QueueMessageHeader>();
        constexpr auto value_offset = (1 + alignof(provider::owned_provider_payload) - 1) /
                                      alignof(provider::owned_provider_payload) *
                                      alignof(provider::owned_provider_payload);
        constexpr auto payload_size = value_offset + sizeof(provider::owned_provider_payload);
        auto tick_reader = bg::util::buffer_reader{
            message_reader.current_buffer().chunk(0, payload_size)};

        auto output_time = bg::TimeSeriesState{};
        auto output_storage = test_scalar_provider::storage{};
        auto output_state = bg::OutputState{runtime_type};
        const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
        output_state.bind(output_time, &output_storage, evaluation_time);

        auto failure = std::string{};
        try {
            bg::details::QueueTickApply{tick_reader, bg::QueueMode::elide}.apply(output_state);
        } catch (const std::runtime_error &error) {
            failure = error.what();
        }

        CHECK(failure == (throw_before_callback ? "provider scalar read failed before callback"
                                                : "provider scalar read failed after callback"));
        CHECK(tick_reader.readable().empty());
        CHECK(provider::scalar_tick_state.reads == 1);
        CHECK(provider::scalar_tick_state.discards == 1);
        CHECK(output_time.valid() == !throw_before_callback);
        CHECK(output_storage.number == (throw_before_callback ? 0 : 79));
        CHECK(frame.target == 29);
        CHECK(resource.allocations > 0);
        CHECK(resource.allocations == resource.deallocations);
    };

    replay_with_failure(true);
    replay_with_failure(false);
    provider::throw_before_scalar_read_callback = false;
    provider::throw_after_scalar_read_callback = false;
}

TEST_CASE("a custom TSD key hook that throws after its callback does not discard the child twice",
          "[feature:time-series-storage][series:tsd][scalar:generic][queue]") {
    using Key = test_scalar_provider::bundle_provider_value;
    using Dictionary = bg::TSD<Key, bg::TS<bg::i64>>;
    const auto register_bundle_key_read_hook = [] {
        static const auto registered = [] {
            auto operations = bg::ScalarBufferOperations::for_read<Key>([](auto &reader, auto consume) {
                auto value_reader = reader.fork();
                const auto &value = value_reader.template peek<bg::ScalarValue<>>();
                reader.template consume<bg::ScalarValue<>>();
                ++provider::bundle_key_read_count;
                const auto key_view = value.view();
                consume(bg::Scalar<Key>::from(key_view));
                if (provider::throw_after_bundle_key_read_callback) {
                    throw std::runtime_error{"bundle key read failed after callback"};
                }
            });
            operations.set_discard([](auto &reader) {
                reader.template skip<bg::ScalarValue<>>();
                ++provider::bundle_key_discard_count;
            });
            return bg::ScalarDispatch<bg::ScalarBufferOperations>::instance().add(
                bg::cpp_scalar_type<Key>(), std::move(operations));
        }();
        return registered;
    };
    REQUIRE(register_bundle_key_read_hook());
    provider::bundle_key_read_count = 0;
    provider::bundle_key_discard_count = 0;
    provider::throw_after_bundle_key_read_callback = true;
    auto reset_read_failure_flag = bg::util::scope_exit{[]() noexcept {
        provider::throw_after_bundle_key_read_callback = false;
    }};

    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<Dictionary>();
    const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
    auto payload = bg::util::buffer::contiguous(256);
    {
        auto writer = bg::util::buffer_writer{payload};
        auto buffer_context = bg::ScalarBufferWriteContext{writer};
        bg::details::TickHeader{bg::details::TickOperation::tick, true}.write(buffer_context);
        bg::details::TickHeader{bg::details::TickOperation::add, true}.write(buffer_context);
        const auto key_bundle = bg::bundle<test_scalar_provider::bundle_provider_derived>(
            memory, bg::i64{79}, bg::i64{91});
        const auto key_bundle_view = key_bundle.view();
        const auto key = test_scalar_provider::bundle_provider_view{
            .value = bg::Scalar<test_scalar_provider::bundle_provider_base_type>::from(key_bundle_view),
        };
        const auto owning_key = bg::ScalarValue<Key>{key};
        buffer_context.write(bg::ScalarValue<>{owning_key, memory});
        const auto child_value = bg::i64{17};
        buffer_context.write_bytes(std::as_bytes(std::span{&child_value, std::size_t{1}}));
        bg::details::TickHeader{bg::details::TickOperation::end}.write(buffer_context);
        writer.commit();
    }
    auto failure = std::string{};
    auto reader_empty = false;
    auto key_read_count = 0;
    auto key_discard_count = 0;
    auto child_valid = false;
    auto child_value = bg::i64{};
    auto replay_action = provider::ContextAction{[&](bg::EvaluationContext &context) {
        auto tick_reader = bg::util::buffer_reader{payload};
        auto *allocation_owner = bg::details::BundleAllocationOwner::create(memory);
        auto release_allocation_owner = bg::util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
        const auto allocation_context = bg::details::ScalarAllocationContext{memory, *allocation_owner, &buffer, 0};
        auto *output_storage = memory.allocate_bytes(runtime_type.value_size(), runtime_type.value_alignment());
        runtime_type.construct_runtime_value(output_storage, memory);
        auto destroy_output = bg::util::scope_exit{[&]() noexcept {
            runtime_type.destroy_value(output_storage, memory);
            memory.deallocate_bytes(output_storage, runtime_type.value_size(), runtime_type.value_alignment());
        }};
        auto output_time = bg::TimeSeriesState{};
        auto output_state = bg::OutputState{runtime_type};
        output_state.bind(output_time, output_storage, context);
        try {
            bg::details::QueueTickApply{tick_reader, bg::QueueMode::elide}.apply(output_state);
        } catch (const std::runtime_error &error) {
            failure = error.what();
        }

        reader_empty = tick_reader.readable().empty();
        key_read_count = provider::bundle_key_read_count;
        key_discard_count = provider::bundle_key_discard_count;
        const auto output = bg::Out<Dictionary>{bg::TimeSeriesOutputBinding{output_state}};
        const auto key_value = bg::bundle<test_scalar_provider::bundle_provider_derived>(
            memory, bg::i64{79}, bg::i64{91});
        const auto key_value_view = key_value.view();
        const auto output_key = test_scalar_provider::bundle_provider_view{
            .value = bg::Scalar<test_scalar_provider::bundle_provider_base_type>::from(key_value_view),
        };
        const auto output_key_owner = bg::ScalarValue<Key>{output_key};
        const auto output_scalar_key = bg::Scalar<Key>::from(output_key_owner);
        const auto value = output[output_scalar_key];
        child_valid = value.valid();
        if (child_valid) {
            const auto item_id = output.item_id(output_scalar_key);
            child_value = *static_cast<bg::i64 *>(runtime_type.item(output_storage, item_id).value);
        }
    }};
    auto replay_action_value = bg::ScalarValue<bg::Opaque<provider::ContextAction *>>{&replay_action};
    const auto replay_node = bg::node<provider::ContextActionNode>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> trigger) { return replay_node(replay_action_value, trigger); };
    const auto graph_result = bg::testing::test_graph(graph, std::array{79});

    CHECK(graph_result.equals<bg::i64>({79}));
    CHECK(failure == "bundle key read failed after callback");
    CHECK(reader_empty);
    CHECK(key_read_count == 1);
    CHECK(key_discard_count == 1);
    CHECK(child_valid);
    CHECK(child_value == 17);
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("a failed queue provider write destroys its emitted owned payload",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {};

    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto buffer = bg::util::buffer::concurrent_chain(256, 4);
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<Value>>();
    auto writer = bg::details::QueueMessageWriter{buffer, 23, runtime_type, memory};

    CHECK_THROWS_AS(writer.write(literal::TimeSeriesLiteral(Value{91})), std::runtime_error);
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);

    auto reader = bg::util::concurrent_buffer_reader{buffer};
    const auto frame = reader.read<bg::details::QueueMessageHeader>();
    CHECK(frame.target == std::numeric_limits<std::uint32_t>::max());
    CHECK(frame.size >= sizeof(bg::details::QueueMessageHeader));
}

TEST_CASE("queue message projection does not borrow a semantic hook for an unprojected runtime writer",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {};

    const auto source = test_scalar_provider::storage{37};
    const auto value = bg::Scalar<Value>::from(test_scalar_provider::canonicalizing_runtime_type(), &source);
    const auto literal_value = literal::TimeSeriesLiteral(value);
    const auto &runtime_type = bg::time_series_value_runtime_type(
        bg::TS<Value>::type(), test_scalar_provider::canonicalizing_runtime_type());
    const auto buffer = bg::util::buffer::concurrent_chain(128, 4);
    auto writer = bg::details::QueueMessageWriter{buffer, 31, runtime_type};

    CHECK_THROWS_AS(writer.write(literal_value), std::invalid_argument);
    auto reader = bg::util::concurrent_buffer_reader{buffer};
    CHECK(reader.readable().empty());
}

TEST_CASE("a provider scalar write handler uses a concurrent reservation context",
          "[feature:scalar-storage][scalar:generic]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    auto &dispatch = bg::ScalarDispatch<bg::ScalarBufferOperations>::instance();
    const auto *semantic = dispatch.find_exact(bg::cpp_scalar_type<Value>());
    const auto *runtime = dispatch.find_exact(bg::cpp_scalar_runtime_type<Value>());
    REQUIRE(semantic != nullptr);
    REQUIRE(runtime != nullptr);

    provider::scalar_tick_state = {};
    const auto buffer = bg::util::buffer::concurrent_chain(32, 4);
    auto writer = bg::util::concurrent_buffer_writer{buffer};
    auto memory = bg::allocator{};
    const auto write = [&](int number) {
        const auto source = test_scalar_provider::storage{number};
        const auto value = bg::Scalar<Value>::from(source);
        auto reservation = writer.reserve(sizeof(int));
        auto context = bg::ScalarBufferWriteContext{reservation};
        runtime->write(bg::Scalar<>::from(value), context, memory);
        reservation.commit();
    };
    write(17);
    write(29);

    auto reader = bg::util::buffer_reader{buffer};
    auto replay = reader.fork();
    auto observed = 0;
    const auto consume = [](void *context, bg::Scalar<> value) {
        *static_cast<int *>(context) = bg::scalar_cast<Value>(value).value().get();
    };
    semantic->read(replay, {&observed, consume});
    CHECK(observed == 17);

    semantic->discard(reader);
    semantic->discard(reader);
    CHECK(provider::scalar_tick_state.writes == 2);
    CHECK(provider::scalar_tick_state.discards == 2);
    CHECK(reader.readable().empty());
}

TEST_CASE("a reserved scalar context aligns typed fields and retains owned values",
          "[feature:scalar-storage][scalar:string][storage:variable]") {
    struct alignas(std::max_align_t) aligned_value {
        explicit aligned_value(int number) : number{number} {}
        int number;
    };

    constexpr auto text = std::string_view{"an owned scalar payload that requires allocated storage"};
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto buffer = bg::util::buffer::concurrent_chain(256, 32);
    auto writer = bg::util::concurrent_buffer_writer{buffer};
    auto reservation = writer.reserve(128);
    const auto marker = std::byte{'x'};

    {
        const auto source = bg::ScalarValue<bg::string>{text, memory};
        auto context = bg::ScalarBufferWriteContext{reservation};
        context.write_bytes(std::span{&marker, std::size_t{1}});
        context.emplace<aligned_value>(42);
        context.write(bg::ScalarValue<>{source.view(), memory});
        context.write_bytes(std::span{&marker, std::size_t{1}});
        context.write(bg::ScalarValue<>{source.view(), memory});
        reservation.commit();
    }

    CHECK(resource.allocations > resource.deallocations);
    {
        auto reader = bg::util::buffer_reader{buffer};
        CHECK(reader.read_raw<std::byte>() == marker);
        CHECK(reader.read<aligned_value>().number == 42);
        {
            auto replayed = reader.read<bg::ScalarValue<>>();
            CHECK(bg::scalar_cast<bg::string>(replayed.view()).value() == text);
        }
        CHECK(reader.read_raw<std::byte>() == marker);
        reader.skip<bg::ScalarValue<>>();
        CHECK(resource.allocations == resource.deallocations);
    }
    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("discarding a provider scalar tick uses its registered handlers",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto input_state = bg::TimeSeriesState{time};
    const auto input_storage = test_scalar_provider::storage{17};
    const auto input = bg::In<bg::TS<Value>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<Value>>(), input_state, &input_storage, true}};
    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    REQUIRE(writer.write(input, time));
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<Value>::type()};

    CHECK(reader.discard());
    CHECK(provider::scalar_tick_state.discards == 1);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("a failed scalar write preserves the earlier record", "[coverage-exempt]") {
    using Value = test_scalar_provider::value;
    REQUIRE(provider::register_provider_scalar_buffer_handlers());
    provider::scalar_tick_state = {.fail_on_write = 2};
    const auto first_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto second_time = first_time + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    const auto input_storage = test_scalar_provider::storage{17};
    const auto input = bg::In<bg::TS<Value>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<Value>>(), input_state, &input_storage, true}};
    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    input_state.publish(first_time);
    REQUIRE(writer.write(input, first_time));
    const auto first_record_size = buffer.bytes().size();
    CHECK(first_record_size == 1 + sizeof(int));

    input_state.publish(second_time);
    CHECK_THROWS_AS(writer.write(input, second_time), std::runtime_error);
    CHECK(buffer.bytes().size() == first_record_size);
    CHECK(provider::scalar_tick_state.discards == 1);

    auto output_time = bg::TimeSeriesState{};
    auto output_storage = test_scalar_provider::storage{};
    auto output_state = bg::OutputState{bg::cpp_time_series_runtime_type<bg::TS<Value>>()};
    output_state.bind(output_time, &output_storage, second_time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS<Value>>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<Value>::type()};

    CHECK(reader.replay(output));
    CHECK(output_storage.number == 17);
    CHECK(provider::scalar_tick_state.discards == 2);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("runtime-specific provider write replays through its semantic handler",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    provider::register_default_tick_handlers();
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_storage = provider::SeriesViewStorage{17, 91};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_view_runtime_type(Series::type()), input_state, &input_storage, true}};

    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(input_storage.value));

    auto output_time = bg::TimeSeriesState{};
    auto output_value = bg::i64{};
    auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
    output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
    auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};

    CHECK(reader.replay(output));
    CHECK(output_value == 17);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("failed provider write rolls back all payloads in its record",
          "[feature:time-series-storage][series:generic][scalar:i64][declaration:concrete]") {
    using Series = provider::OwnedPayloadSeries;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};

    provider::payload_write_count = 0;
    auto operations = bg::TimeSeriesTickOperations::for_type<Series>(
        provider::write_second_payload_then_fail<Series>, provider::replay_owned_payload_record<Series>,
        provider::discard_owned_payload_record);
    REQUIRE(bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance().add(Series::type(),
                                                                                std::move(operations)));
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};

    {
        const auto buffer = bg::util::buffer::chain(1, memory);
        {
            auto writer = bg::TimeSeriesTickWriter{buffer, memory};
            REQUIRE(writer.write(input, time));
        }

        auto failing_writer = bg::TimeSeriesTickWriter{buffer, memory};

        input_state.publish(time + std::chrono::microseconds{1});
        CHECK_THROWS_AS(failing_writer.write(input, time + std::chrono::microseconds{1}), std::runtime_error);

        auto output_time = bg::TimeSeriesState{};
        auto output_value = bg::i64{};
        auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
        output_state.bind(output_time, &output_value, time + std::chrono::microseconds{2});
        const auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};
        auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};
        CHECK(reader.replay(output));
        CHECK(output_value == 17);
        CHECK_FALSE(reader.has_next());
    }

    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("failed TSD write releases owned payloads from completed entries",
          "[feature:time-series-storage][series:tsd][transition:update]") {
    using Series = provider::TsdFailureSeries;
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    auto buffer = bg::util::buffer::chain(256);
    provider::failed_tsd_write_count = 0;
    auto operations = bg::TimeSeriesTickOperations::for_write<Series>(
        provider::write_tsd_payload_then_fail<Series>);
    REQUIRE(operations.set_discard(provider::discard_owned_payload_record));
    REQUIRE(bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance().add(
        Series::type(), std::move(operations)));

    const auto populate = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TSD<bg::i64, Series>> output) {
        output[1].set(*input);
        output[2].set(*input + 1);
    });
    const auto serialize = bg::node([&](bg::In<bg::TSD<bg::i64, Series>> input, bg::Out<bg::TS<bool>> output) {
        auto writer = bg::TimeSeriesTickWriter{buffer, memory};
        auto failed = false;
        try {
            writer.write(input, input.last_updated());
        } catch (const std::runtime_error &) {
            failed = true;
        }
        auto reader = bg::util::buffer_reader{buffer};
        output.set(failed && provider::failed_tsd_write_count == 2 && reader.readable().empty());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return serialize(populate(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 1>{17}).equals({true}));
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("failed provider write before an owned value leaves no record",
          "[feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::FailBeforePayloadSeries;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};
    auto operations = bg::TimeSeriesTickOperations::for_write<Series>(provider::fail_before_payload<Series>);
    REQUIRE(operations.set_discard(provider::discard_owned_payload_record));
    REQUIRE(bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance().add(
        Series::type(), std::move(operations)));

    const auto buffer = bg::util::buffer::contiguous(64);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    CHECK_THROWS_AS(writer.write(input, time), std::runtime_error);
    CHECK(buffer.bytes().empty());
}

TEST_CASE("reader destruction discards unread provider-owned payloads",
          "[feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::UnreadPayloadSeries;
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};
    auto operations = bg::TimeSeriesTickOperations::for_write<Series>(provider::write_owned_payload<Series>);
    REQUIRE(operations.set_discard(provider::discard_owned_payload));
    REQUIRE(bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance().add(
        Series::type(), std::move(operations)));
    const auto buffer = bg::util::buffer::contiguous(256);

    {
        auto writer = bg::TimeSeriesTickWriter{buffer, memory};
        REQUIRE(writer.write(input, time));
        CHECK(resource.allocations > 0);

        { auto reader = bg::TimeSeriesTickReader{buffer, Series::type()}; }
    }

    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("a throwing provider replay discards its record only once",
          "[feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::ThrowingReplaySeries;
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};
    auto operations = bg::TimeSeriesTickOperations::for_type<Series>(
        provider::write_owned_payload<Series>, provider::replay_owned_payload_then_fail<Series>,
        provider::discard_owned_payload);
    REQUIRE(bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance().add(Series::type(),
                                                                                std::move(operations)));
    const auto buffer = bg::util::buffer::contiguous(256);

    {
        auto writer = bg::TimeSeriesTickWriter{buffer, memory};
        REQUIRE(writer.write(input, time));
        CHECK(resource.allocations > 0);
        auto output_time = bg::TimeSeriesState{};
        auto output_value = bg::i64{};
        auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
        output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
        auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};

        {
            auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};
            CHECK_THROWS_AS(reader.replay(output), std::runtime_error);
            CHECK_FALSE(reader.has_next());
        }
    }

    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("tick replay registration is unique per semantic time series type",
          "[evaluation][feature:time-series-storage][series:generic]") {
    using Series = provider::ReplayRegistrationSeries;
    using OtherSeries = provider::OwnedPayloadSeries;
    auto dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>{};
    auto semantic = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series, provider::view>>(
        provider::replay_tick_view<Series>);
    CHECK(dispatch.add(Series::type(), semantic));
    const auto *semantic_entry = dispatch.find_exact(Series::type());
    REQUIRE(semantic_entry != nullptr);
    CHECK(dispatch.find(provider::series_runtime_type(Series::type())) == semantic_entry);

    auto duplicate_semantic = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series>>(
        provider::replay_tick_default<Series>);
    CHECK_FALSE(dispatch.add(Series::type(), std::move(duplicate_semantic)));

    auto runtime = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series>>(
        provider::replay_tick_default<Series>);
    CHECK(dispatch.add(provider::series_runtime_type(Series::type()), std::move(runtime)));
    const auto *runtime_entry = dispatch.find_exact(provider::series_runtime_type(Series::type()));
    REQUIRE(runtime_entry != nullptr);
    CHECK(dispatch.find(provider::series_runtime_type(Series::type())) == runtime_entry);

    auto duplicate_runtime = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series, provider::view>>(
        provider::replay_tick_view<Series>);
    CHECK_FALSE(dispatch.add(provider::series_runtime_type(Series::type()), std::move(duplicate_runtime)));

    CHECK(dispatch.add(OtherSeries::type(), bg::TimeSeriesTickOperations{}));
    CHECK(dispatch.find_exact(Series::type()) == semantic_entry);
    CHECK(dispatch.find_exact(provider::series_runtime_type(Series::type())) == runtime_entry);
}

TEST_CASE("nested generic TSD replay selects the handler by item runtime representation",
          "[evaluation][feature:test-graph][feature:time-series-storage][series:tsd][series:generic]"
          "[declaration:concrete][transition:update]") {
    using Series = provider::Series<bg::i64, bg::TimeSeriesGuarantee::final>;
    CHECK(&bg::output_declaration_traits<bg::Out<Series, provider::view>>::runtime(Series::type()) ==
          &provider::series_runtime_type(Series::type()));

    auto semantic = bg::TimeSeriesTickOperations::for_write<Series>(provider::write_tick<bg::In<Series>>);
    REQUIRE(semantic.set_discard(provider::discard_tick));
    auto runtime = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series, provider::view>>(
        [](auto &reader, const auto &output) { output.set(reader.template read_raw<bg::i64>()); });
    auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
    REQUIRE(dispatch.add(Series::type(), std::move(semantic)));
    REQUIRE(dispatch.add(provider::series_runtime_type(Series::type()), std::move(runtime)));

    const auto make_dictionary = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                             bg::Out<bg::TSD<bg::i64, Series>> output) {
        output[1].set(*input);
    });
    const auto replay_dictionary = bg::node([&](bg::In<bg::TSD<bg::i64, Series>> input,
                                                bg::Out<bg::TSD<bg::i64, Series>> output) {
        const auto buffer = bg::util::buffer::contiguous(128);
        auto writer = bg::TimeSeriesTickWriter{buffer};
        if (!writer.write(input, input.last_updated())) {
            throw std::logic_error{"dictionary input did not produce a tick"};
        }

        auto reader = bg::TimeSeriesTickReader{buffer, bg::TSD<bg::i64, Series>::type()};
        if (!reader.replay(output)) {
            throw std::logic_error{"dictionary tick was not replayed"};
        }
    });
    const auto read_item = bg::node([](bg::In<bg::TSD<bg::i64, Series>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input[1].value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return read_item(replay_dictionary(make_dictionary(input)));
    };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 1>{17}).equals({17}));
}

TEST_CASE("provider replay prefers exact runtime handlers over semantic handlers",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::RuntimeReplaySeries;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_time = bg::TimeSeriesState{};
    input_time.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_time, &input_value, true}};

    auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
    auto semantic = bg::TimeSeriesTickOperations::for_type<Series, bg::Out<Series, provider::view>>(
        provider::write_tick<bg::In<Series>>, provider::replay_tick_view_override<Series>, provider::discard_tick);
    REQUIRE(dispatch.add(Series::type(), std::move(semantic)));
    auto runtime = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series, provider::view>>(
        [](auto &reader, const auto &output) { output.set(reader.template read_raw<bg::i64>() + 1000); });
    REQUIRE(dispatch.add(provider::series_view_runtime_type(Series::type()), std::move(runtime)));

    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    REQUIRE(writer.write(input, time));

    auto view_state_time = bg::TimeSeriesState{};
    auto view_value = provider::SeriesViewStorage{};
    auto view_state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    view_state.bind(view_state_time, &view_value, time + std::chrono::microseconds{1});
    const auto view_output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{view_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};

    CHECK(reader.replay(view_output));

    CHECK(view_value.value == 1017);
}

TEST_CASE("runtime replay registration does not hide semantic write registration",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::WriteRegistrationSeries;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input_data = bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
    provider::written_tick_count = 0;
    auto semantic = bg::TimeSeriesTickOperations::for_write<Series>(provider::write_tick<bg::In<Series>>);
    REQUIRE(semantic.set_discard(provider::discard_tick));
    REQUIRE(dispatch.add(Series::type(), semantic));
    CHECK_FALSE(dispatch.add(Series::type(), std::move(semantic)));
    auto runtime = bg::TimeSeriesTickOperations::for_replay<bg::Out<Series, provider::view>>(
        provider::replay_tick_view<Series>);
    REQUIRE(dispatch.add(provider::series_runtime_type(Series::type()), std::move(runtime)));
    const auto buffer = bg::util::buffer::chain(256);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    CHECK(writer.write(input, time));
    CHECK(provider::written_tick_count == 1);
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};

    auto output_time = bg::TimeSeriesState{};
    auto output_value = bg::i64{};
    auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
    output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};

    CHECK(reader.replay(output));
    CHECK(output_value == 17);
}

TEST_CASE("tick replay dispatch reaches provider-declared outputs",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::RuntimeOutputSeries;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input_data = bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<Series>(), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
    auto runtime = bg::TimeSeriesTickOperations::for_write<Series>(provider::write_tick<bg::In<Series>>);
    REQUIRE(runtime.set_replay<bg::Out<Series>>(provider::replay_tick_default<Series>));
    auto semantic = bg::TimeSeriesTickOperations::for_discard(provider::discard_tick);
    REQUIRE(dispatch.add(provider::series_runtime_type(Series::type()), std::move(runtime)));
    REQUIRE(dispatch.add(Series::type(), std::move(semantic)));
    const auto buffer = bg::util::buffer::chain(256);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    CHECK(writer.write(input, time));
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};

    auto default_output_time = bg::TimeSeriesState{};
    auto default_output_value = bg::i64{};
    auto default_output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
    default_output_state.bind(default_output_time, &default_output_value, time + std::chrono::microseconds{1});
    auto default_output = bg::Out<Series>{bg::TimeSeriesOutputBinding{default_output_state}};

    CHECK(reader.replay(default_output));
    CHECK(default_output_value == 17);
}

TEST_CASE("a node delays a configured provider output through ticks",
          "[evaluation][feature:test-graph][feature:time-series-storage][feature:timed-scheduling]"
          "[series:generic][declaration:concrete][transition:update]") {
    using Series = provider::Series<bg::i64>;
    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, provider::view> output) {
        output.set(*input);
    });
    const auto delay = bg::node<provider::OneTickDelay>();
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(delay(provide(input))); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 4>{1, 2, 3, 4});

    CHECK(actual.equals<bg::i64>({std::nullopt, 1, 2, 3, 4}));
}

TEST_CASE("feedback uses registered handlers for provider time series",
          "[evaluation][feature:feedback][feature:time-series-storage][feature:timed-scheduling]"
          "[series:generic][declaration:concrete][transition:update]") {
    using Series = provider::FeedbackSeries;
    provider::written_tick_count = 0;
    provider::replayed_tick_count = 0;
    provider::discarded_tick_count = 0;
    auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
    auto semantic = bg::TimeSeriesTickOperations::for_type<Series>(
        provider::write_tick<bg::In<Series>>, provider::replay_tick_default<Series>, provider::discard_tick);
    REQUIRE(dispatch.add(Series::type(), std::move(semantic)));

    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, provider::view> output) {
        output.set(*input);
    });
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto loop = bg::feedback<Series>("provider_feedback");
        const auto output = consume(static_cast<bg::wp<Series>>(loop));
        loop = provide(input);
        return output;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 3>{2, 5, 7});
    CHECK(actual.equals<bg::i64>({std::nullopt, 2, 5, 7}));
    CHECK(provider::written_tick_count == 3);
    CHECK(provider::replayed_tick_count == 3);
    CHECK(provider::discarded_tick_count == 3);
}

TEST_CASE("a provider output binding works in a callable node",
          "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    static_assert(!bg::details::direct_time_series_input<bg::In<Series>>);
    static_assert(bg::details::direct_time_series_output<bg::Out<Series>>);

    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series> output) { output.set(*input); });
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) { output.set(input.value()); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(provide(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{3, 8}).equals<bg::i64>({3, 8}));
}

TEST_CASE("a provider generic series resolves its scalar and guarantee variables",
          "[type][time_series][feature:type-system][declaration:generic]") {
    using Generic = provider::Series<bg::SCALAR, bg::guarantee_variable<"G">>;
    using Concrete = provider::Series<bg::i64>;
    using GenericValue = bg::TS<bg::i64, bg::guarantee_variable<"G">>;
    static_assert(bg::time_series_type_traits<Generic>::generic);
    static_assert(bg::details::direct_time_series_input<bg::In<GenericValue>>);
    static_assert(bg::details::direct_time_series_output<bg::Out<GenericValue>>);

    auto count = 0;
    const auto *guarantee = static_cast<const bg::GuaranteeVariable *>(nullptr);
    bg::for_each_declaration_variable<Generic>([&]<typename Variable>(const auto &value) {
        ++count;
        if constexpr (std::same_as<Variable, bg::GuaranteeVariableDeclaration<"G">>) {
            guarantee = &value;
        }
    });
    CHECK(count == 2);
    REQUIRE(guarantee != nullptr);

    auto type_map = bg::TypeMap{};
    CHECK(Generic::type().match(Concrete::type(), type_map));
    CHECK(&Generic::type().resolve(type_map) == &Concrete::type());
    CHECK(type_map.find(*guarantee) != nullptr);
}

TEST_CASE("a generic provider input matches a concrete provider series", "[evaluation][time_series][feature:test-graph]") {
    using Concrete = provider::Series<bg::i64>;
    using Generic = provider::Series<bg::SCALAR>;
    static_assert(!bg::details::direct_time_series_input<bg::In<Generic>>);

    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Concrete> output) { output.set(*input); });
    const auto consume = bg::node([](bg::In<Generic> input, bg::Out<bg::TS<bg::i64>> output) { output.set(input.value()); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(provide(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{4, 9}).equals<bg::i64>({4, 9}));
}

TEST_CASE("a provider scalar declaration resolves in a generic series signature",
          "[evaluation][time_series][feature:test-graph]") {
    using Concrete = provider::Series<provider::Number<bg::i64>>;
    using Generic = provider::Series<provider::Number<bg::SCALAR>>;
    static_assert(bg::scalar_declaration_traits<provider::Number<bg::SCALAR>>::generic);

    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Concrete> output) {
        output.set(*input);
    });
    const auto consume = bg::node([](bg::In<Generic> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(provide(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{11, 14}).equals<bg::i64>({11, 14}));
}

TEST_CASE("a builtin collection publishes provider items", "[evaluation][time_series][feature:test-graph]") {
    using Item = provider::Series<bg::i64>;
    using List = bg::TSL<Item, 1>;
    const auto &item_runtime_type = bg::cpp_time_series_runtime_type<Item>();
    const auto available = std::array<const bg::TimeSeriesRuntimeType *, 1>{&item_runtime_type};
    const auto &runtime_type = bg::resolve_time_series_runtime_type(List::type(), available);

    CHECK(&runtime_type.item_layout(0).runtime_type == &item_runtime_type);

    const auto &registered_runtime_type = bg::cpp_time_series_runtime_type<List>();
    const auto alternate_item_runtime_type = provider::SeriesRuntimeType{Item::type()};
    const auto alternate_available = std::array<const bg::TimeSeriesRuntimeType *, 1>{&alternate_item_runtime_type};
    const auto &alternate_runtime_type = bg::resolve_time_series_runtime_type(List::type(), alternate_available);
    CHECK(&alternate_runtime_type.item_layout(0).runtime_type == &alternate_item_runtime_type);
    CHECK(&alternate_runtime_type != &registered_runtime_type);

    const auto make_list = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<List> output) {
        output[0].set(*input);
    });
    const auto consume = bg::node([](bg::In<List> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input[0].value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(make_list(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{6, 10}).equals<bg::i64>({6, 10}));
}

TEST_CASE("an aggregate node binds configured provider outputs through public owners",
          "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series> output) { output.set(*input); });
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) { output.set(input.value()); });
    const auto aggregate = bg::node<provider::Aggregate>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(aggregate(provide(input))); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{5, 7}).equals<bg::i64>({5, 7}));
}

TEST_CASE("configured provider outputs select distinct callable and aggregate runtimes",
          "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    const auto &default_runtime = bg::cpp_time_series_runtime_type<Series>();
    const auto &view_runtime = provider::series_view_runtime_type(Series::type());
    CHECK(&default_runtime != &view_runtime);
    CHECK(default_runtime.value_size() != view_runtime.value_size());

    const auto *callable_runtime = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto callable_provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, provider::view> output) {
        output.set(*input);
    });
    const auto callable_consume = bg::node([&](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        callable_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto callable_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return callable_consume(callable_provide(input)); };

    CHECK(bg::testing::test_graph(callable_graph, std::array<bg::i64, 2>{2, 3}).equals<bg::i64>({2, 3}));
    CHECK(callable_runtime == &view_runtime);

    const auto *aggregate_runtime = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto aggregate = bg::node<provider::ViewAggregate>();
    const auto aggregate_consume = bg::node([&](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        aggregate_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto aggregate_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return aggregate_consume(aggregate(input)); };

    CHECK(bg::testing::test_graph(aggregate_graph, std::array<bg::i64, 2>{4, 5}).equals<bg::i64>({4, 5}));
    CHECK(aggregate_runtime == &view_runtime);
}

TEST_CASE("a runtime-signature node binds its configured provider output",
          "[evaluation][time_series][feature:test-graph][definition:runtime]") {
    using Series = provider::Series<bg::i64>;
    auto invalidated = false;
    provider::view_invalidation_observer = &invalidated;

    const auto provide = bg::node(RuntimeProviderViewDefinition{});
    const auto *selected_runtime = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto consume = bg::node([&](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        selected_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(provide(input)); };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 3>{12, 0, 14});
    provider::view_invalidation_observer = nullptr;

    CHECK(actual.equals<bg::i64>({12, std::nullopt, 14}));
    CHECK(invalidated);
    CHECK(selected_runtime == &provider::series_view_runtime_type(Series::type()));
}

TEST_CASE("configured output conversion rejects an incompatible semantic time series",
          "[type][time_series][feature:time-series-storage]") {
    using Series = provider::Series<bg::i64>;
    auto time = bg::TimeSeriesState{};
    auto value = provider::SeriesViewStorage{};
    auto state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    state.bind(time, &value, bg::MIN_DT + std::chrono::microseconds{1});
    const auto erased = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{state}};

    CHECK_THROWS_AS((erased.as_output<bg::Out<provider::Series<bg::string>, provider::view>>()), std::bad_cast);
}

TEST_CASE("tick replay rejects an incompatible configured output declaration",
          "[evaluation][feature:time-series-storage][series:generic]") {
    using Series = provider::IncompatibleReplaySeries;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};

    provider::incompatible_replay_called = false;
    auto &dispatch = bg::TimeSeriesDispatch<bg::TimeSeriesTickOperations>::instance();
    auto semantic = bg::TimeSeriesTickOperations::for_type<Series>(
        provider::write_tick<bg::In<Series>>, provider::replay_tick_default<Series>, provider::discard_tick);
    REQUIRE(dispatch.add(Series::type(), std::move(semantic)));
    auto runtime = bg::TimeSeriesTickOperations::for_replay<
        bg::Out<provider::Series<bg::string>, provider::view>>([](auto &, const auto &) {
        provider::incompatible_replay_called = true;
    });
    REQUIRE(dispatch.add(provider::series_view_runtime_type(Series::type()), std::move(runtime)));

    const auto buffer = bg::util::buffer::chain(128);
    auto writer = bg::TimeSeriesTickWriter{buffer};
    REQUIRE(writer.write(input, time));

    auto output_time = bg::TimeSeriesState{};
    auto output_value = provider::SeriesViewStorage{};
    auto output_state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type()};

    CHECK_THROWS_AS(reader.replay(output), std::bad_cast);
    CHECK_FALSE(provider::incompatible_replay_called);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("ordinary provider outputs use resolved declarations instead of input runtimes",
          "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    using List = bg::TSL<Series, 1>;
    const auto &default_runtime = bg::cpp_time_series_runtime_type<Series>();

    const auto *generic_runtime = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto concrete_provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series> output) {
        output.set(*input);
    });
    const auto generic_copy = bg::node(
        [](bg::In<provider::Series<bg::SCALAR>> input, bg::Out<provider::Series<bg::SCALAR>> output) {
            output.set(input.value());
        });
    const auto generic_consume = bg::node([&](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        generic_runtime = &input.runtime_type();
        output.set(input.value());
    });
    const auto generic_graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return generic_consume(generic_copy(concrete_provide(input)));
    };

    CHECK(bg::testing::test_graph(generic_graph, std::array<bg::i64, 2>{6, 7}).equals<bg::i64>({6, 7}));
    CHECK(generic_runtime == &default_runtime);

    const auto *default_runtime_after_view = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto view_provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, provider::view> output) {
        output.set(*input);
    });
    const auto default_copy = bg::node([](bg::In<Series> input, bg::Out<Series> output) { output.set(input.value()); });
    const auto default_consume = bg::node([&](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        default_runtime_after_view = &input.runtime_type();
        output.set(input.value());
    });
    const auto default_graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        return default_consume(default_copy(view_provide(input)));
    };

    CHECK(bg::testing::test_graph(default_graph, std::array<bg::i64, 2>{8, 9}).equals<bg::i64>({8, 9}));
    CHECK(default_runtime_after_view == &default_runtime);

    const auto *list_item_runtime = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto make_list = bg::node([](bg::In<Series> input, bg::Out<List> output) { output[0].set(input.value()); });
    const auto list_consume = bg::node([&](bg::In<List> input, bg::Out<bg::TS<bg::i64>> output) {
        const auto item = input[0];
        list_item_runtime = &item.runtime_type();
        output.set(item.value());
    });
    const auto list_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return list_consume(make_list(view_provide(input))); };

    CHECK(bg::testing::test_graph(list_graph, std::array<bg::i64, 2>{10, 11}).equals<bg::i64>({10, 11}));
    CHECK(list_item_runtime == &default_runtime);
}

TEST_CASE("a configured provider output invalidates through its declared interface",
          "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    auto invalidated = false;
    provider::view_invalidation_observer = &invalidated;

    const auto source = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, provider::view> output) {
        output.set(*input);
    });
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(provide(source(input))); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{12, 0}).equals<bg::i64>({12, std::nullopt}));
    CHECK(invalidated);
    provider::view_invalidation_observer = nullptr;
}

TEST_CASE("a mismatched provider selector does not register a node", "[type][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::string>;
    const auto node = bg::node([](bg::Out<Series, provider::view>) {});
    auto wiring = bg::GraphWiring{};
    auto context = bg::WiringContext{wiring};

    CHECK_THROWS_AS(node(), bg::WiringError);
    CHECK_NOTHROW(wiring.finalize());
    CHECK(wiring.nodes().empty());
    CHECK(wiring.outputs().empty());
}

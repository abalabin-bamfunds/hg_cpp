#include "../value/scalar/test_scalar_provider.hpp"

#include <bg/node.hpp>
#include <bg/graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/scalar/data.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/time_series_type_traits.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/declaration_variables.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
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
inline int written_tick_count{};

template <typename Input>
void write_tick(const Input &input, bg::TimeSeriesTickWriteContext &writer, bg::allocator &) {
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

} // namespace provider

namespace bg {

template <typename Item, auto... Properties> struct time_series_type_traits<provider::Series<Item, Properties...>> {
    using semantic_type = provider::SeriesType;

    static constexpr bool generic = scalar_declaration_traits<Item>::generic;
    static constexpr bool supported = scalar_declaration_traits<Item>::generic || scalar_type_mapped<Item>;

    template <typename, std::size_t, bool> static const TimeSeriesType &get(GuaranteeParameter guarantee) {
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
        return In<TS_T>{data_.last_updated_ref(), data_.data(), runtime_type(), data_.modified()}.visit(
            std::forward<Functions>(functions)...);
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

struct RuntimeProviderViewDefinition {
    using output_time_series_type = provider::Series<bg::i64>;
    using output_declaration_type = bg::Out<output_time_series_type, provider::view>;

    RuntimeProviderViewDefinition()
        : signature_{bg::util::symbol{"runtime_provider_view"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}},
                     output_time_series_type::type()} {}

    const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto input = invocation.input<bg::In<bg::TS<bg::i64>>>(0, 0);
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

void replay_tick_view(bg::util::buffer_reader &reader, const bg::Out<Series<bg::i64>, view> &output) {
    output.set(reader.read_raw<bg::i64>());
}

void replay_tick_default(bg::util::buffer_reader &reader, const bg::Out<Series<bg::i64>> &output) {
    output.set(reader.read_raw<bg::i64>());
}

void replay_tick_view_override(bg::util::buffer_reader &reader,
                               const bg::Out<Series<bg::i64>, view> &output) {
    output.set(reader.read_raw<bg::i64>() + 100);
}

void discard_tick(bg::util::buffer_reader &reader) {
    (void)reader.read_raw<bg::i64>();
}

bg::util::buffer tick_record(bg::i64 value) {
    auto buffer = bg::util::buffer::contiguous(32);
    auto buffer_writer = bg::util::buffer_writer{buffer};
    buffer_writer.write_bytes(std::as_bytes(std::span{&value, std::size_t{1}}));
    buffer_writer.commit();
    return buffer;
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

void write_owned_payload_value(bg::TimeSeriesTickWriteContext &writer, bg::allocator &memory, std::string value) {
    auto count = std::uint32_t{1};
    writer.write_bytes(std::as_bytes(std::span{&count, std::size_t{1}}));
    const auto marker = bg::i64{17};
    writer.write_bytes(std::as_bytes(std::span{&marker, std::size_t{1}}));
    const auto stored = bg::ScalarValue<bg::string>{std::move(value)};
    auto payload = bg::ScalarValue<>{stored, memory};
    writer.write(std::move(payload));
}

void write_owned_payload(const bg::In<Series<bg::i64>> &, bg::TimeSeriesTickWriteContext &writer,
                         bg::allocator &memory) {
    write_owned_payload_value(writer, memory, "provider-owned retained payload that needs allocated storage");
}

void write_two_owned_payloads_then_fail(const bg::In<Series<bg::i64>> &,
                                        bg::TimeSeriesTickWriteContext &writer, bg::allocator &memory) {
    auto count = std::uint32_t{2};
    writer.write_bytes(std::as_bytes(std::span{&count, std::size_t{1}}));
    const auto marker = bg::i64{29};
    writer.write_bytes(std::as_bytes(std::span{&marker, std::size_t{1}}));
    const auto first_value = bg::ScalarValue<bg::string>{
        std::string{"first provider-owned payload with allocated storage"}};
    const auto second_value = bg::ScalarValue<bg::string>{
        std::string{"second provider-owned payload with allocated storage"}};
    auto first = bg::ScalarValue<>{first_value, memory};
    auto second = bg::ScalarValue<>{second_value, memory};
    writer.write(std::move(first));
    writer.write(std::move(second));
    throw std::runtime_error{"provider write failed after writing two payloads"};
}

void replay_owned_payload_record(bg::util::buffer_reader &reader, const bg::Out<Series<bg::i64>> &output) {
    const auto count = reader.read_raw<std::uint32_t>();
    output.set(reader.read_raw<bg::i64>());
    for (auto index = std::uint32_t{}; index < count; ++index) {
        (void)reader.peek<bg::ScalarValue<>>();
        reader.consume<bg::ScalarValue<>>();
    }
}

void replay_owned_payload_then_fail(bg::util::buffer_reader &reader, const bg::Out<Series<bg::i64>> &) {
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

template <typename Output>
void replay_tick(const bg::TimeSeriesTickDispatch &dispatch, const bg::util::buffer &record, const Output &output) {
    auto buffer_reader = bg::util::buffer_reader{record};
    dispatch.replay(buffer_reader, output);
}

const bg::TimeSeriesTickDispatch &tick_dispatch() {
    static auto dispatch = bg::TimeSeriesTickDispatch{};
    static const auto registered =
        dispatch.add_write<Series<bg::i64>>(Series<bg::i64>::type(), write_tick<bg::In<Series<bg::i64>>>) &&
        dispatch.add_replay<bg::Out<Series<bg::i64>, provider::view>>(Series<bg::i64>::type(), replay_tick_view) &&
        dispatch.add_discard(Series<bg::i64>::type(), discard_tick);
    if (!registered) {
        throw std::logic_error{"provider tick handlers could not be registered"};
    }
    return dispatch;
}

struct TickDelayStorage {
    explicit TickDelayStorage(bg::allocator memory)
        : buffer{bg::util::buffer::chain(256, memory)}, reader{buffer, Series<bg::i64>::type(), tick_dispatch()},
          writer{buffer, memory, tick_dispatch()} {}

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
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input_data = bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<Series>(), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_replay<bg::Out<Series, provider::view>>(Series::type(), provider::replay_tick_view));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));
    const auto buffer = bg::util::buffer::chain(256);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    provider::written_tick_count = 0;

    CHECK(writer.write(input, time));
    CHECK(provider::written_tick_count == 1);
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};
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
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));
    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};

    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(input_value));
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};
    CHECK(reader.discard());
}

TEST_CASE("a fixed provider scalar uses its registered tick handlers",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto input_state = bg::TimeSeriesState{time};
    const auto input_storage = test_scalar_provider::storage{17};
    const auto input = bg::In<bg::TS<Value>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<Value>>(), input_state, &input_storage, true}};
    auto discard_count = 0;

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_scalar_handlers<Value>(
        [](bg::Scalar<Value> value, bg::TimeSeriesTickWriteContext &writer, bg::allocator &) {
            const auto number = value.value().get();
            writer.write_bytes(std::as_bytes(std::span{&number, std::size_t{1}}));
        },
        [](bg::util::buffer_reader &reader, auto consume) {
            const auto storage = test_scalar_provider::storage{reader.read_raw<int>()};
            consume(bg::Scalar<Value>::from(storage));
        },
        [&discard_count](bg::util::buffer_reader &reader) {
            (void)reader.read_raw<int>();
            ++discard_count;
        }));

    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(int));

    auto output_time = bg::TimeSeriesState{};
    auto output_storage = test_scalar_provider::storage{};
    auto output_state = bg::OutputState{bg::cpp_time_series_runtime_type<bg::TS<Value>>()};
    output_state.bind(output_time, &output_storage, time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS<Value>>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<Value>::type(), dispatch};

    CHECK(reader.replay(output));
    CHECK(output_storage.number == 17);
    CHECK(discard_count == 1);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("discarding a provider scalar tick uses its registered handlers",
          "[feature:time-series-storage][series:value][scalar:generic][declaration:concrete]") {
    using Value = test_scalar_provider::value;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto input_state = bg::TimeSeriesState{time};
    const auto input_storage = test_scalar_provider::storage{17};
    const auto input = bg::In<bg::TS<Value>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<Value>>(), input_state, &input_storage, true}};
    auto discard_count = 0;

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_scalar_handlers<Value>(
        [](bg::Scalar<Value> value, bg::TimeSeriesTickWriteContext &writer, bg::allocator &) {
            const auto number = value.value().get();
            writer.write_bytes(std::as_bytes(std::span{&number, std::size_t{1}}));
        },
        [](bg::util::buffer_reader &reader, auto consume) {
            const auto storage = test_scalar_provider::storage{reader.read_raw<int>()};
            consume(bg::Scalar<Value>::from(storage));
        },
        [&discard_count](bg::util::buffer_reader &reader) {
            (void)reader.read_raw<int>();
            ++discard_count;
        }));

    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    REQUIRE(writer.write(input, time));
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<Value>::type(), dispatch};

    CHECK(reader.discard());
    CHECK(discard_count == 1);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("a failed scalar write preserves the earlier record", "[coverage-exempt]") {
    using Value = test_scalar_provider::value;
    const auto first_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto second_time = first_time + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    const auto input_storage = test_scalar_provider::storage{17};
    const auto input = bg::In<bg::TS<Value>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<Value>>(), input_state, &input_storage, true}};
    auto write_count = 0;
    auto discard_count = 0;
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_scalar_handlers<Value>(
        [&write_count](bg::Scalar<Value> value, bg::TimeSeriesTickWriteContext &writer, bg::allocator &) {
            const auto number = value.value().get();
            writer.write_bytes(std::as_bytes(std::span{&number, std::size_t{1}}));
            if (++write_count == 2) {
                const auto partial = 29;
                writer.write_bytes(std::as_bytes(std::span{&partial, std::size_t{1}}));
                throw std::runtime_error{"scalar handler write failed"};
            }
        },
        [](bg::util::buffer_reader &reader, auto consume) {
            const auto storage = test_scalar_provider::storage{reader.read_raw<int>()};
            consume(bg::Scalar<Value>::from(storage));
        },
        [&discard_count](bg::util::buffer_reader &reader) {
            (void)reader.read_raw<int>();
            ++discard_count;
        }));

    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    input_state.publish(first_time);
    REQUIRE(writer.write(input, first_time));
    const auto first_record_size = buffer.bytes().size();
    CHECK(first_record_size == 1 + sizeof(int));

    input_state.publish(second_time);
    CHECK_THROWS_AS(writer.write(input, second_time), std::runtime_error);
    CHECK(buffer.bytes().size() == first_record_size);
    CHECK(discard_count == 1);

    auto output_time = bg::TimeSeriesState{};
    auto output_storage = test_scalar_provider::storage{};
    auto output_state = bg::OutputState{bg::cpp_time_series_runtime_type<bg::TS<Value>>()};
    output_state.bind(output_time, &output_storage, second_time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS<Value>>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, bg::TS<Value>::type(), dispatch};

    CHECK(reader.replay(output));
    CHECK(output_storage.number == 17);
    CHECK(discard_count == 2);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("runtime-specific provider write replays through its semantic handler",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_storage = provider::SeriesViewStorage{17, 91};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_view_runtime_type(Series::type()), input_state, &input_storage, true}};

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(provider::series_view_runtime_type(Series::type()),
                                         provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_replay<bg::Out<Series>>(Series::type(), provider::replay_tick_default));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));
    const auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    REQUIRE(writer.write(input, time));
    CHECK(buffer.bytes().size() == 1 + sizeof(input_storage.value));

    auto output_time = bg::TimeSeriesState{};
    auto output_value = bg::i64{};
    auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
    output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
    auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};

    CHECK(reader.replay(output));
    CHECK(output_value == 17);
    CHECK_FALSE(reader.has_next());
}

TEST_CASE("failed provider write rolls back all payloads in its record",
          "[feature:time-series-storage][series:generic][scalar:i64][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_owned_payload));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_owned_payload_record));
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};

    {
        const auto buffer = bg::util::buffer::chain(1, memory);
        {
            auto writer = bg::TimeSeriesTickWriter{buffer, memory, dispatch};
            REQUIRE(writer.write(input, time));
        }

        auto failing_dispatch = bg::TimeSeriesTickDispatch{};
        REQUIRE(failing_dispatch.add_write<Series>(Series::type(), provider::write_two_owned_payloads_then_fail));
        REQUIRE(failing_dispatch.add_replay<bg::Out<Series>>(
            Series::type(), provider::replay_owned_payload_record));
        REQUIRE(failing_dispatch.add_discard(Series::type(), provider::discard_owned_payload_record));
        auto failing_writer = bg::TimeSeriesTickWriter{buffer, memory, failing_dispatch};

        input_state.publish(time + std::chrono::microseconds{1});
        CHECK_THROWS_AS(failing_writer.write(input, time + std::chrono::microseconds{1}), std::runtime_error);

        auto output_time = bg::TimeSeriesState{};
        auto output_value = bg::i64{};
        auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
        output_state.bind(output_time, &output_value, time + std::chrono::microseconds{2});
        const auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};
        auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), failing_dispatch};
        CHECK(reader.replay(output));
        CHECK(output_value == 17);
        CHECK_FALSE(reader.has_next());
    }

    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("failed TSD write releases owned payloads from completed entries",
          "[feature:time-series-storage][series:tsd][transition:update]") {
    using Series = provider::Series<bg::i64>;
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    auto buffer = bg::util::buffer::chain(256);
    auto writes = std::size_t{};
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), [&](const auto &, auto &writer, auto &payload_memory) {
        if (++writes == 2) {
            throw std::runtime_error{"provider write failed in a later TSD entry"};
        }
        provider::write_owned_payload_value(writer, payload_memory,
                                            "owned payload in the first completed TSD entry");
    }));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_owned_payload_record));

    const auto populate = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TSD<bg::i64, Series>> output) {
        output[1].set(*input);
        output[2].set(*input + 1);
    });
    const auto serialize = bg::node([&](bg::In<bg::TSD<bg::i64, Series>> input, bg::Out<bg::TS<bool>> output) {
        auto writer = bg::TimeSeriesTickWriter{buffer, memory, dispatch};
        auto failed = false;
        try {
            writer.write(input, input.last_updated());
        } catch (const std::runtime_error &) {
            failed = true;
        }
        auto reader = bg::util::buffer_reader{buffer};
        output.set(failed && writes == 2 && reader.readable().empty());
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return serialize(populate(input)); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 1>{17}).equals({true}));
    CHECK(resource.allocations > 0);
    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("failed provider write before an owned value leaves no record",
          "[feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), [](const auto &, auto &, auto &) {
        throw std::runtime_error{"provider write failed before producing a value"};
    }));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_owned_payload_record));

    const auto buffer = bg::util::buffer::contiguous(64);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    CHECK_THROWS_AS(writer.write(input, time), std::runtime_error);
    CHECK(buffer.bytes().empty());
}

TEST_CASE("reader destruction discards unread provider-owned payloads",
          "[feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_owned_payload));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_owned_payload));
    const auto buffer = bg::util::buffer::contiguous(256);

    {
        auto writer = bg::TimeSeriesTickWriter{buffer, memory, dispatch};
        REQUIRE(writer.write(input, time));
        CHECK(resource.allocations > 0);

        { auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch}; }
    }

    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("a throwing provider replay discards its record only once",
          "[feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    auto resource = provider::allocation_counter{};
    auto memory = bg::allocator{resource};
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_owned_payload));
    REQUIRE(dispatch.add_replay<bg::Out<Series>>(
        Series::type(), provider::replay_owned_payload_then_fail));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_owned_payload));
    const auto buffer = bg::util::buffer::contiguous(256);

    {
        auto writer = bg::TimeSeriesTickWriter{buffer, memory, dispatch};
        REQUIRE(writer.write(input, time));
        CHECK(resource.allocations > 0);
        auto output_time = bg::TimeSeriesState{};
        auto output_value = bg::i64{};
        auto output_state = bg::OutputState{provider::series_runtime_type(Series::type())};
        output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
        auto output = bg::Out<Series>{bg::TimeSeriesOutputBinding{output_state}};

        {
            auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};
            CHECK_THROWS_AS(reader.replay(output), std::runtime_error);
            CHECK_FALSE(reader.has_next());
        }
    }

    CHECK(resource.allocations == resource.deallocations);
}

TEST_CASE("tick replay registration is unique per semantic time series type",
          "[evaluation][feature:time-series-storage][series:generic]") {
    using Series = provider::Series<bg::i64>;
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_replay<bg::Out<Series, provider::view>>(Series::type(), provider::replay_tick_view));
    CHECK_FALSE(dispatch.add_replay<bg::Out<Series>>(
        Series::type(), provider::replay_tick_default));
    REQUIRE(dispatch.add_replay<bg::Out<Series>>(
        provider::series_runtime_type(Series::type()), provider::replay_tick_default));
    CHECK_FALSE(dispatch.add_replay<bg::Out<Series, provider::view>>(
        provider::series_runtime_type(Series::type()), provider::replay_tick_view));
}

TEST_CASE("nested generic TSD replay selects the handler by item runtime representation",
          "[evaluation][feature:test-graph][feature:time-series-storage][series:tsd][series:generic]"
          "[declaration:concrete][transition:update]") {
    using Series = provider::Series<bg::i64, bg::TimeSeriesGuarantee::final>;
    CHECK(&bg::output_declaration_traits<bg::Out<Series, provider::view>>::runtime(Series::type()) ==
          &provider::series_runtime_type(Series::type()));

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_replay<bg::Out<Series, provider::view>>(
        provider::series_runtime_type(Series::type()), [](auto &reader, const auto &output) {
            output.set(reader.template read_raw<bg::i64>());
        }));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));

    const auto make_dictionary = bg::node([](bg::In<bg::TS<bg::i64>> input,
                                             bg::Out<bg::TSD<bg::i64, Series>> output) {
        output[1].set(*input);
    });
    const auto replay_dictionary = bg::node([&](bg::In<bg::TSD<bg::i64, Series>> input,
                                                bg::Out<bg::TSD<bg::i64, Series>> output) {
        const auto buffer = bg::util::buffer::contiguous(128);
        auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
        if (!writer.write(input, input.last_updated())) {
            throw std::logic_error{"dictionary input did not produce a tick"};
        }

        auto reader = bg::TimeSeriesTickReader{
            buffer, bg::TSD<bg::i64, Series>::type(), dispatch};
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
    using Series = provider::Series<bg::i64>;
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_replay<bg::Out<Series, provider::view>>(Series::type(),
                                                                        provider::replay_tick_view_override));
    REQUIRE(dispatch.add_replay<bg::Out<Series, provider::view>>(
        provider::series_view_runtime_type(Series::type()),
        [](auto &reader, const auto &output) {
            output.set(reader.template read_raw<bg::i64>() + 1000);
        }));

    const auto record = provider::tick_record(17);
    const auto output_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto view_state_time = bg::TimeSeriesState{};
    auto view_value = provider::SeriesViewStorage{};
    auto view_state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    view_state.bind(view_state_time, &view_value, output_time);
    const auto view_output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{view_state}};

    provider::replay_tick(dispatch, record, view_output);

    CHECK(view_value.value == 1017);
}

TEST_CASE("runtime replay registration does not hide semantic write registration",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input_data = bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<Series>(), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    auto dispatch = bg::TimeSeriesTickDispatch{};
    provider::written_tick_count = 0;
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_tick<bg::In<Series>>));
    CHECK_FALSE(dispatch.add_write<Series>(Series::type(), provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_replay<bg::Out<Series, provider::view>>(
        provider::series_view_runtime_type(Series::type()), provider::replay_tick_view));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));
    const auto buffer = bg::util::buffer::chain(256);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    CHECK(writer.write(input, time));
    CHECK(provider::written_tick_count == 1);
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};

    auto output_time = bg::TimeSeriesState{};
    auto output_value = provider::SeriesViewStorage{};
    auto output_state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};

    CHECK(reader.replay(output));
    CHECK(output_value.value == 17);
}

TEST_CASE("tick replay dispatch reaches provider-declared outputs",
          "[evaluation][feature:time-series-storage][series:generic][declaration:concrete]") {
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input_data = bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<Series>(), input_state, &input_value, true};
    const auto input = bg::In<Series>{input_data};

    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(provider::series_runtime_type(Series::type()),
                                         provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_replay<bg::Out<Series>>(
        provider::series_runtime_type(Series::type()), provider::replay_tick_default));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));
    const auto buffer = bg::util::buffer::chain(256);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    CHECK(writer.write(input, time));
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};

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
    using Series = provider::Series<bg::i64>;
    const auto time = bg::MIN_DT + std::chrono::microseconds{1};
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(time);
    const auto input_value = bg::i64{17};
    const auto input = bg::In<Series>{bg::TimeSeriesInputData{
        provider::series_runtime_type(Series::type()), input_state, &input_value, true}};

    auto handler_called = false;
    auto dispatch = bg::TimeSeriesTickDispatch{};
    REQUIRE(dispatch.add_write<Series>(Series::type(), provider::write_tick<bg::In<Series>>));
    REQUIRE(dispatch.add_replay<bg::Out<provider::Series<bg::string>, provider::view>>(
        provider::series_view_runtime_type(Series::type()), [&](auto &, const auto &) { handler_called = true; }));
    REQUIRE(dispatch.add_discard(Series::type(), provider::discard_tick));

    const auto buffer = bg::util::buffer::chain(128);
    auto writer = bg::TimeSeriesTickWriter{buffer, {}, dispatch};
    REQUIRE(writer.write(input, time));

    auto output_time = bg::TimeSeriesState{};
    auto output_value = provider::SeriesViewStorage{};
    auto output_state = bg::OutputState{provider::series_view_runtime_type(Series::type())};
    output_state.bind(output_time, &output_value, time + std::chrono::microseconds{1});
    const auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};
    auto reader = bg::TimeSeriesTickReader{buffer, Series::type(), dispatch};

    CHECK_THROWS_AS(reader.replay(output), std::bad_cast);
    CHECK_FALSE(handler_called);
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

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
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/declaration_variables.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace provider {

inline bool *view_invalidation_observer{};

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
    static auto types = std::map<const bg::ScalarType *, std::unique_ptr<NumberType>>{};
    const auto [entry, inserted] = types.try_emplace(&item);
    if (inserted) {
        entry->second = std::make_unique<NumberType>(item);
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
    static auto types = std::map<Key, std::unique_ptr<SeriesType>>{};
    const auto key = Key{&item, guarantee};
    const auto [entry, inserted] = types.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<SeriesType>(item, guarantee);
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
struct time_series_output_traits<provider::Series<Item, SeriesProperties...>, OutputSpecification::view> {
    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return provider::series_view_runtime_type(type);
    }
};

template <>
struct time_series_output_traits<provider::Series<string>, OutputSpecification::view> {
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
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    i64 value() const noexcept { return *static_cast<const i64 *>(data_.data()); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;

  private:
    TimeSeriesInputData data_;
};

template <typename Item, OutputSpecification Specification, auto... Properties>
class Out<provider::Series<Item, Properties...>, Specification> {
    static constexpr bool has_direct_storage =
        Specification == OutputSpecification::default_ && !scalar_declaration_traits<Item>::generic;

  public:
    using storage_type = i64;

    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(has_direct_storage)
        : binding_{last_updated, &value, cpp_time_series_runtime_type<provider::Series<Item, Properties...>>(),
                   context} {}

    void set(i64 value) const {
        *static_cast<i64 *>(binding_->mutable_data()) = value;
        binding_->publish();
    }

    void invalidate() const {
        if constexpr (Specification == OutputSpecification::view) {
            if (provider::view_invalidation_observer != nullptr) {
                *provider::view_invalidation_observer = true;
            }
        }
        binding_->invalidate();
    }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = provider::Series<Item, Properties...>;

  private:
    TimeSeriesOutputBinding binding_;
};

} // namespace bg

namespace provider {

struct Aggregate {
    bg::In<Series<bg::i64>> input;
    bg::Out<Series<bg::i64>> output;

    void evaluate() const { output.set(input.value()); }
};

struct ViewAggregate {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<Series<bg::i64>, bg::view> output;

    void evaluate() const { output.set(*input); }
};

} // namespace provider

TEST_CASE("a provider declaration exposes public semantic and runtime customizations",
          "[type][time_series][feature:type-system][declaration:runtime]") {
    using Series = provider::Series<bg::i64>;
    const auto &semantic_type = Series::type();
    const auto &runtime_type = bg::cpp_time_series_runtime_type<Series>();

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

TEST_CASE("provider views bind through public input and output owners",
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

TEST_CASE("an aggregate node binds provider views through public owners", "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series> output) { output.set(*input); });
    const auto consume = bg::node([](bg::In<Series> input, bg::Out<bg::TS<bg::i64>> output) { output.set(input.value()); });
    const auto aggregate = bg::node<provider::Aggregate>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return consume(aggregate(provide(input))); };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{5, 7}).equals<bg::i64>({5, 7}));
}

TEST_CASE("provider output specifications select distinct callable and aggregate runtimes",
          "[evaluation][time_series][feature:test-graph]") {
    using Series = provider::Series<bg::i64>;
    const auto &default_runtime = bg::cpp_time_series_runtime_type<Series>();
    const auto &view_runtime = provider::series_view_runtime_type(Series::type());
    CHECK(&default_runtime != &view_runtime);
    CHECK(default_runtime.value_size() != view_runtime.value_size());

    const auto *callable_runtime = static_cast<const bg::TimeSeriesRuntimeType *>(nullptr);
    const auto callable_provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, bg::view> output) {
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

TEST_CASE("provider default outputs use resolved declarations instead of input runtimes",
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
    const auto view_provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, bg::view> output) {
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

TEST_CASE("a provider view output invalidates through its selected declaration",
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
    const auto provide = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<Series, bg::view> output) {
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
    const auto node = bg::node([](bg::Out<Series, bg::view>) {});
    auto wiring = bg::GraphWiring{};
    auto context = bg::WiringContext{wiring};

    CHECK_THROWS_AS(node(), bg::WiringError);
    CHECK(wiring.nodes().empty());
    CHECK(wiring.outputs().empty());
}

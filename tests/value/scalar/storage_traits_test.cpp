#include "test_scalar_provider.hpp"

#include <bg/scalar/value.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/static_storage.hpp>

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

using namespace bg;

namespace {

using Value = test_scalar_provider::value;
using ReadValue = test_scalar_provider::value_view;
using Storage = test_scalar_provider::storage;

} // namespace

TEST_CASE("a scalar provider read type controls typed, erased, and time series reads",
          "[feature:scalar-api][feature:scalar-storage][series:value][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(std::same_as<typename scalar_storage_traits<Value>::stored_type, Storage>);
    STATIC_CHECK(std::same_as<typename scalar_storage_traits<Value>::value_type, ReadValue>);
    STATIC_CHECK(std::same_as<decltype(std::declval<const Scalar<Value> &>().value()), ReadValue>);
    STATIC_CHECK(std::same_as<decltype(std::declval<const In<TS<Value>> &>().value()), ReadValue>);

    auto storage = Storage{42};
    const auto typed = Scalar<Value>::from(storage);
    const auto typed_read = typed.value();
    CHECK(typed_read.get() == 42);
    CHECK(typed_read.number == std::addressof(storage.number));

    const auto owned = ScalarValue<Value>{typed};
    storage.number = 7;
    const auto owned_read = owned.view().value();
    CHECK(owned_read.get() == 42);
    CHECK(owned_read.number != std::addressof(storage.number));

    const auto erased = ScalarValue<>{owned.view()};
    const auto erased_read = unchecked_scalar_cast<Value>(erased.view()).value();
    CHECK(erased_read.get() == 42);
    CHECK(erased_read.number != owned_read.number);

    const auto state = TimeSeriesState{MIN_DT + EvaluationTime::duration{1}};
    const auto input = In<TS<Value>>{state, std::addressof(storage)};
    const auto input_read = input.value();
    CHECK(input_read.get() == 7);
    CHECK(input_read.number == std::addressof(storage.number));

    const auto generic = Scalar<SCALAR>::from(owned);
    auto generic_read = false;
    generic.visit(
        [&](const Scalar<> *value) {
            STATIC_CHECK(std::same_as<decltype(unchecked_scalar_cast<Value>(*value).value()), ReadValue>);
            CHECK(unchecked_scalar_cast<Value>(*value).value().get() == 42);
            generic_read = true;
        },
        ddv::noop);
    CHECK(generic_read);
}

namespace throwing_scalar_read_test {

struct ThrowingValue {
    explicit ThrowingValue(int value) noexcept : number{value} {}
    ThrowingValue(const ThrowingValue &other) noexcept : number{other.number} {}
    ThrowingValue(ThrowingValue &&other);
    ~ThrowingValue();

    inline static bool throw_on_move{};
    inline static int destructor_count{};
    int number;
};

struct Storage {
    int number;
};

const ScalarType &semantic_type() noexcept;
const ScalarRuntimeType &alternate_runtime();

} // namespace throwing_scalar_read_test

namespace bg {

template <> struct scalar_type_traits<throwing_scalar_read_test::ThrowingValue> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<throwing_scalar_read_test::ThrowingValue> {
    using stored_type = throwing_scalar_read_test::Storage;
    using value_type = throwing_scalar_read_test::ThrowingValue;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &stored) noexcept { return value_type{stored.number}; }
    static void construct(stored_type *destination, value_type value, allocator &) {
        std::construct_at(destination, stored_type{value.number});
    }
    static void assign(stored_type &destination, value_type value, allocator &) noexcept {
        destination.number = value.number;
    }
    static void destroy(stored_type *stored, allocator &) noexcept { std::destroy_at(stored); }
    static const ScalarRuntimeType &runtime_type(const ScalarType &type);
};

} // namespace bg

namespace throwing_scalar_read_test {

namespace {

class SemanticType final : public ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
};

class CanonicalRuntime final : public details::CppScalarRuntimeTypeBase<ThrowingValue, ScalarRuntimeType> {
    using Base = details::CppScalarRuntimeTypeBase<ThrowingValue, ScalarRuntimeType>;

  public:
    CanonicalRuntime() : Base{semantic_type()} {}
};

class AlternateRuntime final : public details::CppScalarRuntimeTypeBase<ThrowingValue, ScalarRuntimeType> {
    using Base = details::CppScalarRuntimeTypeBase<ThrowingValue, ScalarRuntimeType>;

  public:
    AlternateRuntime() : Base{semantic_type()} {}

    void read_value(ScalarDataMode, std::uintptr_t data, void *destination) const override {
        const auto *source = reinterpret_cast<const Storage *>(data);
        std::construct_at(static_cast<ThrowingValue *>(destination), source->number);
    }
};

const ScalarType &make_semantic_type() noexcept {
    static const auto type = util::static_storage<SemanticType>{};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

const ScalarRuntimeType &canonical_runtime() {
    static const auto runtime = util::static_storage<CanonicalRuntime>{};
    return runtime.get();
}

} // namespace

ThrowingValue::ThrowingValue(ThrowingValue &&other) {
    if (throw_on_move) {
        throw std::runtime_error{"throwing scalar read move"};
    }
    number = other.number;
}

ThrowingValue::~ThrowingValue() { ++destructor_count; }

const ScalarType &semantic_type() noexcept {
    return make_semantic_type();
}

const ScalarRuntimeType &alternate_runtime() {
    static const auto runtime = util::static_storage<AlternateRuntime>{};
    return runtime.get();
}

} // namespace throwing_scalar_read_test

namespace bg {

const ScalarType &scalar_type_traits<throwing_scalar_read_test::ThrowingValue>::type() noexcept {
    return throwing_scalar_read_test::semantic_type();
}

const ScalarRuntimeType &
scalar_storage_traits<throwing_scalar_read_test::ThrowingValue>::runtime_type(const ScalarType &type) {
    if (&type != &throwing_scalar_read_test::semantic_type()) {
        throw std::invalid_argument{"unexpected throwing scalar type"};
    }
    return throwing_scalar_read_test::canonical_runtime();
}

} // namespace bg

namespace reference_scalar_read_test {

struct Object {
    int number;
};

struct Value {};

struct CanonicalStorage {
    Object object;
};

struct AlternateStorage {
    std::array<std::uint32_t, 4> prefix{};
    Object object;
};

const ScalarType &semantic_type() noexcept;
const ScalarRuntimeType &alternate_runtime();

} // namespace reference_scalar_read_test

namespace bg {

template <> struct scalar_type_traits<reference_scalar_read_test::Value> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_storage_traits<reference_scalar_read_test::Value> {
    using stored_type = reference_scalar_read_test::CanonicalStorage;
    using value_type = const reference_scalar_read_test::Object &;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &stored) noexcept { return stored.object; }
    static value_type read_value(const details::ScalarData &source);

    static void construct(stored_type *destination, const reference_scalar_read_test::Object &value, allocator &) {
        std::construct_at(destination, stored_type{value});
    }

    static void assign(stored_type &destination, const reference_scalar_read_test::Object &value, allocator &) {
        destination.object = value;
    }

    static void destroy(stored_type *stored, allocator &) noexcept { std::destroy_at(stored); }
};

} // namespace bg

namespace reference_scalar_read_test {

namespace {

class SemanticType final : public ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
};

class AlternateRuntime final : public ScalarRuntimeType {
  public:
    AlternateRuntime()
        : ScalarRuntimeType{semantic_type(), sizeof(AlternateStorage), alignof(AlternateStorage), false, false, true} {
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<AlternateStorage *>(destination));
    }

    void read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        const auto source = details::ScalarData{*this, data, mode};
        std::construct_at(static_cast<Object *>(destination), scalar_storage_traits<Value>::read_value(source));
    }

    void copy_construct(void *destination, const void *source, allocator &) const override {
        std::construct_at(static_cast<AlternateStorage *>(destination),
                          *static_cast<const AlternateStorage *>(source));
    }

    std::size_t hash(const void *) const noexcept override { std::terminate(); }

    void destroy(void *value, allocator &) const noexcept override {
        std::destroy_at(static_cast<AlternateStorage *>(value));
    }

  private:
    void do_construct_from_input(void *destination, const details::ScalarData &source, allocator &) const override {
        std::construct_at(static_cast<AlternateStorage *>(destination),
                          AlternateStorage{.object = scalar_storage_traits<Value>::read_value(source)});
    }

    void do_assign_from_input(void *destination, const details::ScalarData &source) const override {
        static_cast<AlternateStorage *>(destination)->object = scalar_storage_traits<Value>::read_value(source);
    }

    bool equals_same_type(const void *, const details::ScalarData &) const noexcept override { std::terminate(); }
};

const SemanticType &make_semantic_type() noexcept {
    static const auto type = util::static_storage<SemanticType>{};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

} // namespace

const ScalarType &semantic_type() noexcept {
    return make_semantic_type();
}

const ScalarRuntimeType &alternate_runtime() {
    static const auto runtime = util::static_storage<AlternateRuntime>{};
    return runtime.get();
}

} // namespace reference_scalar_read_test

namespace bg {

const ScalarType &scalar_type_traits<reference_scalar_read_test::Value>::type() noexcept {
    return reference_scalar_read_test::semantic_type();
}

scalar_storage_traits<reference_scalar_read_test::Value>::value_type
scalar_storage_traits<reference_scalar_read_test::Value>::read_value(const details::ScalarData &source) {
    using namespace reference_scalar_read_test;

    if (source.mode() == ScalarDataMode::source_view) {
        return *static_cast<const Object *>(source.data());
    }
    if (&source.runtime_type() == &alternate_runtime()) {
        return static_cast<const AlternateStorage *>(source.data())->object;
    }
    if (&source.runtime_type() == &cpp_scalar_runtime_type<reference_scalar_read_test::Value>()) {
        return read(*static_cast<const stored_type *>(source.data()));
    }
    throw std::invalid_argument{"unsupported reference scalar runtime"};
}

} // namespace bg

namespace {

using CopyOnlyValue = test_scalar_provider::copy_only_value;

class scalar_copy_memory_resource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t allocation_size{};
    std::size_t allocation_alignment{};
    std::size_t deallocation_size{};
    std::size_t deallocation_alignment{};

  private:
    void *do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        allocation_size = size;
        allocation_alignment = alignment;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }

    void do_deallocate(void *value, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        deallocation_size = size;
        deallocation_alignment = alignment;
        std::pmr::new_delete_resource()->deallocate(value, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
};

class LegacyIntegerRuntimeType final : public ScalarRuntimeType {
  public:
    LegacyIntegerRuntimeType()
        : ScalarRuntimeType{cpp_scalar_type<i64>(), sizeof(i64), alignof(i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<i64 *>(destination));
    }

    void read_value(ScalarDataMode, std::uintptr_t data, void *destination) const override {
        std::construct_at(static_cast<i64 *>(destination), *reinterpret_cast<const i64 *>(data));
    }

    void copy_construct(void *destination, const void *source, allocator &) const override {
        std::construct_at(static_cast<i64 *>(destination), *static_cast<const i64 *>(source));
    }

    std::size_t hash(const void *value) const noexcept override {
        return std::hash<i64>{}(*static_cast<const i64 *>(value));
    }

    void destroy(void *value, allocator &) const noexcept override { std::destroy_at(static_cast<i64 *>(value)); }

  private:
    void do_construct_from_input(void *destination, const ScalarData &source, allocator &) const override {
        std::construct_at(static_cast<i64 *>(destination), details::scalar_data_value<i64>(source));
    }

    void do_assign_from_input(void *destination, const ScalarData &source) const override {
        *static_cast<i64 *>(destination) = details::scalar_data_value<i64>(source);
    }

    bool equals_same_type(const void *value, const ScalarData &other) const noexcept override {
        return *static_cast<const i64 *>(value) == details::scalar_data_value<i64>(other);
    }
};

const LegacyIntegerRuntimeType &legacy_integer_runtime() {
    static const auto runtime = util::static_storage<LegacyIntegerRuntimeType>{};
    return runtime.get();
}

struct ProviderScalarAggregateReader {
    static constexpr bool schedule_on_start = true;

    Scalar<Value> first;
    Scalar<Value> second;
    Out<TS<i64>> output;

    void evaluate() const { output.set(first.value().get() + second.value().get()); }
};

struct MixedProviderPromotedReader {
    static constexpr bool schedule_on_start = true;

    Value first;
    Value second;
    Out<TS<i64>> output;

    void evaluate() const { output.set(first.number + second.number); }
};

struct OptionalProviderPromotedReader {
    static constexpr bool schedule_on_start = true;

    std::optional<Value> value;
    Out<TS<i64>> output;

    void evaluate() const { output.set(value ? value->number : 0); }
};

struct CopyOnlyProviderAggregateReader {
    static constexpr bool schedule_on_start = true;

    Scalar<CopyOnlyValue> view;
    CopyOnlyValue promoted;
    Out<TS<i64>> output;

    void evaluate() const { output.set(view.value().number + promoted.number); }
};

static_assert(AggregateNodeClass<ProviderScalarAggregateReader>);
static_assert(AggregateNodeClass<MixedProviderPromotedReader>);
static_assert(AggregateNodeClass<OptionalProviderPromotedReader>);
static_assert(AggregateNodeClass<CopyOnlyProviderAggregateReader>);

} // namespace

TEST_CASE("a scalar provider can own a value through its borrowed read view",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(std::constructible_from<ScalarValue<Value>, test_scalar_provider::explicit_value_source>);
    STATIC_CHECK(!noexcept(details::make_scalar_source<Value>(std::declval<const ReadValue &>())));

    const auto owned = [] {
        const auto source = test_scalar_provider::explicit_value_source{.number = 42};
        return ScalarValue<Value>{source};
    }();

    CHECK(&owned.runtime_type() == &cpp_scalar_runtime_type<Value>());
    CHECK(&resolve_scalar_runtime_type(cpp_scalar_type<Value>(), {}) == &cpp_scalar_runtime_type<Value>());
    CHECK(owned.view().value().get() == 42);

    const auto empty_view = ReadValue{};
    CHECK_THROWS_WITH(details::make_scalar_source<Value>(empty_view), "a provider value view must have a source");
}

TEST_CASE("a scalar provider canonical runtime must match its storage layout",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(!noexcept(details::make_scalar_source<test_scalar_provider::layout_value>(
        std::declval<const test_scalar_provider::layout_value &>())));

    CHECK_THROWS_WITH(cpp_scalar_runtime_type<test_scalar_provider::layout_value>(),
                      "a scalar provider runtime must match its C++ storage layout");

    const auto value = test_scalar_provider::layout_value{.number = 3};
    CHECK_THROWS_WITH(details::make_scalar_source<test_scalar_provider::layout_value>(value),
                      "a scalar provider runtime must match its C++ storage layout");
}

TEST_CASE("a scalar provider validates semantic type before selecting a copy runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    const auto value = i64{7};
    const auto incompatible = Scalar<Value>::from(cpp_scalar_runtime_type<i64>(), &value);
    const auto trait_selector_calls = test_scalar_provider::trait_runtime_type_for_copy_call_count();

    CHECK_THROWS_AS(ScalarValue<Value>{incompatible}, std::invalid_argument);
    CHECK(test_scalar_provider::trait_runtime_type_for_copy_call_count() == trait_selector_calls);
}

TEST_CASE("a scalar provider read type controls collection aliases and reads",
          "[feature:scalar-collection][feature:scalar-storage][storage:fixed][declaration:concrete]") {
    STATIC_CHECK(std::same_as<List<Value>::value_type, ReadValue>);
    STATIC_CHECK(std::same_as<Set<Value>::value_type, ReadValue>);
    STATIC_CHECK(std::same_as<Dict<i64, Value>::mapped_value_type, ReadValue>);

    const auto list = ScalarValue<List<Value>>{std::vector<Value>{{1}}};
    CHECK(list.view()[0].value().get() == 1);

    const auto dictionary = ScalarValue<Dict<i64, Value>>{std::vector<std::pair<i64, Value>>{{3, {9}}}};
    CHECK(dictionary.view().at(i64{3}).value().get() == 9);
}

TEST_CASE("a scalar provider read type passes through a time series output",
          "[feature:test-graph][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:concrete][shape:single-node][binding:direct][transition:first-tick]") {
    const auto copy = node([](In<TS<Value>> input, Out<TS<Value>> output) { output.set(input.value()); });

    const auto graph = [&](wp<TS<Value>> input) { return copy(input); };
    const auto source = std::vector<Value>{{2}, {5}};

    const auto result = testing::test_graph(graph, source);

    REQUIRE(result[0]);
    REQUIRE(result[1]);
    CHECK(result[0]->view().value().get() == 2);
    CHECK(result[1]->view().value().get() == 5);
}

TEST_CASE("a typed time-series output reads its provider value",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:concrete]") {
    using Series = TS<Value>;

    const auto update_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = update_time + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{update_time};
    auto storage = Storage{42};
    auto state = details::OutputState{cpp_time_series_runtime_type<Series>()};
    state.bind(last_updated, &storage, evaluation_time);
    const auto output = Out<Series>{TimeSeriesOutputBinding{state}};

    const auto value = output.value();

    CHECK(value.get() == 42);
    CHECK(value.number == std::addressof(storage.number));
}

TEST_CASE("typed time-series leaf reads use provider hooks for alternative runtimes",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:runtime]") {
    using Series = TS<Value>;

    const auto &scalar_runtime = test_scalar_provider::preserving_runtime_type();
    const auto &runtime = time_series_value_runtime_type(Series::type(), scalar_runtime);
    const auto update_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = update_time + std::chrono::microseconds{1};
    auto input_state = TimeSeriesState{update_time};
    auto input_storage = Storage{42};
    const auto input = In<Series>{TimeSeriesInputData{runtime, input_state, &input_storage}};

    auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto input_value = input.value();
    CHECK(input_value.get() == 42);
    CHECK(input_value.number == std::addressof(input_storage.number));
    CHECK(test_scalar_provider::alternative_runtime_read_count() == reads_before + 1);

    auto output_state = details::OutputState{runtime};
    auto output_time = TimeSeriesState{update_time};
    auto output_storage = Storage{43};
    output_state.bind(output_time, &output_storage, evaluation_time);
    const auto output = Out<Series>{TimeSeriesOutputBinding{output_state}};

    reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto output_value = output.value();
    CHECK(output_value.get() == 43);
    CHECK(output_value.number == std::addressof(output_storage.number));
    CHECK(test_scalar_provider::alternative_runtime_read_count() == reads_before + 1);
}

TEST_CASE("reference-valued scalar providers read canonical, source, and alternative layouts",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][declaration:runtime]") {
    using T = reference_scalar_read_test::Value;
    using Object = reference_scalar_read_test::Object;
    using CanonicalStorage = reference_scalar_read_test::CanonicalStorage;
    using AlternateStorage = reference_scalar_read_test::AlternateStorage;
    using Series = TS<T>;

    auto canonical_storage = CanonicalStorage{.object = Object{.number = 11}};
    const auto canonical = Scalar<T>::from(cpp_scalar_runtime_type<T>(), &canonical_storage);
    const auto &canonical_value = canonical.value();
    CHECK(&canonical_value == &canonical_storage.object);

    auto alternate_storage = AlternateStorage{.object = Object{.number = 22}};
    const auto &alternate_runtime = reference_scalar_read_test::alternate_runtime();
    const auto alternate = Scalar<T>::from(alternate_runtime, &alternate_storage);
    const auto &alternate_value = alternate.value();
    CHECK(&alternate_value == &alternate_storage.object);

    const auto source_view = Object{.number = 33};
    const auto source = details::make_scalar_source<T>(source_view);
    CHECK(source.mode() == ScalarDataMode::source_view);
    CHECK(&details::scalar_data_value<T>(source) == &source_view);

    const auto &runtime = time_series_value_runtime_type(Series::type(), alternate_runtime);
    const auto update_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = update_time + std::chrono::microseconds{1};
    auto input_state = TimeSeriesState{update_time};
    const auto input = In<Series>{TimeSeriesInputData{runtime, input_state, &alternate_storage}};
    CHECK(&input.value() == &alternate_storage.object);

    auto output_state = details::OutputState{runtime};
    auto output_time = TimeSeriesState{update_time};
    auto output_storage = AlternateStorage{.object = Object{.number = 44}};
    output_state.bind(output_time, &output_storage, evaluation_time);
    const auto output = Out<Series>{TimeSeriesOutputBinding{output_state}};

    CHECK(&output.value() == &output_storage.object);
    output.set(source_view);
    CHECK(&output.value() == &output_storage.object);
    CHECK(output.value().number == 33);
}

TEST_CASE("callable promoted values and aggregate scalar views preserve provider runtime reads",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][definition:function][definition:aggregate]"
          "[declaration:concrete]") {
    auto first_storage = Storage{20};
    auto second_storage = Storage{22};
    const auto first_source = Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &first_storage);
    const auto second_source =
        Scalar<Value>::from(test_scalar_provider::canonicalizing_runtime_type(), &second_storage);
    const auto first = ScalarValue<Value>{first_source};
    const auto second = ScalarValue<Value>{second_source};

    const auto callable_reader = node([](Value value, Out<TS<i64>> output) { output.set(value.number); });

    const auto callable_graph = [&] { return callable_reader(first); };
    auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto callable_result = testing::test_graph(callable_graph);
    REQUIRE(callable_result[0]);
    CHECK(callable_result[0]->view().value() == 20);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);

    const auto aggregate_reader = node<ProviderScalarAggregateReader>();
    const auto aggregate_graph = [&] { return aggregate_reader(first, second); };
    reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto aggregate_result = testing::test_graph(aggregate_graph);
    REQUIRE(aggregate_result[0]);
    CHECK(aggregate_result[0]->view().value() == 42);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);
}

TEST_CASE("typed callable scalar inputs use the selected snapshot runtime for provider reads",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][definition:function][declaration:concrete]") {
    auto storage = Storage{20};
    const auto source = Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &storage);
    const auto scalar = ScalarValue<Value>{source};
    const ScalarRuntimeType *received_runtime{};

    const auto reader = node([&](Scalar<Value> value, Out<TS<i64>> output) {
        received_runtime = std::addressof(value.runtime_type());
        output.set(value.value().get());
    });

    const auto graph = [&] { return reader(scalar); };

    const auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto result = testing::test_graph(graph);

    REQUIRE(result[0]);
    CHECK(result[0]->view().value() == 20);
    CHECK(received_runtime == std::addressof(test_scalar_provider::immutable_snapshot_runtime_type()));
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);
}

TEST_CASE("aggregate promoted scalars select runtime metadata for scalar and time-series bindings",
          "[feature:node-api][feature:scalar-storage][definition:aggregate][declaration:runtime]") {
    auto storage = Storage{41};
    const auto source = Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &storage);
    const auto scalar = ScalarValue<Value>{source};

    const auto reader = node<MixedProviderPromotedReader>();

    const auto scalar_graph = [&] { return reader(scalar, Value{1}); };
    auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto scalar_result = testing::test_graph(scalar_graph);
    REQUIRE(scalar_result[0]);
    CHECK(scalar_result[0]->view().value() == 42);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);

    const auto time_series_graph = [&](wp<TS<Value>> input) { return reader(input, scalar); };
    reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto time_series_result = testing::test_graph(time_series_graph, std::array{Value{3}});
    REQUIRE(time_series_result[0]);
    CHECK(time_series_result[0]->view().value() == 44);
    CHECK(test_scalar_provider::alternative_runtime_read_count() > reads_before);
}

TEST_CASE("aggregate scalar views and promoted inputs preserve a copy-only provider runtime",
          "[feature:node-api][feature:scalar-storage][definition:aggregate][declaration:concrete]") {
    STATIC_CHECK(sizeof(test_scalar_provider::copy_only_runtime_storage) >
                 sizeof(test_scalar_provider::copy_only_storage));

    auto storage = test_scalar_provider::copy_only_runtime_storage{
        .prefix = {0xBAD0, 0xBAD1, 0xBAD2, 0xBAD3}, .value = {.number = 20}};
    const auto source =
        Scalar<CopyOnlyValue>::from(test_scalar_provider::preserving_copy_only_runtime_type(), &storage);
    const auto preserved = ScalarValue<CopyOnlyValue>{source};

    CHECK(&preserved.runtime_type() == &test_scalar_provider::preserving_copy_only_runtime_type());
    CHECK(preserved.view().value().number == 20);

    const auto reader = node<CopyOnlyProviderAggregateReader>();
    const auto graph = [&] { return reader(preserved, preserved); };
    const auto result = testing::test_graph(graph);

    REQUIRE(result[0]);
    CHECK(result[0]->view().value() == 40);
}

TEST_CASE("typed output writes through a prefixed scalar runtime layout",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][declaration:runtime]") {
    using Series = TS<CopyOnlyValue>;
    const auto &runtime =
        time_series_value_runtime_type(Series::type(), test_scalar_provider::preserving_copy_only_runtime_type());
    const auto update_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = update_time + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{update_time};
    auto storage = test_scalar_provider::copy_only_runtime_storage{
        .prefix = {0xBAD0, 0xBAD1, 0xBAD2, 0xBAD3}, .value = {.number = 20}};
    auto state = details::OutputState{runtime};
    state.bind(last_updated, &storage, evaluation_time);
    const auto output = Out<Series>{TimeSeriesOutputBinding{state}};

    output.set(CopyOnlyValue{42});

    CHECK((storage.prefix == std::array<std::uint32_t, 4>{0xBAD0, 0xBAD1, 0xBAD2, 0xBAD3}));
    CHECK(storage.value.number == 42);
    CHECK(output.value().number == 42);
}

TEST_CASE("aggregate optional provider scalars handle present and absent values",
          "[feature:node-api][feature:scalar-storage][definition:aggregate][declaration:concrete]"
          "[validity:optional]") {
    const auto reader = node<OptionalProviderPromotedReader>();

    const auto present_graph = [&] { return reader(std::optional<Value>{Value{17}}); };
    const auto present = testing::test_graph(present_graph);
    REQUIRE(present[0]);
    CHECK(present[0]->view().value() == 17);

    const auto absent_graph = [&] { return reader(std::optional<Value>{}); };
    const auto absent = testing::test_graph(absent_graph);
    REQUIRE(absent[0]);
    CHECK(absent[0]->view().value() == 0);
}

TEST_CASE("erased scalar borrowing uses the composed bundle runtime",
          "[feature:scalar-api][scalar:hierarchical][declaration:runtime][storage:variable]") {
    using Base = test_scalar_provider::bundle_provider_base_type;
    using Derived = test_scalar_provider::bundle_provider_derived_type;
    using Stored = details::scalar_stored_t<Base>;

    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto memory = allocator{memory_resource};
    const auto value = bundle<test_scalar_provider::bundle_provider_derived>(memory, i64{73}, i64{91});
    const auto &source = details::scalar_data(value);
    const auto stored = *static_cast<const Stored *>(source.data());

    const auto borrowed = Scalar<>::from<Base>(stored);

    CHECK(details::scalar_data(borrowed).mode() == ScalarDataMode::shared_view);
    CHECK(&borrowed.runtime_type() == &value.runtime_type());
    CHECK(&borrowed.type() == &cpp_scalar_type<Derived>());
    const auto view = scalar_cast<Base>(borrowed);
    CHECK(view.field_count() == 2);
    CHECK(view.field_name(1) == "extra");
    CHECK(scalar_cast<i64>(view[0]).value() == 73);
    CHECK(scalar_cast<i64>(view[1]).value() == 91);
}

TEST_CASE("a provider borrowed bundle view preserves derived identity and publishes through its output",
          "[feature:scalar-api][feature:scalar-storage][scalar:hierarchical][series:value]"
          "[declaration:runtime][storage:variable]") {
    using Base = test_scalar_provider::bundle_provider_base_type;
    using Derived = test_scalar_provider::bundle_provider_derived_type;
    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto memory = allocator{memory_resource};
    const auto update_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = update_time + std::chrono::microseconds{1};
    auto output_state =
        details::OutputState{cpp_time_series_runtime_type<TS<test_scalar_provider::bundle_provider_value>>()};
    auto output_time = TimeSeriesState{update_time};
    auto output_storage = details::scalar_bundle_storage{};
    output_state.bind(output_time, &output_storage, evaluation_time);
    const auto &runtime =
        static_cast<const TimeSeriesValueRuntimeType &>(output_state.runtime_type()).scalar_runtime_type();
    auto cleanup = util::scope_exit{[&]() noexcept { runtime.destroy(&output_storage, memory); }};
    const auto output = Out<TS<test_scalar_provider::bundle_provider_value>>{TimeSeriesOutputBinding{output_state}};
    const auto [owner, actual_type, actual_runtime] = [&] {
        const auto source = bundle<test_scalar_provider::bundle_provider_derived>(memory, i64{73}, i64{91});
        const auto source_view = source.view();
        const auto view = test_scalar_provider::bundle_provider_view{
            .value = Scalar<Base>::from(source_view),
        };
        const auto *type = &source.type();
        const auto *source_runtime = &source.runtime_type();
        output.set(view);
        auto owner = ScalarValue<test_scalar_provider::bundle_provider_value>{view};
        return std::tuple{std::move(owner), type, source_runtime};
    }();

    CHECK(&owner.runtime_type() == actual_runtime);
    CHECK(&owner.type() == actual_type);
    CHECK(scalar_cast<i64>(owner.view().value().value[1]).value() == 91);

    const auto published = Scalar<Base>::from(runtime, &output_storage);
    CHECK(&published.type() == actual_type);
    CHECK(scalar_cast<i64>(published[1]).value() == 91);
}

TEST_CASE("a generic provider uses its borrowed source copy policy",
          "[feature:scalar-api][feature:scalar-storage][scalar:hierarchical]"
          "[declaration:generic][storage:variable]") {
    using Generic = test_scalar_provider::generic_bundle_provider_type;
    using Derived = test_scalar_provider::bundle_provider_derived_type;

    auto memory_resource = std::pmr::monotonic_buffer_resource{};
    auto memory = allocator{memory_resource};
    const auto generic_trait_selector_calls = test_scalar_provider::generic_trait_runtime_type_for_copy_call_count();
    const auto generic_runtime_calls = test_scalar_provider::generic_runtime_type_call_count();
    const auto [owner, actual_type, actual_runtime] = [&] {
        const auto source = bundle<test_scalar_provider::bundle_provider_derived>(memory, i64{73}, i64{91});
        const auto source_view = source.view();
        auto view = test_scalar_provider::generic_bundle_provider_view{
            .value = Scalar<Generic>::from(source_view),
        };
        const auto *type = &source.type();
        const auto *runtime = &source.runtime_type();
        auto owner = ScalarValue<test_scalar_provider::generic_bundle_provider_value>{view};
        return std::tuple{std::move(owner), type, runtime};
    }();

    CHECK(test_scalar_provider::generic_trait_runtime_type_for_copy_call_count() == generic_trait_selector_calls + 1);
    CHECK(test_scalar_provider::generic_runtime_type_call_count() == generic_runtime_calls);
    CHECK(&owner.type() == actual_type);
    CHECK(&owner.runtime_type() == actual_runtime);

    const auto owner_view = owner.view();
    const auto resolved = Scalar<Generic>::from(owner_view);
    CHECK(&resolved.type() == &cpp_scalar_type<Derived>());
    CHECK(resolved.field_count() == 2);
    CHECK(resolved.field_name(1) == "extra");
    CHECK(scalar_cast<i64>(resolved[1]).value() == 91);
}

TEST_CASE("a deduplicated time-series output suppresses equal values and publishes changes",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][series:value][storage:fixed]"
          "[declaration:runtime][transition:update][guarantee:deduplicated]") {
    using DeduplicatedSeries = TS<Value, TimeSeriesGuarantee::deduplicated>;

    const auto &scalar_runtime = test_scalar_provider::preserving_runtime_type();
    const auto &deduplicated_type = DeduplicatedSeries::type();
    const auto &deduplicated_runtime = time_series_value_runtime_type(deduplicated_type, scalar_runtime);
    const auto update_time = MIN_DT + std::chrono::microseconds{1};
    const auto evaluation_time = update_time + std::chrono::microseconds{1};
    auto deduplicated_output_state = details::OutputState{deduplicated_runtime};
    auto deduplicated_output_time = TimeSeriesState{update_time};
    auto deduplicated_output_storage = Storage{42};
    auto requested_storage = Storage{42};
    deduplicated_output_state.bind(deduplicated_output_time, &deduplicated_output_storage,
                                   evaluation_time);
    const auto deduplicated_output = Out<DeduplicatedSeries>{TimeSeriesOutputBinding{deduplicated_output_state}};

    deduplicated_output.set(ReadValue{std::addressof(requested_storage.number)});

    CHECK(deduplicated_output_time.time() == update_time);
    CHECK(deduplicated_output_storage.number == 42);

    requested_storage.number = 43;
    deduplicated_output.set(ReadValue{std::addressof(requested_storage.number)});

    CHECK(deduplicated_output_time.time() == evaluation_time);
    CHECK(deduplicated_output_storage.number == 43);
}

TEST_CASE("an alternative runtime writes a provider read value",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    STATIC_CHECK(sizeof(test_scalar_provider::large_source_view) > sizeof(ReadValue));

    auto storage = Storage{42};
    const auto &runtime = test_scalar_provider::preserving_runtime_type();
    const auto reads_before = test_scalar_provider::alternative_runtime_read_count();
    const auto value = Scalar<Value>::from(runtime, std::addressof(storage));

    const auto read = value.value();
    CHECK(read.get() == 42);
    CHECK(read.number == std::addressof(storage.number));
    CHECK(test_scalar_provider::alternative_runtime_read_count() == reads_before + 1);
}

TEST_CASE("an erased owner snapshots a borrowed value using its selected runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    using SnapshotStorage = test_scalar_provider::immutable_snapshot_storage;

    STATIC_CHECK(sizeof(SnapshotStorage) > sizeof(Storage));
    STATIC_CHECK(alignof(SnapshotStorage) > alignof(Storage));

    auto resource = scalar_copy_memory_resource{};
    auto memory = allocator{resource};
    const auto source_selector_calls = test_scalar_provider::source_runtime_type_for_copy_call_count();
    const auto source_type = std::addressof(cpp_scalar_type<Value>());

    {
        const auto owner = [&] {
            auto storage = Storage{42};
            const auto source = Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &storage);
            auto copy = ScalarValue<>{source, memory};
            CHECK(std::addressof(copy.runtime_type()) ==
                  std::addressof(test_scalar_provider::immutable_snapshot_runtime_type()));
            storage.number = 7;
            return copy;
        }();

        CHECK(std::addressof(owner.type()) == source_type);
        CHECK(unchecked_scalar_cast<Value>(owner.view()).value().get() == 42);
        const auto &owned_data = details::scalar_data(owner);
        const auto *snapshot = static_cast<const SnapshotStorage *>(owned_data.data());
        CHECK(snapshot->number == 42);
        CHECK(reinterpret_cast<std::uintptr_t>(snapshot) % alignof(SnapshotStorage) == 0);
        CHECK(resource.allocations == 1);
        CHECK(resource.allocation_alignment == alignof(SnapshotStorage));
        CHECK(resource.allocation_size ==
              details::allocation_value_offset(test_scalar_provider::immutable_snapshot_runtime_type(),
                                               ScalarDataMode::owned_explicit_allocator) +
                  sizeof(SnapshotStorage));
    }

    CHECK(test_scalar_provider::source_runtime_type_for_copy_call_count() == source_selector_calls + 1);
    CHECK(resource.deallocations == 1);
    CHECK(resource.deallocation_size == resource.allocation_size);
    CHECK(resource.deallocation_alignment == resource.allocation_alignment);
}

TEST_CASE("an erased owner releases its selected runtime allocation when construction throws",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    auto storage = Storage{42};
    const auto source = Scalar<Value>::from(test_scalar_provider::preserving_runtime_type(), &storage);
    auto resource = scalar_copy_memory_resource{};
    auto memory = allocator{resource};
    auto restore = util::scope_exit{[]() noexcept { test_scalar_provider::set_snapshot_construct_throws(false); }};
    test_scalar_provider::set_snapshot_construct_throws(true);

    CHECK_THROWS_WITH((ScalarValue<>{source, memory}), "snapshot construction failed");

    CHECK(resource.allocations == 1);
    CHECK(resource.deallocations == 1);
    CHECK(resource.deallocation_size == resource.allocation_size);
    CHECK(resource.deallocation_alignment == resource.allocation_alignment);
}

TEST_CASE("a typed owner can retain an extension-selected source runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    auto storage = Storage{42};
    const auto &runtime = test_scalar_provider::preserving_runtime_type();
    const auto source = Scalar<Value>::from(runtime, std::addressof(storage));
    const auto runtime_selector_calls = test_scalar_provider::source_runtime_type_for_copy_call_count();
    const auto copy = ScalarValue<Value>{source};

    CHECK(&copy.runtime_type() == &runtime);
    CHECK(test_scalar_provider::source_runtime_type_for_copy_call_count() == runtime_selector_calls);
    storage.number = 7;
    CHECK(copy.view().value().get() == 42);
}

TEST_CASE("a typed owner can copy an alternative runtime into the canonical runtime",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    auto storage = Storage{42};
    const auto &source_runtime = test_scalar_provider::canonicalizing_runtime_type();
    const auto source = Scalar<Value>::from(source_runtime, std::addressof(storage));
    const auto runtime_selector_calls = test_scalar_provider::source_runtime_type_for_copy_call_count();
    const auto copy = ScalarValue<Value>{source};

    CHECK(&copy.runtime_type() == &cpp_scalar_runtime_type<Value>());
    CHECK(&copy.type() == &source.type());
    CHECK(test_scalar_provider::source_runtime_type_for_copy_call_count() == runtime_selector_calls);
    storage.number = 7;
    CHECK(copy.view().value().get() == 42);
}

TEST_CASE("a provider without source-aware hooks keeps its alternative-runtime behavior",
          "[feature:scalar-api][feature:scalar-storage][storage:fixed][declaration:runtime]") {
    const i64 source_value{42};
    const auto source = Scalar<i64>::from(legacy_integer_runtime(), std::addressof(source_value));
    const auto copy = ScalarValue<i64>{source};

    CHECK(source.value() == 42);
    CHECK(&copy.runtime_type() == &cpp_scalar_runtime_type<i64>());
    CHECK(copy.view().value() == 42);
}

TEST_CASE("a typed callable scalar view uses its declared storage without alternative layouts",
          "[feature:node-api][feature:scalar-api][feature:scalar-storage][definition:function][declaration:concrete]") {
    const i64 source_value{42};
    const auto source = Scalar<i64>::from(legacy_integer_runtime(), std::addressof(source_value));
    auto erased = ScalarValue<>{source};
    const ScalarRuntimeType *received_runtime{};

    const auto read = node([&](Scalar<i64> input, Out<TS<i64>> output) {
        received_runtime = std::addressof(input.runtime_type());
        output.set(input.value());
    });

    const auto graph = [&] { return read(std::move(erased)); };

    CHECK(std::addressof(erased.runtime_type()) == std::addressof(legacy_integer_runtime()));
    CHECK(testing::test_graph(graph).equals({42}));
    CHECK(received_runtime == std::addressof(cpp_scalar_runtime_type<i64>()));
}

TEST_CASE("an alternative scalar read destroys its temporary when moving the value throws",
          "[feature:scalar-api][feature:scalar-storage][declaration:runtime]") {
    using Value = throwing_scalar_read_test::ThrowingValue;

    Value::destructor_count = 0;
    Value::throw_on_move = true;
    const auto storage = throwing_scalar_read_test::Storage{42};
    const auto source = Scalar<Value>::from(throwing_scalar_read_test::alternate_runtime(), &storage);

    CHECK_THROWS_AS(source.value(), std::runtime_error);
    CHECK(Value::destructor_count == 1);
    Value::throw_on_move = false;
}

TEST_CASE("an opaque scalar containing ScalarData keeps its ordinary stored-value read",
          "[feature:scalar-api][feature:scalar-storage][scalar:opaque][storage:fixed]") {
    const i64 payload{42};
    const auto descriptor = details::ScalarData{cpp_scalar_runtime_type<i64>(), &payload};
    const auto value = ScalarValue<Opaque<ScalarData>>{descriptor};
    const auto view = value.view();

    CHECK(&view.value() == static_cast<const ScalarData *>(view.data()));
    CHECK(&view.value().runtime_type() == &cpp_scalar_runtime_type<i64>());
    CHECK(view.value().data() == &payload);
}

#include "test_scalar_provider.hpp"

#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ts/ts_input.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <memory>
#include <utility>
#include <vector>

namespace {

using Value = test_scalar_provider::value;
using ReadValue = test_scalar_provider::value_view;
using Storage = test_scalar_provider::storage;

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

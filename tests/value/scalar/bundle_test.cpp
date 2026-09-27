#include "../../utils/counting_memory_resource.hpp"

#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/scalar/bundle/details/value_factory.hpp>
#include <bg/scalar/opaque.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory_resource>
#include <optional>
#include <string_view>

namespace {

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
};

template <template <typename, auto...> typename Field> struct Equity : bg::schema_extends<Instrument, Field> {
    Field<bg::i64> lot_size;
};

template <template <typename, auto...> typename Field> struct Position {
    Field<bg::Bundle<Instrument>> instrument;
};

template <template <typename, auto...> typename Field> struct RecursiveValue {
    Field<bg::i64> value;
    Field<bg::Bundle<RecursiveValue>, bg::optional> next;
};

struct Finalizable {
    Finalizable(int &count, bool count_destruction = false) noexcept
        : count{&count}, count_destruction{count_destruction} {}

    Finalizable(const Finalizable &other) noexcept : count{other.count}, count_destruction{true} {}

    Finalizable(Finalizable &&other) noexcept : count{other.count}, count_destruction{other.count_destruction} {
        other.count_destruction = false;
    }

    ~Finalizable() noexcept {
        if (count_destruction) {
            ++*count;
        }
    }

    int *count;
    bool count_destruction;
};

template <template <typename, auto...> typename Field> struct FinalizableRecord {
    Field<bg::Opaque<Finalizable>> value;
};

template <template <typename, auto...> typename Field> struct HashableRecord {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field> struct UnhashableRecord {
    Field<bg::Opaque<Finalizable>> value;
};

template <typename T>
concept HashableScalar = requires(const bg::Scalar<T> &value) { value.hash(); };

} // namespace

static_assert(sizeof(bg::Scalar<bg::Bundle<Instrument>>) == 2 * sizeof(void *));
static_assert(sizeof(bg::ScalarValue<bg::Bundle<Instrument>>) == 2 * sizeof(void *));
static_assert(HashableScalar<bg::Bundle<HashableRecord>>);
static_assert(!HashableScalar<bg::Bundle<UnhashableRecord>>);

TEST_CASE("hierarchical scalar values expose required and optional fields",
          "[value][feature:scalar-api][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
    const auto fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), std::nullopt};
    const auto value = bg::details::HierarchicalScalarValueFactory::make<Instrument>(fields);
    const auto view = value.view();

    CHECK(view.field_count() == 2);
    CHECK(view.field_name(0) == "symbol");
    CHECK(view.present(0));
    CHECK_FALSE(view.present(1));
    CHECK(bg::scalar_cast<bg::symbol>(view[0]).value().text() == "ABC");
    CHECK_THROWS_AS(view[1], std::bad_optional_access);
}

TEST_CASE("copying a hierarchical scalar retains its shared value",
          "[value][feature:scalar-storage][scalar:hierarchical][storage:variable]") {
    const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
    const auto price = bg::ScalarValue{12.5};
    const auto fields =
        std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), bg::Scalar<>::from(price)};
    const auto value = bg::details::HierarchicalScalarValueFactory::make<Instrument>(fields);
    const auto copy = bg::ScalarValue<bg::Bundle<Instrument>>{value.view()};

    CHECK(copy == value);
}

TEST_CASE("hierarchical scalar equality and hashing include optional-field presence",
          "[value][feature:scalar-api][scalar:hierarchical][validity:optional]") {
    const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
    const auto price = bg::ScalarValue{0.0};
    const auto absent_fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), std::nullopt};
    const auto present_fields =
        std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), bg::Scalar<>::from(price)};

    const auto left = bg::details::HierarchicalScalarValueFactory::make<Instrument>(absent_fields);
    const auto right = bg::details::HierarchicalScalarValueFactory::make<Instrument>(absent_fields);
    const auto present = bg::details::HierarchicalScalarValueFactory::make<Instrument>(present_fields);

    CHECK(left.equals(right));
    CHECK(left.hash() == right.hash());
    CHECK_FALSE(left.equals(present));
}

TEST_CASE("a hierarchical scalar visitor reaches the extension catch-all",
          "[value][feature:visitor][scalar:hierarchical][declaration:runtime]") {
    const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
    const auto fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), std::nullopt};
    const auto value = bg::details::HierarchicalScalarValueFactory::make<Instrument>(fields);
    auto called = false;

    value.view().visit(
        [&](const bg::Scalar<> *view) {
            called = true;
            CHECK(&view->type() == &value.type());
        },
        ddv::noop);

    CHECK(called);
}

TEST_CASE("a hierarchical scalar keeps its allocation owner alive",
          "[value][feature:scalar-storage][scalar:hierarchical][storage:variable]") {
    auto memory = counting_memory_resource{};
    {
        auto allocator = bg::allocator{memory};
        const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
        const auto fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), std::nullopt};
        const auto value = bg::details::HierarchicalScalarValueFactory::make<Instrument>(fields, &allocator);

        CHECK(value.view().present(0));
        CHECK(memory.allocations() != 0);
    }

    CHECK(memory.allocations() == 0);
    CHECK(memory.allocated_bytes() == 0);
}

TEST_CASE("a hierarchical scalar survives its global state",
          "[value][feature:global-state][scalar:hierarchical][storage:variable]") {
    auto retained = std::optional<bg::ScalarValue<bg::Bundle<Instrument>>>{};
    {
        auto state = bg::GlobalState{};
        state.write([&](auto &values) {
            const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
            const auto fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), std::nullopt};
            const auto value = bg::details::HierarchicalScalarValueFactory::make<Instrument>(fields);

            values.set(bg::util::symbol{"instrument"}, value);
            retained.emplace(value.view());
        });
    }

    REQUIRE(retained.has_value());
    CHECK(bg::scalar_cast<bg::symbol>(retained->view()[0]).value().text() == "ABC");
}

TEST_CASE("a hierarchical scalar survives its runtime graph",
          "[value][feature:lifecycle][scalar:hierarchical][storage:variable]"
          "[transition:lifecycle]") {
    auto wiring = bg::GraphWiring{};
    const auto compiled = bg::CompiledGraph{wiring};
    auto retained = std::optional<bg::ScalarValue<bg::Bundle<Instrument>>>{};
    {
        auto runtime = bg::RuntimeGraph{compiled};
        runtime.global_state().write([&](auto &values) {
            const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
            const auto fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(symbol), std::nullopt};
            const auto value = bg::details::HierarchicalScalarValueFactory::make<Instrument>(fields);

            values.set(bg::util::symbol{"instrument"}, value);
            retained.emplace(value.view());
        });
    }

    REQUIRE(retained.has_value());
    CHECK(bg::scalar_cast<bg::symbol>(retained->view()[0]).value().text() == "ABC");
}

TEST_CASE("a hierarchical scalar destroys its fields after its final release",
          "[value][feature:finalization][scalar:hierarchical][storage:variable]"
          "[transition:lifecycle]") {
    auto destruction_count = 0;
    const auto source = Finalizable{destruction_count};
    const auto field = bg::ScalarValue<bg::Opaque<Finalizable>>{source};
    {
        const auto fields = std::array<std::optional<bg::Scalar<>>, 1>{bg::Scalar<>::from(field)};
        const auto value = bg::details::HierarchicalScalarValueFactory::make<FinalizableRecord>(fields);
        {
            const auto copy = bg::ScalarValue<bg::Bundle<FinalizableRecord>>{value.view()};
            CHECK(copy.view().present(0));
        }

        CHECK(destruction_count == 0);
    }

    CHECK(destruction_count == 1);
}

TEST_CASE("a base hierarchical view preserves the derived runtime type",
          "[value][feature:scalar-api][scalar:hierarchical][schema:named][binding:covariant]"
          "[representation:covariant]") {
    const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
    const auto price = bg::ScalarValue{12.5};
    const auto lot_size = bg::ScalarValue{bg::i64{100}};
    const auto fields = std::array<std::optional<bg::Scalar<>>, 3>{
        bg::Scalar<>::from(symbol), bg::Scalar<>::from(price), bg::Scalar<>::from(lot_size)};
    const auto derived = bg::details::HierarchicalScalarValueFactory::make<Equity>(fields);
    const auto base = bg::scalar_cast<bg::Bundle<Instrument>>(derived);

    CHECK(&base.type() == &derived.type());
    CHECK(base.field_count() == 2);
    CHECK(bg::scalar_cast<bg::symbol>(base[0]).value().text() == "ABC");
    CHECK(bg::scalar_cast<double>(base[1]).value() == 12.5);
}

TEST_CASE("a hierarchical field preserves a derived value stored through its base type",
          "[value][feature:scalar-storage][scalar:hierarchical][schema:named][binding:covariant]"
          "[representation:covariant]") {
    const auto symbol = bg::ScalarValue{bg::util::symbol{"ABC"}};
    const auto price = bg::ScalarValue{12.5};
    const auto lot_size = bg::ScalarValue{bg::i64{100}};
    const auto equity_fields = std::array<std::optional<bg::Scalar<>>, 3>{
        bg::Scalar<>::from(symbol), bg::Scalar<>::from(price), bg::Scalar<>::from(lot_size)};
    const auto equity = bg::details::HierarchicalScalarValueFactory::make<Equity>(equity_fields);
    const auto position_fields = std::array<std::optional<bg::Scalar<>>, 1>{bg::Scalar<>::from(equity)};
    const auto position = bg::details::HierarchicalScalarValueFactory::make<Position>(position_fields);

    const auto instrument = bg::scalar_cast<bg::Bundle<Instrument>>(position.view()[0]);
    CHECK(&instrument.type() == &equity.type());
    CHECK(bg::scalar_cast<bg::symbol>(instrument[0]).value().text() == "ABC");
}

TEST_CASE("a recursive hierarchical scalar runtime type supports an absent recursive field",
          "[value][feature:scalar-storage][scalar:hierarchical][schema:named][validity:optional]") {
    const auto field = bg::ScalarValue{bg::i64{42}};
    const auto fields = std::array<std::optional<bg::Scalar<>>, 2>{bg::Scalar<>::from(field), std::nullopt};
    const auto value = bg::details::HierarchicalScalarValueFactory::make<RecursiveValue>(fields);

    CHECK(bg::scalar_cast<bg::i64>(value.view()[0]).value() == 42);
    CHECK_FALSE(value.view().present(1));
}

#include "../../utils/counting_memory_resource.hpp"

#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/opaque.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

template <template <typename, auto...> typename> struct EmptyRecord {};

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
};

template <template <typename, auto...> typename Field> struct Equity : bg::schema_extends<Instrument, Field> {
    Field<bg::i64> lot_size;
};

template <template <typename, auto...> typename Field> struct UnrelatedRecord {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field> struct Position {
    Field<bg::Bundle<Instrument>> instrument;
};

template <template <typename, auto...> typename Field> struct InstrumentList {
    Field<bg::List<bg::Bundle<Instrument>>> instruments;
};

template <template <typename, auto...> typename Field> struct InstrumentDictionary {
    Field<bg::Dict<bg::i64, bg::Bundle<Instrument>>> instruments;
};

template <template <typename, auto...> typename Field> struct RecursiveValue {
    Field<bg::i64> value;
    Field<bg::Bundle<RecursiveValue>, bg::optional> next;
};

template <template <typename, auto...> typename Field> struct RecursiveCollectionValue {
    Field<bg::i64> value;
    Field<bg::List<bg::Bundle<RecursiveCollectionValue>>, bg::optional> children;
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

class AlternateHierarchicalRuntimeType final : public bg::HierarchicalScalarRuntimeType {
  public:
    explicit AlternateHierarchicalRuntimeType(const bg::HierarchicalScalarType &type) noexcept
        : HierarchicalScalarRuntimeType{type, sizeof(void *), alignof(void *), false, false, true} {}

    bg::details::ScalarData field(const bg::details::ScalarData &, std::size_t) const override {
        std::terminate();
    }

    bool field_present(const bg::details::ScalarData &, std::size_t) const override { return false; }

  private:
    std::uintptr_t construct_shared(std::span<const bg::details::ScalarData *const>,
                                    bg::details::BundleAllocationOwner &) const override {
        std::terminate();
    }

    void construct_initial_value(void *destination) const noexcept override {
        *static_cast<void **>(destination) = nullptr;
    }

    void do_construct_from_input(void *, const bg::details::ScalarData &, bg::allocator &) const override {
        std::terminate();
    }

    void copy_construct(void *, const void *, bg::allocator &) const override { std::terminate(); }

    void do_assign_from_input(void *, const bg::details::ScalarData &) const override { std::terminate(); }

    std::size_t hash(const void *) const noexcept override { return 0; }

    bool equals_same_type(const void *, const bg::details::ScalarData &) const noexcept override { return false; }

    void destroy(void *, bg::allocator &) const noexcept override {}
};

} // namespace

static_assert(sizeof(bg::Scalar<bg::Bundle<Instrument>>) == 2 * sizeof(void *));
static_assert(sizeof(bg::ScalarValue<bg::Bundle<Instrument>>) == 2 * sizeof(void *));
static_assert(sizeof(bg::Bundle<Instrument>) == sizeof(void *));
static_assert(std::constructible_from<bg::ScalarValue<bg::Bundle<Instrument>>, bg::Scalar<bg::Bundle<Equity>>>);
static_assert(
    std::constructible_from<bg::ScalarValue<bg::Bundle<Instrument>>, const bg::ScalarValue<bg::Bundle<Equity>> &>);
static_assert(
    !std::constructible_from<bg::ScalarValue<bg::Bundle<Instrument>>, const bg::ScalarValue<bg::Bundle<Instrument>> &>);
static_assert(HashableScalar<bg::Bundle<HashableRecord>>);
static_assert(!HashableScalar<bg::Bundle<UnhashableRecord>>);

TEST_CASE("bundle constructs an empty hierarchical scalar",
          "[feature:scalar-api][scalar:hierarchical][schema:named]") {
    const auto value = bg::bundle<EmptyRecord>();

    CHECK(value.view().field_count() == 0);
}

TEST_CASE("bundle constructs a hierarchical scalar from positional and named arguments",
          "[feature:scalar-api][scalar:hierarchical][schema:named][validity:optional]") {
    using namespace bg::literals;

    const auto value = bg::bundle<Instrument>("price"_n = 12.5, "symbol"_n = bg::symbol{"ABC"});
    const auto view = value.view();

    STATIC_CHECK(std::same_as<std::remove_cvref_t<decltype(value)>, bg::ScalarValue<bg::Bundle<Instrument>>>);
    CHECK(bg::scalar_cast<bg::symbol>(view[0]).value().text() == "ABC");
    CHECK(bg::scalar_cast<double>(view[1]).value() == 12.5);
}

TEST_CASE("bundle uses inherited logical field order and permits omitted optional fields",
          "[feature:scalar-api][scalar:hierarchical][schema:named][binding:covariant][validity:optional]") {
    using namespace bg::literals;

    const auto value = bg::bundle<Equity>("symbol"_n = bg::symbol{"ABC"}, "lot_size"_n = 100);
    const auto view = value.view();

    REQUIRE(view.field_count() == 3);
    CHECK(bg::scalar_cast<bg::symbol>(view[0]).value().text() == "ABC");
    CHECK_FALSE(view.present(1));
    CHECK(bg::scalar_cast<bg::i64>(view[2]).value() == 100);
}

TEST_CASE("bundle constructs nested and recursive hierarchical scalars",
          "[feature:scalar-api][scalar:hierarchical][schema:named][shape:nested][validity:optional]") {
    using namespace bg::literals;

    const auto instrument = bg::bundle<Instrument>(bg::symbol{"ABC"});
    const auto position = bg::bundle<Position>(instrument.view());
    const auto leaf = bg::bundle<RecursiveValue>(1);
    const auto parent = bg::bundle<RecursiveValue>(2, "next"_n = leaf.view());

    const auto stored_instrument = bg::scalar_cast<bg::Bundle<Instrument>>(position.view()[0]);
    const auto stored_leaf = bg::scalar_cast<bg::Bundle<RecursiveValue>>(parent.view()[1]);
    CHECK(bg::scalar_cast<bg::symbol>(stored_instrument[0]).value().text() == "ABC");
    CHECK(bg::scalar_cast<bg::i64>(stored_leaf[0]).value() == 1);
    CHECK_FALSE(stored_leaf.present(1));
}

TEST_CASE("bundle rejects invalid field assignments",
          "[feature:scalar-api][scalar:hierarchical][schema:named][validity:required]") {
    using namespace bg::literals;

    const auto integer = bg::ScalarValue{bg::i64{1}};

    CHECK_THROWS_AS(bg::bundle<Instrument>(), std::invalid_argument);
    CHECK_THROWS_AS(bg::bundle<Instrument>("missing"_n = bg::symbol{"ABC"}), std::invalid_argument);
    CHECK_THROWS_AS(
        bg::bundle<Instrument>("symbol"_n = bg::symbol{"ABC"}, "symbol"_n = bg::symbol{"DEF"}),
        std::invalid_argument);
    CHECK_THROWS_AS(bg::bundle<Instrument>("price"_n = 12.5, bg::symbol{"ABC"}), std::invalid_argument);
    CHECK_THROWS_AS(bg::bundle<Instrument>(integer), std::invalid_argument);
}

TEST_CASE("bundle releases explicit allocator storage after failed construction",
          "[feature:scalar-storage][feature:finalization][scalar:hierarchical][storage:variable]") {
    auto memory = counting_memory_resource{};
    auto allocator = bg::allocator{memory};
    const auto incompatible = bg::ScalarValue{bg::i64{1}};

    CHECK_THROWS_AS(bg::bundle<Instrument>(allocator, bg::symbol{"ABC"}, incompatible), std::invalid_argument);
    CHECK(memory.allocations() == 0);
    CHECK(memory.allocated_bytes() == 0);
}

TEST_CASE("hierarchical scalar values expose required and optional fields",
          "[value][feature:scalar-api][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto value = bg::bundle<Instrument>(bg::symbol{"ABC"});
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
    const auto value = bg::bundle<Instrument>(bg::symbol{"ABC"}, 12.5);
    const auto copy = bg::ScalarValue<bg::Bundle<Instrument>>{value.view()};

    CHECK(copy == value);
}

TEST_CASE("hierarchical scalar equality and hashing include optional-field presence",
          "[value][feature:scalar-api][scalar:hierarchical][validity:optional]") {
    const auto left = bg::bundle<Instrument>(bg::symbol{"ABC"});
    const auto right = bg::bundle<Instrument>(bg::symbol{"ABC"});
    const auto present = bg::bundle<Instrument>(bg::symbol{"ABC"}, 0.0);

    CHECK(left.equals(right));
    CHECK(left.hash() == right.hash());
    CHECK_FALSE(left.equals(present));
}

TEST_CASE("a hierarchical scalar visitor reaches the extension catch-all",
          "[value][feature:visitor][scalar:hierarchical][declaration:runtime]") {
    const auto value = bg::bundle<Instrument>(bg::symbol{"ABC"});
    auto called = false;

    value.view().visit(
        [&](const bg::Scalar<> *view) {
            called = true;
            CHECK(&view->type() == &value.type());
        },
        ddv::noop);

    CHECK(called);
}

TEST_CASE("a scalar list stores hierarchical values and preserves their actual type",
          "[value][feature:scalar-storage][scalar:list][scalar:hierarchical][binding:covariant]") {
    const auto equity = bg::bundle<Equity>(bg::symbol{"ABC"}, 12.5, bg::i64{100});
    const auto instrument = bg::scalar_cast<bg::Bundle<Instrument>>(equity);
    const auto items = std::array<bg::Scalar<bg::Bundle<Instrument>>, 1>{instrument};

    const auto list = bg::ScalarValue<bg::List<bg::Bundle<Instrument>>>{items};
    const auto equivalent = bg::ScalarValue<bg::List<bg::Bundle<Instrument>>>{items};
    const auto copy = bg::ScalarValue<bg::List<bg::Bundle<Instrument>>>{list.view()};
    const auto stored = list.view()[0];
    const auto copied = copy.view()[0];

    CHECK(&stored.type() == &equity.type());
    CHECK(&copied.type() == &equity.type());
    CHECK(bg::scalar_cast<bg::symbol>(stored[0]).value().text() == "ABC");
    CHECK(bg::scalar_cast<double>(stored[1]).value() == 12.5);
    CHECK(list.equals(equivalent));
    CHECK(list.hash() == equivalent.hash());
}

TEST_CASE("a scalar collection rejects an incompatible hierarchical item",
          "[value][feature:scalar-storage][scalar:list][scalar:hierarchical][declaration:concrete]") {
    const auto value = bg::bundle<UnrelatedRecord>(bg::i64{42});
    const auto items = std::array<bg::Scalar<bg::Bundle<UnrelatedRecord>>, 1>{value.view()};

    CHECK_THROWS_AS((bg::ScalarValue<bg::List<bg::Bundle<Instrument>>>{items}), std::invalid_argument);
}

TEST_CASE("an owning base hierarchical scalar retains a derived value",
          "[value][feature:scalar-storage][scalar:hierarchical][binding:covariant]") {
    auto from_view = std::optional<bg::ScalarValue<bg::Bundle<Instrument>>>{};
    auto from_value = std::optional<bg::ScalarValue<bg::Bundle<Instrument>>>{};
    const auto *derived_type = static_cast<const bg::ScalarType *>(nullptr);
    {
        auto state = bg::GlobalState{};
        state.write([&](auto &) {
            const auto equity = bg::bundle<Equity>(bg::symbol{"ABC"}, 12.5, bg::i64{100});

            derived_type = &equity.type();
            from_view.emplace(equity.view());
            from_value.emplace(equity);
        });
    }

    REQUIRE(from_view.has_value());
    REQUIRE(from_value.has_value());
    CHECK(&from_view->type() == derived_type);
    CHECK(&from_value->type() == derived_type);
    CHECK(bg::scalar_cast<bg::i64>(bg::scalar_cast<bg::Bundle<Equity>>(*from_view)[2]).value() == 100);
    CHECK(bg::scalar_cast<bg::i64>(bg::scalar_cast<bg::Bundle<Equity>>(*from_value)[2]).value() == 100);
}

TEST_CASE("an owning hierarchical scalar rejects an unrelated schema",
          "[value][feature:scalar-storage][scalar:hierarchical][declaration:concrete]") {
    const auto value = bg::bundle<UnrelatedRecord>(bg::i64{42});

    CHECK_THROWS_AS((bg::ScalarValue<bg::Bundle<Instrument>>{value.view()}), std::invalid_argument);
    CHECK_THROWS_AS((bg::ScalarValue<bg::Bundle<Instrument>>{value}), std::invalid_argument);
}

TEST_CASE("a scalar dictionary stores hierarchical mapped values",
          "[value][feature:scalar-storage][scalar:dict][scalar:hierarchical][declaration:concrete]") {
    const auto instrument = bg::bundle<Instrument>(bg::symbol{"ABC"});
    const auto items =
        std::array<std::pair<bg::i64, bg::Scalar<bg::Bundle<Instrument>>>, 1>{std::pair{bg::i64{1}, instrument.view()}};

    const auto dictionary = bg::ScalarValue<bg::Dict<bg::i64, bg::Bundle<Instrument>>>{items};
    const auto stored = dictionary.view().at(1);

    CHECK(bg::scalar_cast<bg::symbol>(stored[0]).value().text() == "ABC");
    CHECK_FALSE(stored.present(1));
}

TEST_CASE("a collection retains hierarchical fields until its final release",
          "[value][feature:finalization][scalar:list][scalar:hierarchical][storage:variable]"
          "[transition:lifecycle]") {
    auto memory = counting_memory_resource{};
    auto allocator = bg::allocator{memory};
    auto destruction_count = 0;
    auto retained = std::optional<bg::ScalarValue<bg::List<bg::Bundle<FinalizableRecord>>>>{};
    {
        const auto source = Finalizable{destruction_count};
        const auto field = bg::ScalarValue<bg::Opaque<Finalizable>>{source, allocator};
        const auto value = bg::bundle<FinalizableRecord>(allocator, field);
        const auto items = std::array<bg::Scalar<bg::Bundle<FinalizableRecord>>, 1>{value.view()};

        retained.emplace(items, allocator);
    }

    CHECK(destruction_count == 1);
    REQUIRE(retained.has_value());
    CHECK(retained->view().size() == 1);

    retained.reset();

    CHECK(destruction_count == 2);
    CHECK(memory.allocations() == 0);
    CHECK(memory.allocated_bytes() == 0);
}

TEST_CASE("a hierarchical scalar keeps its allocation owner alive",
          "[value][feature:scalar-storage][scalar:hierarchical][storage:variable]") {
    auto memory = counting_memory_resource{};
    {
        auto allocator = bg::allocator{memory};
        const auto value = bg::bundle<Instrument>(allocator, bg::symbol{"ABC"});

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
            const auto value = bg::bundle<Instrument>(bg::symbol{"ABC"});

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
            const auto value = bg::bundle<Instrument>(bg::symbol{"ABC"});

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
        const auto value = bg::bundle<FinalizableRecord>(field);
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
    const auto derived = bg::bundle<Equity>(bg::symbol{"ABC"}, 12.5, bg::i64{100});
    const auto base = bg::scalar_cast<bg::Bundle<Instrument>>(derived);

    CHECK(&base.type() == &derived.type());
    CHECK(base.field_count() == 2);
    CHECK(bg::scalar_cast<bg::symbol>(base[0]).value().text() == "ABC");
    CHECK(bg::scalar_cast<double>(base[1]).value() == 12.5);
}

TEST_CASE("composed hierarchical storage rejects another runtime layout",
          "[feature:scalar-storage][scalar:hierarchical][representation:covariant]") {
    const auto &runtime_type = static_cast<const bg::HierarchicalScalarRuntimeType &>(
        bg::cpp_scalar_runtime_type<bg::Bundle<Instrument>>());
    const auto alternate = AlternateHierarchicalRuntimeType{runtime_type.type()};

    CHECK_FALSE(runtime_type.accepts_input(alternate));
}

TEST_CASE("a hierarchical field preserves a derived value stored through its base type",
          "[value][feature:scalar-storage][scalar:hierarchical][schema:named][binding:covariant]"
          "[representation:covariant]") {
    const auto equity = bg::bundle<Equity>(bg::symbol{"ABC"}, 12.5, bg::i64{100});
    const auto position = bg::bundle<Position>(equity.view());

    const auto instrument = bg::scalar_cast<bg::Bundle<Instrument>>(position.view()[0]);
    CHECK(&instrument.type() == &equity.type());
    CHECK(bg::scalar_cast<bg::symbol>(instrument[0]).value().text() == "ABC");
}

TEST_CASE("hierarchical construction rejects unsupported covariant collection representations",
          "[feature:scalar-storage][scalar:hierarchical][scalar:list][scalar:dict][binding:covariant]") {
    const auto equity = bg::bundle<Equity>(bg::symbol{"ABC"}, 12.5, bg::i64{100});
    const auto list_items = std::array<bg::Scalar<bg::Bundle<Equity>>, 1>{equity.view()};
    const auto list = bg::ScalarValue<bg::List<bg::Bundle<Equity>>>{list_items};
    const auto dictionary_items =
        std::array{std::pair{bg::i64{1}, bg::Scalar<bg::Bundle<Equity>>::from(equity)}};
    const auto dictionary =
        bg::ScalarValue<bg::Dict<bg::i64, bg::Bundle<Equity>>>{dictionary_items};

    CHECK(bg::cpp_scalar_type<bg::List<bg::Bundle<Instrument>>>().accepts(list.type()));
    CHECK(bg::cpp_scalar_type<bg::Dict<bg::i64, bg::Bundle<Instrument>>>().accepts(dictionary.type()));
    CHECK_THROWS_AS(bg::bundle<InstrumentList>(list.view()), std::invalid_argument);
    CHECK_THROWS_AS(bg::bundle<InstrumentDictionary>(dictionary.view()), std::invalid_argument);
}

TEST_CASE("a recursive hierarchical scalar runtime type supports an absent recursive field",
          "[value][feature:scalar-storage][scalar:hierarchical][schema:named][validity:optional]") {
    const auto value = bg::bundle<RecursiveValue>(bg::i64{42});

    CHECK(bg::scalar_cast<bg::i64>(value.view()[0]).value() == 42);
    CHECK_FALSE(value.view().present(1));
}

TEST_CASE("a recursive hierarchical scalar stores descendants in a scalar list",
          "[value][feature:scalar-storage][scalar:hierarchical][scalar:list][schema:named]"
          "[validity:optional]") {
    const auto leaf = bg::bundle<RecursiveCollectionValue>(bg::i64{1});
    const auto children_source = std::array<bg::Scalar<bg::Bundle<RecursiveCollectionValue>>, 1>{leaf.view()};
    const auto children = bg::ScalarValue<bg::List<bg::Bundle<RecursiveCollectionValue>>>{children_source};
    const auto parent = bg::bundle<RecursiveCollectionValue>(bg::i64{2}, children);

    const auto stored_children = bg::scalar_cast<bg::List<bg::Bundle<RecursiveCollectionValue>>>(parent.view()[1]);

    REQUIRE(stored_children.size() == 1);
    CHECK(bg::scalar_cast<bg::i64>(stored_children[0][0]).value() == 1);
    CHECK_FALSE(stored_children[0].present(1));
}

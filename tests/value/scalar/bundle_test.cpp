#include "../../utils/counting_memory_resource.hpp"

#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/opaque.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace bg;

namespace {

template <template <typename, auto...> typename> struct EmptyRecord {};

auto bundle_allocator = allocator{};

} // namespace

TEST_CASE("bundle constructs an empty hierarchical scalar", "[feature:scalar-api][scalar:hierarchical][schema:named]") {
    const auto value = bundle<EmptyRecord>(bundle_allocator);

    CHECK(value.view().field_count() == 0);
}

namespace {

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
    Field<double, optional> price;
};

template <template <typename, auto...> typename Field> struct Equity : schema_extends<Instrument, Field> {
    Field<i64> lot_size;
};

template <template <typename, auto...> typename Field> struct UnrelatedRecord {
    Field<i64> value;
};

template <template <typename, auto...> typename Field> struct AnotherRecord {
    Field<i64> value;
};

template <template <typename, auto...> typename Field> struct ExtensionBackedRecord {
    Field<i64> value;
};

template <template <typename, auto...> typename Field> struct Position {
    Field<Bundle<Instrument>> instrument;
};

template <template <typename, auto...> typename Field> struct InstrumentList {
    Field<List<Bundle<Instrument>>> instruments;
};

template <template <typename, auto...> typename Field> struct InstrumentDictionary {
    Field<Dict<i64, Bundle<Instrument>>> instruments;
};

template <template <typename, auto...> typename Field> struct RecursiveValue {
    Field<i64> value;
    Field<Bundle<RecursiveValue>, optional> next;
};

template <template <typename, auto...> typename Field> struct RecursiveCollectionValue {
    Field<i64> value;
    Field<List<Bundle<RecursiveCollectionValue>>, optional> children;
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
    Field<Opaque<Finalizable>> value;
};

template <template <typename, auto...> typename Field> struct HashableRecord {
    Field<i64> value;
};

template <template <typename, auto...> typename Field> struct UnhashableRecord {
    Field<Opaque<Finalizable>> value;
};

template <typename T>
concept HashableScalar = requires(const Scalar<T> &value) { value.hash(); };

template <template <template <typename, auto...> typename> typename Schema>
concept PositionalBundleConstructible = requires(allocator &memory) { bundle<Schema>(memory, i64{1}); };

class AlternateHierarchicalRuntimeType final : public HierarchicalScalarRuntimeType {
  public:
    explicit AlternateHierarchicalRuntimeType(const ResolvedHierarchicalScalarType &type) noexcept
        : HierarchicalScalarRuntimeType{type, sizeof(void *), alignof(void *), false, false, true} {}

    void construct_fields_with(void *, allocator &, ScalarFieldConstruction) const override {
        throw std::logic_error{"alternate hierarchical runtime does not support field construction"};
    }

    details::ScalarData field(const details::ScalarData &, std::size_t) const override { std::terminate(); }

    bool field_present(const details::ScalarData &, std::size_t) const override { return false; }

  private:
    void construct_initial_value(void *destination) const noexcept override {
        *static_cast<void **>(destination) = nullptr;
    }

    void do_construct_from_input(void *, const details::ScalarData &, allocator &) const override { std::terminate(); }

    void copy_construct(void *, const void *, allocator &) const override { std::terminate(); }

    void do_assign_from_input(void *, const details::ScalarData &) const override { std::terminate(); }

    std::size_t hash(const void *) const noexcept override { return 0; }

    bool equals_same_type(const void *, const details::ScalarData &) const noexcept override { return false; }

    void destroy(void *, allocator &) const noexcept override {}
};

} // namespace

static_assert(sizeof(Scalar<Bundle<Instrument>>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<Bundle<Instrument>>) == 2 * sizeof(void *));
static_assert(sizeof(Bundle<Instrument>) == sizeof(void *));
static_assert(std::constructible_from<ScalarValue<Bundle<Instrument>>, Scalar<Bundle<Equity>>>);
static_assert(std::constructible_from<ScalarValue<Bundle<Instrument>>, const ScalarValue<Bundle<Equity>> &>);
static_assert(!std::constructible_from<ScalarValue<Bundle<Instrument>>, const ScalarValue<Bundle<Instrument>> &>);
static_assert(
    !std::constructible_from<ScalarValue<Set<Bundle<Instrument>>>, const std::array<Scalar<Bundle<Equity>>, 1> &>);
static_assert(HashableScalar<Bundle<HashableRecord>>);
static_assert(!HashableScalar<Bundle<UnhashableRecord>>);
static_assert(!PositionalBundleConstructible<SCHEMA>);

TEST_CASE("bundle construction outside an allocation context requires an allocator",
          "[feature:scalar-api][scalar:hierarchical][storage:variable]") {
    CHECK_THROWS_AS(bundle<EmptyRecord>(), std::logic_error);
}

TEST_CASE("bundle constructs a hierarchical scalar from positional and named arguments",
          "[feature:scalar-api][scalar:hierarchical][schema:named][validity:optional]") {
    using namespace bg::literals;

    const auto value = bundle<Instrument>(bundle_allocator, "price"_n = 12.5, "symbol"_n = symbol{"ABC"});
    const auto view = value.view();

    STATIC_CHECK(std::same_as<std::remove_cvref_t<decltype(value)>, ScalarValue<Bundle<Instrument>>>);
    CHECK(scalar_cast<symbol>(view[0]).value().text() == "ABC");
    CHECK(scalar_cast<double>(view[1]).value() == 12.5);
}

TEST_CASE("bundle uses inherited logical field order and permits omitted optional fields",
          "[feature:scalar-api][scalar:hierarchical][schema:named][binding:covariant][validity:optional]") {
    using namespace bg::literals;

    const auto value = bundle<Equity>(bundle_allocator, "symbol"_n = symbol{"ABC"}, "lot_size"_n = 100);
    const auto view = value.view();

    REQUIRE(view.field_count() == 3);
    CHECK(scalar_cast<symbol>(view[0]).value().text() == "ABC");
    CHECK_FALSE(view.present(1));
    CHECK(scalar_cast<i64>(view[2]).value() == 100);
}

TEST_CASE("bundle constructs nested and recursive hierarchical scalars",
          "[feature:scalar-api][scalar:hierarchical][schema:named][shape:nested][validity:optional]") {
    using namespace bg::literals;

    const auto instrument = bundle<Instrument>(bundle_allocator, symbol{"ABC"});
    const auto position = bundle<Position>(bundle_allocator, instrument.view());
    const auto leaf = bundle<RecursiveValue>(bundle_allocator, 1);
    const auto parent = bundle<RecursiveValue>(bundle_allocator, 2, "next"_n = leaf.view());

    const auto stored_instrument = scalar_cast<Bundle<Instrument>>(position.view()[0]);
    const auto stored_leaf = scalar_cast<Bundle<RecursiveValue>>(parent.view()[1]);
    CHECK(scalar_cast<symbol>(stored_instrument[0]).value().text() == "ABC");
    CHECK(scalar_cast<i64>(stored_leaf[0]).value() == 1);
    CHECK_FALSE(stored_leaf.present(1));
}

TEST_CASE("bundle rejects invalid field assignments",
          "[feature:scalar-api][scalar:hierarchical][schema:named][validity:required]") {
    using namespace bg::literals;

    const auto integer = ScalarValue{i64{1}};

    CHECK_THROWS_AS(bundle<Instrument>(bundle_allocator), std::invalid_argument);
    CHECK_THROWS_AS(bundle<Instrument>(bundle_allocator, "missing"_n = symbol{"ABC"}), std::invalid_argument);
    CHECK_THROWS_AS(bundle<Instrument>(bundle_allocator, "symbol"_n = symbol{"ABC"}, "symbol"_n = symbol{"DEF"}),
                    std::invalid_argument);
    CHECK_THROWS_AS(bundle<Instrument>(bundle_allocator, "price"_n = 12.5, symbol{"ABC"}), std::invalid_argument);
    CHECK_THROWS_AS(bundle<Instrument>(bundle_allocator, integer), std::invalid_argument);
}

TEST_CASE("bundle releases explicit allocator storage after failed construction",
          "[feature:scalar-storage][feature:finalization][scalar:hierarchical][storage:variable]") {
    auto memory = counting_memory_resource{};
    auto allocator = bg::allocator{memory};
    const auto incompatible = ScalarValue{i64{1}};

    CHECK_THROWS_AS(bundle<Instrument>(allocator, symbol{"ABC"}, incompatible), std::invalid_argument);
    CHECK(memory.allocations() == 0);
    CHECK(memory.allocated_bytes() == 0);
}

TEST_CASE("hierarchical scalar values expose required and optional fields",
          "[value][feature:scalar-api][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto value = bundle<Instrument>(bundle_allocator, symbol{"ABC"});
    const auto view = value.view();

    CHECK(view.field_count() == 2);
    CHECK(view.field_name(0) == "symbol");
    CHECK(view.present(0));
    CHECK_FALSE(view.present(1));
    CHECK(scalar_cast<symbol>(view[0]).value().text() == "ABC");
    CHECK_THROWS_AS(view[1], std::bad_optional_access);
}

TEST_CASE("copying a hierarchical scalar retains its shared value",
          "[value][feature:scalar-storage][scalar:hierarchical][storage:variable]") {
    const auto value = bundle<Instrument>(bundle_allocator, symbol{"ABC"}, 12.5);
    const auto copy = ScalarValue<Bundle<Instrument>>{value.view()};

    CHECK(copy == value);
}

TEST_CASE("hierarchical scalar equality and hashing include optional-field presence",
          "[value][feature:scalar-api][scalar:hierarchical][validity:optional]") {
    const auto left = bundle<Instrument>(bundle_allocator, symbol{"ABC"});
    const auto right = bundle<Instrument>(bundle_allocator, symbol{"ABC"});
    const auto present = bundle<Instrument>(bundle_allocator, symbol{"ABC"}, 0.0);

    CHECK(left.equals(right));
    CHECK(left.hash() == right.hash());
    CHECK_FALSE(left.equals(present));
}

TEST_CASE("a hierarchical scalar visitor exposes its structural view",
          "[value][feature:visitor][scalar:hierarchical][declaration:runtime]") {
    const auto value = bundle<Instrument>(bundle_allocator, symbol{"ABC"});
    auto called = false;
    auto erased_called = false;

    value.view().visit(
        [&](const Scalar<HIERARCHICAL> *view) {
            called = true;
            CHECK(&view->type() == &value.type());
            CHECK(view->field_count() == 2);
            CHECK(view->field_name(0) == "symbol");
            CHECK(view->present(0));
            CHECK_FALSE(view->present(1));
            CHECK(scalar_cast<symbol>((*view)[0]).value().text() == "ABC");
        },
        [&](const Scalar<> *) { erased_called = true; }, ddv::noop);

    CHECK(called);
    CHECK_FALSE(erased_called);
}

TEST_CASE("a scalar list stores hierarchical values and preserves their actual type",
          "[value][feature:scalar-storage][scalar:list][scalar:hierarchical][binding:covariant]") {
    const auto equity = bundle<Equity>(bundle_allocator, symbol{"ABC"}, 12.5, i64{100});
    const auto instrument = scalar_cast<Bundle<Instrument>>(equity);
    const auto items = std::array<Scalar<Bundle<Instrument>>, 1>{instrument};

    const auto list = ScalarValue<List<Bundle<Instrument>>>{items};
    const auto equivalent = ScalarValue<List<Bundle<Instrument>>>{items};
    const auto copy = ScalarValue<List<Bundle<Instrument>>>{list.view()};
    const auto stored = list.view()[0];
    const auto copied = copy.view()[0];

    CHECK(&stored.type() == &equity.type());
    CHECK(&copied.type() == &equity.type());
    CHECK(scalar_cast<symbol>(stored[0]).value().text() == "ABC");
    CHECK(scalar_cast<double>(stored[1]).value() == 12.5);
    CHECK(list.equals(equivalent));
    CHECK(list.hash() == equivalent.hash());
}

TEST_CASE("a scalar collection rejects an incompatible hierarchical item",
          "[value][feature:scalar-storage][scalar:list][scalar:hierarchical][declaration:concrete]") {
    const auto value = bundle<UnrelatedRecord>(bundle_allocator, i64{42});
    const auto items = std::array<Scalar<Bundle<UnrelatedRecord>>, 1>{value.view()};

    CHECK_THROWS_AS((ScalarValue<List<Bundle<Instrument>>>{items}), std::invalid_argument);
}

TEST_CASE("an owning base hierarchical scalar retains a derived value",
          "[value][feature:scalar-storage][scalar:hierarchical][binding:covariant]") {
    auto from_view = std::optional<ScalarValue<Bundle<Instrument>>>{};
    auto from_value = std::optional<ScalarValue<Bundle<Instrument>>>{};
    const auto *derived_type = static_cast<const ScalarType *>(nullptr);
    const auto *derived_runtime_type = static_cast<const ScalarRuntimeType *>(nullptr);
    {
        auto state = GlobalState{};
        state.write([&](auto &) {
            const auto equity = bundle<Equity>(symbol{"ABC"}, 12.5, i64{100});

            derived_type = &equity.type();
            derived_runtime_type = &equity.runtime_type();
            from_view.emplace(equity.view());
            from_value.emplace(equity);
        });
    }

    REQUIRE(from_view.has_value());
    REQUIRE(from_value.has_value());
    CHECK(&from_view->type() == derived_type);
    CHECK(&from_view->runtime_type() == derived_runtime_type);
    CHECK(&from_value->type() == derived_type);
    CHECK(&from_value->runtime_type() == derived_runtime_type);
    CHECK(scalar_cast<i64>(scalar_cast<Bundle<Equity>>(*from_view)[2]).value() == 100);
    CHECK(scalar_cast<i64>(scalar_cast<Bundle<Equity>>(*from_value)[2]).value() == 100);
}

TEST_CASE("an owning generic bundle scalar retains its actual runtime schema",
          "[value][feature:scalar-storage][scalar:hierarchical][declaration:generic]") {
    const auto equity = bundle<Equity>(bundle_allocator, symbol{"ABC"}, 12.5, i64{100});
    const auto erased = ScalarValue<Bundle<SCHEMA>>{equity.view()};

    CHECK(&erased.type() == &equity.type());
    CHECK(erased.view().field_count() == 3);
    CHECK(erased.view().field_name(2) == "lot_size");
    CHECK(scalar_cast<i64>(erased.view()[2]).value() == 100);
}

TEST_CASE("generic bundle declarations retain ownership through lists and dictionaries",
          "[value][feature:scalar-storage][scalar:hierarchical][scalar:list][scalar:dict]"
          "[declaration:generic]") {
    auto generic_list = std::optional<ScalarValue<List<Bundle<SCHEMA>>>>{};
    auto generic_dictionary = std::optional<ScalarValue<Dict<i64, Bundle<SCHEMA>>>>{};
    auto derived_type = static_cast<const ScalarType *>(nullptr);
    {
        using namespace bg::literals;

        const auto equity = bundle<Equity>(bundle_allocator, symbol{"ABC"}, "lot_size"_n = i64{100});
        derived_type = &equity.type();

        const auto list = ScalarValue<List<Bundle<Equity>>>{std::array<Scalar<Bundle<Equity>>, 1>{equity.view()}};
        const auto dictionary = ScalarValue<Dict<i64, Bundle<Equity>>>{std::array{std::pair{i64{1}, equity.view()}}};
        const auto source_list_view = list.view();
        const auto source_dictionary_view = dictionary.view();
        const auto list_view = Scalar<List<Bundle<SCHEMA>>>::from(source_list_view);
        const auto dictionary_view = Scalar<Dict<i64, Bundle<SCHEMA>>>::from(source_dictionary_view);

        generic_list.emplace(list_view);
        generic_dictionary.emplace(dictionary_view);
    }

    REQUIRE(generic_list.has_value());
    REQUIRE(generic_dictionary.has_value());
    REQUIRE(generic_list->view().size() == 1);
    CHECK(&generic_list->view()[0].type() == derived_type);
    CHECK(scalar_cast<i64>(generic_list->view()[0][2]).value() == 100);
    REQUIRE(generic_dictionary->view().size() == 1);
    CHECK(&generic_dictionary->view().at(1).type() == derived_type);
    CHECK(scalar_cast<i64>(generic_dictionary->view().at(1)[2]).value() == 100);
}

TEST_CASE("a runtime schema constructs a bundle with optional and nested fields",
          "[feature:scalar-api][scalar:hierarchical][scalar:list][schema:named][declaration:runtime]"
          "[validity:optional][shape:nested]") {
    const auto fields = std::array{
        SchemaField{util::symbol{"values"}, &cpp_scalar_type<List<i64>>(), false},
        SchemaField{util::symbol{"comment"}, &cpp_scalar_type<symbol>(), true},
    };
    const auto &schema = bg::schema(util::symbol{"RuntimeValues"}, new_runtime_schema_id(), nullptr, fields);
    auto result = std::optional<ScalarValue<Bundle<SCHEMA>>>{};

    {
        const auto items = std::array{ScalarValue<i64>{17}};
        const auto values = ScalarValue<List<i64>>{std::array<Scalar<i64>, 1>{items[0].view()}, bundle_allocator};
        const auto values_view = values.view();
        const auto inputs = std::array{
            std::optional<Scalar<>>{Scalar<>::from(values_view)},
            std::optional<Scalar<>>{},
        };

        result.emplace(bundle(bundle_allocator, schema, inputs));
    }

    REQUIRE(result.has_value());
    CHECK(result->view().field_count() == 2);
    CHECK(result->view().field_name(0) == "values");
    CHECK(result->view().present(0));
    CHECK_FALSE(result->view().present(1));
    const auto stored_values = scalar_cast<List<i64>>(result->view()[0]);
    REQUIRE(stored_values.size() == 1);
    CHECK(scalar_cast<i64>(stored_values[0]).value() == 17);
}

TEST_CASE("runtime bundle construction rejects invalid field counts values and schemas",
          "[feature:scalar-api][scalar:hierarchical][schema:named][declaration:runtime]"
          "[validity:required]") {
    const auto fields = std::array{
        SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>(), false},
        SchemaField{util::symbol{"comment"}, &cpp_scalar_type<symbol>(), true},
    };
    const auto &schema = bg::schema(util::symbol{"RuntimeRequired"}, new_runtime_schema_id(), nullptr, fields);
    const auto integer = ScalarValue<i64>{42};
    const auto text = ScalarValue<symbol>{symbol{"wrong type"}};
    const auto integer_view = integer.view();
    const auto text_view = text.view();
    const auto short_inputs = std::array{std::optional<Scalar<>>{Scalar<>::from(integer_view)}};
    const auto missing_required = std::array{
        std::optional<Scalar<>>{},
        std::optional<Scalar<>>{},
    };
    const auto wrong_representation = std::array{
        std::optional<Scalar<>>{Scalar<>::from(text_view)},
        std::optional<Scalar<>>{},
    };

    CHECK_THROWS_WITH(bundle(bundle_allocator, schema, short_inputs),
                      "hierarchical scalar field count does not match its type");
    CHECK_THROWS_WITH(bundle(bundle_allocator, schema, missing_required),
                      "required hierarchical scalar field is absent");
    CHECK_THROWS_WITH(bundle(bundle_allocator, schema, wrong_representation),
                      "hierarchical scalar field has an incompatible runtime representation");

    const auto unresolved_fields = std::array{
        SchemaField{util::symbol{"value"}, &cpp_scalar_type<SCALAR>(), false},
    };
    const auto &unresolved_schema =
        bg::schema(util::symbol{"RuntimeUnresolved"}, new_runtime_schema_id(), nullptr, unresolved_fields);
    CHECK_THROWS_WITH(hierarchical_scalar_type(unresolved_schema),
                      "a hierarchical scalar schema requires resolved scalar fields");
}

TEST_CASE("an owning hierarchical scalar rejects an unrelated schema",
          "[value][feature:scalar-storage][scalar:hierarchical][declaration:concrete]") {
    const auto value = bundle<UnrelatedRecord>(bundle_allocator, i64{42});

    CHECK_THROWS_AS((ScalarValue<Bundle<Instrument>>{value.view()}), std::invalid_argument);
    CHECK_THROWS_AS((ScalarValue<Bundle<Instrument>>{value}), std::invalid_argument);
}

TEST_CASE("a scalar dictionary stores hierarchical mapped values",
          "[value][feature:scalar-storage][scalar:dict][scalar:hierarchical][declaration:concrete]") {
    const auto instrument = bundle<Instrument>(bundle_allocator, symbol{"ABC"});
    const auto items = std::array<std::pair<i64, Scalar<Bundle<Instrument>>>, 1>{std::pair{i64{1}, instrument.view()}};

    const auto dictionary = ScalarValue<Dict<i64, Bundle<Instrument>>>{items};
    const auto stored = dictionary.view().at(1);

    CHECK(scalar_cast<symbol>(stored[0]).value().text() == "ABC");
    CHECK_FALSE(stored.present(1));
}

TEST_CASE("a collection retains hierarchical fields until its final release",
          "[value][feature:finalization][scalar:list][scalar:hierarchical][storage:variable]"
          "[transition:lifecycle]") {
    auto memory = counting_memory_resource{};
    auto allocator = bg::allocator{memory};
    auto destruction_count = 0;
    auto retained = std::optional<ScalarValue<List<Bundle<FinalizableRecord>>>>{};
    {
        const auto source = Finalizable{destruction_count};
        const auto field = ScalarValue<Opaque<Finalizable>>{source, allocator};
        const auto value = bundle<FinalizableRecord>(allocator, field);
        const auto items = std::array<Scalar<Bundle<FinalizableRecord>>, 1>{value.view()};

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
        const auto value = bundle<Instrument>(allocator, symbol{"ABC"});

        CHECK(value.view().present(0));
        CHECK(memory.allocations() != 0);
    }

    CHECK(memory.allocations() == 0);
    CHECK(memory.allocated_bytes() == 0);
}

TEST_CASE("a hierarchical scalar survives its global state",
          "[value][feature:global-state][scalar:hierarchical][storage:variable]") {
    auto retained = std::optional<ScalarValue<Bundle<Instrument>>>{};
    {
        auto state = GlobalState{};
        state.write([&](auto &values) {
            const auto value = bundle<Instrument>(symbol{"ABC"});

            values.set(util::symbol{"instrument"}, value);
            retained.emplace(value.view());
        });
    }

    REQUIRE(retained.has_value());
    CHECK(scalar_cast<symbol>(retained->view()[0]).value().text() == "ABC");
}

TEST_CASE("a hierarchical scalar survives its runtime graph",
          "[value][feature:lifecycle][scalar:hierarchical][storage:variable]"
          "[transition:lifecycle]") {
    auto wiring = GraphWiring{};
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto retained = std::optional<ScalarValue<Bundle<Instrument>>>{};
    {
        auto runtime_global_state = GlobalState{};
        auto runtime = RuntimeGraph{compiled, runtime_global_state};
        runtime.global_state().write([&](auto &values) {
            const auto value = bundle<Instrument>(symbol{"ABC"});

            values.set(util::symbol{"instrument"}, value);
            retained.emplace(value.view());
        });
    }

    REQUIRE(retained.has_value());
    CHECK(scalar_cast<symbol>(retained->view()[0]).value().text() == "ABC");
}

TEST_CASE("a hierarchical scalar destroys its fields after its final release",
          "[value][feature:finalization][scalar:hierarchical][storage:variable]"
          "[transition:lifecycle]") {
    auto destruction_count = 0;
    const auto source = Finalizable{destruction_count};
    const auto field = ScalarValue<Opaque<Finalizable>>{source};
    {
        const auto value = bundle<FinalizableRecord>(bundle_allocator, field);
        {
            const auto copy = ScalarValue<Bundle<FinalizableRecord>>{value.view()};
            CHECK(copy.view().present(0));
        }

        CHECK(destruction_count == 0);
    }

    CHECK(destruction_count == 1);
}

TEST_CASE("a base hierarchical view preserves the derived runtime type",
          "[value][feature:scalar-api][scalar:hierarchical][schema:named][binding:covariant]"
          "[representation:covariant]") {
    const auto derived = bundle<Equity>(bundle_allocator, symbol{"ABC"}, 12.5, i64{100});
    const auto base = scalar_cast<Bundle<Instrument>>(derived);

    CHECK(&base.type() == &derived.type());
    CHECK(base.field_count() == 3);
    CHECK(base.field_name(2) == "lot_size");
    CHECK(&base.field_type(2) == &cpp_scalar_type<i64>());
    CHECK(scalar_cast<symbol>(base[0]).value().text() == "ABC");
    CHECK(scalar_cast<double>(base[1]).value() == 12.5);
    CHECK(scalar_cast<i64>(base[2]).value() == 100);
}

TEST_CASE("hierarchical equality compares compatible facades using the actual value type",
          "[value][feature:scalar-api][scalar:hierarchical][binding:covariant]") {
    const auto derived = bundle<Equity>(bundle_allocator, symbol{"ABC"}, 12.5, i64{100});
    const auto base = scalar_cast<Bundle<Instrument>>(derived);

    CHECK(base == derived);
    CHECK(derived == base);
}

TEST_CASE("hierarchical hashing includes the actual value type",
          "[value][feature:scalar-api][scalar:hierarchical][declaration:runtime]") {
    const auto first = bundle<UnrelatedRecord>(bundle_allocator, i64{42});
    const auto second = bundle<AnotherRecord>(bundle_allocator, i64{42});

    CHECK(first.hash() != second.hash());
}

TEST_CASE("composed hierarchical storage rejects another runtime layout",
          "[feature:scalar-storage][scalar:hierarchical][representation:covariant]") {
    const auto &runtime_type =
        static_cast<const HierarchicalScalarRuntimeType &>(cpp_scalar_runtime_type<Bundle<Instrument>>());
    const auto alternate = AlternateHierarchicalRuntimeType{runtime_type.type()};

    CHECK_FALSE(runtime_type.accepts_input(alternate));
}

TEST_CASE("bundle construction uses composed storage when an extension runtime type is registered",
          "[feature:scalar-storage][scalar:hierarchical]") {
    const auto &type =
        static_cast<const ResolvedHierarchicalScalarType &>(cpp_scalar_type<Bundle<ExtensionBackedRecord>>());
    static const auto alternate = AlternateHierarchicalRuntimeType{type};
    register_scalar_runtime_type(alternate);

    const auto value = bundle<ExtensionBackedRecord>(bundle_allocator, i64{42});

    CHECK(&value.runtime_type() != &alternate);
    CHECK(scalar_cast<i64>(value.view()[0]).value() == 42);
}

TEST_CASE("a hierarchical field preserves a derived value stored through its base type",
          "[value][feature:scalar-storage][scalar:hierarchical][schema:named][binding:covariant]"
          "[representation:covariant]") {
    const auto equity = bundle<Equity>(bundle_allocator, symbol{"ABC"}, 12.5, i64{100});
    const auto position = bundle<Position>(bundle_allocator, equity.view());

    const auto instrument = scalar_cast<Bundle<Instrument>>(position.view()[0]);
    CHECK(&instrument.type() == &equity.type());
    CHECK(scalar_cast<symbol>(instrument[0]).value().text() == "ABC");
}

TEST_CASE("hierarchical construction converts covariant collection representations",
          "[feature:scalar-storage][scalar:hierarchical][scalar:list][scalar:dict][binding:covariant]") {
    const auto equity = bundle<Equity>(bundle_allocator, symbol{"ABC"}, 12.5, i64{100});
    const auto list_items = std::array<Scalar<Bundle<Equity>>, 1>{equity.view()};
    const auto list = ScalarValue<List<Bundle<Equity>>>{list_items};
    const auto dictionary_items = std::array{std::pair{i64{1}, Scalar<Bundle<Equity>>::from(equity)}};
    const auto dictionary = ScalarValue<Dict<i64, Bundle<Equity>>>{dictionary_items};
    const auto &list_runtime = cpp_scalar_runtime_type<List<Bundle<Instrument>>>();
    const auto &dictionary_runtime = cpp_scalar_runtime_type<Dict<i64, Bundle<Instrument>>>();

    CHECK(cpp_scalar_type<List<Bundle<Instrument>>>().accepts(list.type()));
    CHECK(cpp_scalar_type<Dict<i64, Bundle<Instrument>>>().accepts(dictionary.type()));
    CHECK(list_runtime.accepts_input(list.runtime_type()));
    CHECK(dictionary_runtime.accepts_input(dictionary.runtime_type()));
    CHECK(list_runtime.accepts_input(list_runtime));
    CHECK(dictionary_runtime.accepts_input(dictionary_runtime));
    CHECK_FALSE(list_runtime.accepts_input(cpp_scalar_runtime_type<List<Bundle<UnrelatedRecord>>>()));
    CHECK_FALSE(dictionary_runtime.accepts_input(cpp_scalar_runtime_type<Dict<symbol, Bundle<Equity>>>()));
    CHECK_FALSE(dictionary_runtime.accepts_input(cpp_scalar_runtime_type<Dict<i64, Bundle<UnrelatedRecord>>>()));

    const auto list_container = bundle<InstrumentList>(bundle_allocator, list.view());
    const auto stored_list = scalar_cast<List<Bundle<Instrument>>>(list_container.view()[0]);
    REQUIRE(stored_list.size() == 1);
    CHECK(&stored_list[0].type() == &equity.type());
    CHECK(scalar_cast<symbol>(stored_list[0][0]).value().text() == "ABC");

    const auto dictionary_container = bundle<InstrumentDictionary>(bundle_allocator, dictionary.view());
    const auto stored_dictionary = scalar_cast<Dict<i64, Bundle<Instrument>>>(dictionary_container.view()[0]);
    REQUIRE(stored_dictionary.size() == 1);
    CHECK(&stored_dictionary.at(1).type() == &equity.type());
    const auto stored_equity = scalar_cast<Bundle<Equity>>(stored_dictionary.at(1));
    CHECK(scalar_cast<i64>(stored_equity[2]).value() == 100);
}

TEST_CASE("set runtimes reject covariant hierarchical items",
          "[feature:scalar-storage][scalar:set][scalar:hierarchical][binding:covariant]") {
    const auto &destination = cpp_scalar_runtime_type<Set<Bundle<Instrument>>>();
    const auto &source = cpp_scalar_runtime_type<Set<Bundle<Equity>>>();

    CHECK_FALSE(destination.accepts_input(source));
}

TEST_CASE("a recursive hierarchical scalar runtime type supports an absent recursive field",
          "[value][feature:scalar-storage][scalar:hierarchical][schema:named][validity:optional]") {
    const auto value = bundle<RecursiveValue>(bundle_allocator, i64{42});

    CHECK(scalar_cast<i64>(value.view()[0]).value() == 42);
    CHECK_FALSE(value.view().present(1));
}

TEST_CASE("a recursive hierarchical scalar stores descendants in a scalar list",
          "[value][feature:scalar-storage][scalar:hierarchical][scalar:list][schema:named]"
          "[validity:optional]") {
    const auto leaf = bundle<RecursiveCollectionValue>(bundle_allocator, i64{1});
    const auto children_source = std::array<Scalar<Bundle<RecursiveCollectionValue>>, 1>{leaf.view()};
    const auto children = ScalarValue<List<Bundle<RecursiveCollectionValue>>>{children_source};
    const auto parent = bundle<RecursiveCollectionValue>(bundle_allocator, i64{2}, children);

    const auto stored_children = scalar_cast<List<Bundle<RecursiveCollectionValue>>>(parent.view()[1]);

    REQUIRE(stored_children.size() == 1);
    CHECK(scalar_cast<i64>(stored_children[0][0]).value() == 1);
    CHECK_FALSE(stored_children[0].present(1));
}

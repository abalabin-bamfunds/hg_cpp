#include <bg/frame/frame.hpp>
#include <bg/frame/expression.hpp>
#include <bg/frame/literal.hpp>
#include <bg/bg.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/graph/runtime_graph_input.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/type/type_registry.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

template <template <typename, auto...> typename Field> struct Quotes {
    Field<bg::i64> size;
    Field<double> bid;
    Field<double> ask;
};

template <template <typename, auto...> typename Field> struct KeyedQuotes {
    Field<i64> id;
    Field<double> price;

    static bg::SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field> struct CompositeKeyRows {
    Field<i64> id;
    Field<i64> sequence;
    Field<double> price;
    Field<i64> value;

    static bg::SchemaMetadata metadata() { return key_metadata<"id", "sequence">(); }
};

template <template <typename, auto...> typename Field> struct OtherKeyedQuotes {
    Field<i64> id;
    Field<i64> other_id;
    Field<double> price;

    static bg::SchemaMetadata metadata() { return key_metadata<"other_id">(); }
};

template <template <typename, auto...> typename Field> struct KeyedQuotesBase {
    Field<i64> id;
    Field<double> price;

    static bg::SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field>
struct KeyedQuotesDerived : bg::schema_extends<KeyedQuotesBase, Field> {
    Field<double> bid;
};

template <template <typename, auto...> typename Field> struct UnkeyedQuotesBase {
    Field<i64> id;
    Field<double> price;
};

template <template <typename, auto...> typename Field>
struct KeyedQuotesDerivedFromUnkeyed : bg::schema_extends<UnkeyedQuotesBase, Field> {
    Field<double> bid;

    static bg::SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <template <typename, auto...> typename Field>
struct ChangedKeyQuotes : bg::schema_extends<KeyedQuotesBase, Field> {
    Field<i64> other_id;
    Field<double> bid;

    static bg::SchemaMetadata metadata() { return key_metadata<"other_id">(); }
};

template <template <typename, auto...> typename Field> struct DoubleKeyQuotes {
    Field<i64> id;
    Field<double> price;

    static bg::SchemaMetadata metadata() { return key_metadata<"price">(); }
};

template <template <typename, auto...> typename Field> struct ReorderedQuotes {
    Field<double> ask;
    Field<i64> size;
    Field<double> bid;
};

template <template <typename, auto...> typename Field> struct ComputedQuote {
    Field<double> mid;
    Field<double> ask;
    Field<double> bid;
};

template <template <typename, auto...> typename Field> struct GenericQuote {
    Field<bg::SCALAR> bid;
};
template <template <typename, auto...> typename Field> struct MixedGenericQuote {
    Field<i64> size;
    Field<SCALAR> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct WrongMixedGenericQuote {
    Field<double> size;
    Field<SCALAR> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct RepeatedGenericQuote {
    Field<SCALAR> size;
    Field<SCALAR> bid;
    Field<SCALAR> ask;
};
using I64Cell = ScalarVariable<"I64_CELL", i64>;
template <template <typename, auto...> typename Field> struct RepeatedConstrainedGenericQuote {
    Field<SCALAR> size;
    Field<SCALAR> bid;
    Field<I64Cell> ask;
};
template <template <typename, auto...> typename Field> struct ConstrainedGenericQuote {
    Field<i64> size;
    Field<I64Cell> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct InheritedGenericQuoteBase {
    Field<i64> size;
    Field<double> bid;
};
template <template <typename, auto...> typename Field>
struct InheritedGenericQuote : schema_extends<InheritedGenericQuoteBase, Field> {
    Field<SCALAR> bid;
    Field<double> ask;
};
template <template <typename, auto...> typename Field> struct InheritedFrameBase {
    Field<bg::i64> size;
    Field<double> bid;
};

template <template <typename, auto...> typename Field>
struct InheritedFrame : bg::schema_extends<InheritedFrameBase, Field> {
    Field<double> bid;
    Field<double> ask;
};

template <typename T> concept complete_type = requires { sizeof(T); };
template <typename Output, typename Argument>
concept accepts_frame_add = requires(const Output &output, const Argument &argument) { output.add(argument); };
template <typename Output, typename Argument>
concept accepts_frame_set = requires(const Output &output, const Argument &argument) { output.set(row_id_t{}, argument); };

static_assert(complete_type<Out<TSF<Quotes>>>);
static_assert(complete_type<Out<TSF<Quotes>, bg::frame::view>>);
static_assert(complete_type<Out<TSF<ComputedQuote>, bg::frame::computed<"mid">>>);
static_assert(complete_type<Out<TSC<bg::i64>>>);
static_assert(complete_type<Out<TSC<bg::i64>, bg::frame::view>>);
static_assert(!complete_type<Out<TS<bg::i64>, bg::frame::view>>);
static_assert(accepts_frame_add<Out<TSF<Quotes>>, RowPatch<Quotes>>);
static_assert(!accepts_frame_add<Out<TSF<SCHEMA>>, RowPatch<SCHEMA>>);
static_assert(accepts_frame_add<Out<TSF<GenericQuote>>, Row<GenericQuote>>);
static_assert(accepts_frame_add<Out<TSF<GenericQuote>>, RowPatch<GenericQuote>>);
static_assert(accepts_frame_set<Out<TSF<GenericQuote>>, RowPatch<GenericQuote>>);
static_assert(accepts_frame_add<Out<TSF<SCHEMA>>, Row<MixedGenericQuote>>);
static_assert(accepts_frame_add<Out<TSF<SCHEMA>>, RowPatch<MixedGenericQuote>>);
static_assert(accepts_frame_set<Out<TSF<SCHEMA>>, RowPatch<MixedGenericQuote>>);

template <template <typename, auto...> typename Field> struct EmptySchema {};
template <template <typename, auto...> typename Field> struct BidOnly { Field<double> price; };
template <template <typename, auto...> typename Field> struct UnsupportedFrameCell { Field<bool> value; };

static_assert(!scalar_declaration_traits<Column<bool>>::supports_time_series);
static_assert(!time_series_type_traits<TSC<bool>>::supported);
static_assert(!time_series_type_traits<TSF<UnsupportedFrameCell>>::supported);

class FailingResource final : public std::pmr::memory_resource {
  public:
    std::size_t attempts{};
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t fail_at{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (++attempts == fail_at)
            throw std::bad_alloc{};
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error{message};
}

void test_scalar_literals_and_content_equality() {
    auto resource = std::pmr::monotonic_buffer_resource{};
    auto memory = allocator{resource};

    auto sizes = ScalarValue<Column<i64>>::from_literal({i64{7}, NULL_VALUE, i64{9}}, memory);
    const auto sizes_view = sizes.view().value();
    require(sizes_view.size() == 3 && sizes_view.cell(0) == 7 && !sizes_view.cell(1) && sizes_view.cell(2) == 9,
            "column literal did not preserve values and nulls");
    auto other_sizes = ScalarValue<Column<i64>>::from_literal({i64{7}, NULL_VALUE, i64{9}}, memory);
    require(sizes.equals(other_sizes.view()), "equal column literals compared unequal");

    const auto distinct_null = literal::Sentinel{symbol{"frame.null"}};
    auto rejected_distinct_sentinel = false;
    try {
        static_cast<void>(ScalarValue<Column<i64>>::from_literal({distinct_null}, memory));
    } catch (const std::invalid_argument &) {
        rejected_distinct_sentinel = true;
    }
    require(rejected_distinct_sentinel, "column literal treated a same-name sentinel as the frame null singleton");
    auto frame = ScalarValue<Frame<Quotes>>::from_literal(
        {{"ask", {13.5, 14.5}}, {symbol{"size"}, {i64{7}, NULL_VALUE}}, {"bid", {12.5, NULL_VALUE}}},
        memory);
    const auto frame_view = frame.view().value();
    require(frame_view.size() == 2 && frame_view.columns().size.cell(1) == std::nullopt &&
                frame_view.columns().bid.cell(0) == 12.5 && frame_view.columns().ask.cell(1) == 14.5,
            "frame literal did not construct schema-ordered nullable columns");

    auto same_frame = ScalarValue<Frame<Quotes>>{frame.view(), memory};
    require(frame.equals(same_frame.view()), "equal frame values compared unequal");
    const auto shared_source = Scalar<Frame<Quotes>>::from(frame.runtime_type(), frame.view().data());
    auto shared_frame = ScalarValue<Frame<Quotes>>{shared_source};
    require(bg::details::scalar_data(shared_frame).mode() == ScalarDataMode::shared_value,
            "frame scalar copy did not retain shared storage");
    require(frame.equals(shared_frame.view()),
            "frame equality failed for a shared scalar value");

    const auto &empty_schema = static_cast<const Schema &>(frame_schema_type<EmptySchema>());
    const auto no_columns = std::span<const ColumnView<>>{};
    auto empty_two_rows = make_frame_value(memory, empty_schema, 2, no_columns);
    auto empty_three_rows = make_frame_value(memory, empty_schema, 3, no_columns);
    require(!empty_two_rows.equals(empty_three_rows.view()),
            "zero-column frame equality ignored the row count");

    auto nan_left = ScalarValue<Column<double>>::from_literal({std::numeric_limits<double>::quiet_NaN()}, memory);
    auto nan_right = ScalarValue<Column<double>>::from_literal({std::numeric_limits<double>::quiet_NaN()}, memory);
    require(!nan_left.equals(nan_right.view()) && !nan_left.equals(nan_left.view()),
            "column equality treated NaN cells as equal");

    const auto empty_literal = literal::ScalarLiteral{};
    auto empty_column = ScalarValue<Column<i64>>::from_literal(empty_literal, memory);
    auto empty_frame = ScalarValue<Frame<EmptySchema>>::from_literal(empty_literal, memory);
    require(empty_column.view().value().size() == 0 && empty_frame.view().value().size() == 0 &&
                empty_frame.view().value().schema().field_count() == 0,
            "empty column or frame literal did not construct an empty value");

    auto different_frame = ScalarValue<Frame<Quotes>>::from_literal(
        {{"size", {i64{7}, NULL_VALUE}}, {"bid", {12.5, NULL_VALUE}}, {"ask", {13.5, 99.5}}}, memory);
    require(!frame.equals(different_frame.view()) && !different_frame.equals(frame.view()),
            "frame equality treated different column values as equal");
}

void test_runtime_schema_and_malformed_frame_literals() {
    const auto fields = std::array{
        SchemaField{util::symbol{"number"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
    };
    const auto &schema = anonymous_schema(fields);
    const auto &runtime = frame_snapshot_runtime_type(frame_type(schema));
    auto memory_resource = FailingResource{};
    auto memory = allocator{memory_resource};

    const auto base_fields = std::array{SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
                                        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &keyed_base = bg::schema(util::symbol{"RuntimeKeyBase"}, bg::new_runtime_schema_id(), nullptr,
                                        base_fields, key_metadata<"id">());
    const auto mid_fields = std::array{SchemaField{util::symbol{"other_id"}, &cpp_scalar_type<i64>()},
                                     SchemaField{util::symbol{"bid"}, &cpp_scalar_type<double>()}};
    const auto &changed_key_mid = bg::schema(util::symbol{"RuntimeChangedKeyMid"}, bg::new_runtime_schema_id(),
                                             std::addressof(keyed_base), mid_fields, key_metadata<"other_id">());
    const auto leaf_fields = std::array{SchemaField{util::symbol{"ask"}, &cpp_scalar_type<double>()}};
    const auto &keyed_leaf = bg::schema(util::symbol{"RuntimeKeyLeaf"}, bg::new_runtime_schema_id(),
                                        std::addressof(changed_key_mid), leaf_fields);
    auto inherited_key_change_rejected = false;
    try {
        static_cast<void>(frame_type(keyed_leaf));
    } catch (const std::invalid_argument &) {
        inherited_key_change_rejected = true;
    }
    require(inherited_key_change_rejected,
            "runtime frame schema chain hid an inherited key change behind an intermediate schema");

    {
        auto value = ScalarValue<>::from_literal(runtime, {{"price", {12.5, 13.5}}, {"number", {i64{7}, NULL_VALUE}}},
                                                 memory);
        const auto erased_scalar = value.view();
        const auto frame = Scalar<Frame<SCHEMA>>::from(erased_scalar).value();
        require(frame.size() == 2 && frame.schema().field_count() == 2 && frame.column("number").cell<i64>(0) == 7 &&
                    !frame.column("number").cell<i64>(1) && frame.column("price").cell<double>(1) == 13.5,
                "runtime-schema frame literal lost its schema, values, or nulls");
    }

    const auto rejects = [&](const literal::ScalarLiteral &literal) {
        try {
            static_cast<void>(ScalarValue<Frame<Quotes>>::from_literal(literal, memory));
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    require(rejects({{"size"}}), "frame literal accepted an entry without a column");
    require(rejects({{"unknown", {i64{1}}}, {"bid", {12.5}}, {"ask", {13.5}}}),
            "frame literal accepted an unknown column");
    require(rejects({{"size", {i64{1}}}, {"bid", {12.5}}}),
            "frame literal accepted a missing column");
    require(rejects({{"size", {i64{1}}}, {"bid", {12.5}}, {"bid", {13.5}}, {"ask", {14.5}}}),
            "frame literal accepted a duplicate column");
    require(rejects({{"size", {12.5}}, {"bid", {12.5}}, {"ask", {13.5}}}),
            "frame literal accepted a cell with the wrong type");
    require(rejects({{"size", {i64{1}, i64{2}}}, {"bid", {12.5}}, {"ask", {13.5, 14.5}}}),
            "frame literal accepted columns with different lengths");

    auto malformed_resource = FailingResource{};
    auto malformed_memory = allocator{malformed_resource};
    auto malformed_rejected = false;
    try {
        static_cast<void>(ScalarValue<Frame<Quotes>>::from_literal(
            {{"size", {i64{1}, i64{2}}}, {"bid", {12.5}}, {"ask", {13.5, 14.5}}}, malformed_memory));
    } catch (const std::invalid_argument &) {
        malformed_rejected = true;
    }
    require(malformed_rejected && malformed_resource.allocations == malformed_resource.deallocations,
            "malformed frame literal leaked column allocations");
    require(memory_resource.allocations == memory_resource.deallocations,
            "runtime-schema frame literal leaked allocations");

    auto allocation_failures = std::size_t{};
    auto succeeded = false;
    for (auto failure = std::size_t{1};; ++failure) {
        auto attempt_resource = FailingResource{};
        auto attempt_memory = allocator{attempt_resource};
        attempt_resource.fail_at = failure;
        try {
            auto frame = ScalarValue<Frame<Quotes>>::from_literal(
                {{"size", {i64{1}, i64{2}}}, {"bid", {12.5, 13.5}}, {"ask", {14.5, 15.5}}}, attempt_memory);
            require(frame.view().value().size() == 2, "frame literal changed values during allocation retry");
            succeeded = true;
        } catch (const std::bad_alloc &) {
            ++allocation_failures;
        }
        require(attempt_resource.allocations == attempt_resource.deallocations,
                "frame literal leaked allocations during failure or success");
        if (succeeded)
            break;
    }
    require(allocation_failures != 0, "frame literal allocation retry did not exercise rollback");
}

void test_frame_literals_as_ordinary_time_series_inputs() {
    using namespace bg::literals;

    const auto copy_column = node([](In<TS<Column<i64>>> input, Out<TS<Column<i64>>> output) {
        output.set_value(input.value_view());
    });
    const auto column_result = testing::test_graph(
        copy_column, testing::input<TS<Column<i64>>>({{i64{1}, i64{2}}, {i64{3}, NULL_VALUE}}));
    auto first_column = ScalarValue<Column<i64>>::from_literal({i64{1}, i64{2}});
    auto second_column = ScalarValue<Column<i64>>::from_literal({i64{3}, NULL_VALUE});
    require(column_result.equals<Column<i64>>({first_column.view().value(), second_column.view().value()}),
            "ordinary TS<Column> literal inputs produced incorrect results");
    require(column_result.equals({{i64{1}, i64{2}}, {i64{3}, NULL_VALUE}}),
            "ordinary TS<Column> literal result comparison failed");

    const auto copy_frame = node([](In<TS<Frame<Quotes>>> input, Out<TS<Frame<Quotes>>> output) {
        output.set_value(input.value_view());
    });
    const auto frame_result = testing::test_graph(
        copy_frame,
        testing::input<TS<Frame<Quotes>>>({{{"size", {i64{1}}}, {"bid", {10.0}}, {"ask", {12.0}}},
                                           {{"ask", {20.0}}, {"size", {i64{2}}}, {"bid", {20.0}}}}));
    auto first_frame = ScalarValue<Frame<Quotes>>::from_literal(
        {{"size", {i64{1}}}, {"bid", {10.0}}, {"ask", {12.0}}});
    auto second_frame = ScalarValue<Frame<Quotes>>::from_literal(
        {{"ask", {20.0}}, {"size", {i64{2}}}, {"bid", {20.0}}});
    require(frame_result.equals<Frame<Quotes>>({first_frame.view().value(), second_frame.view().value()}),
            "ordinary TS<Frame> literal inputs produced incorrect results");
    require(frame_result.equals({{{"size", {i64{1}}}, {"bid", {10.0}}, {"ask", {12.0}}},
                                 {{"ask", {20.0}}, {"size", {i64{2}}}, {"bid", {20.0}}}}),
            "ordinary TS<Frame> literal result comparison failed");
}

void test_base_and_derived_frame_equality() {
    using namespace bg::frame;

    auto memory = allocator{};
    const auto make_derived = [&](double ask) {
        auto frame_value = Frame<InheritedFrame>{InheritedFrame<Column>{
            InheritedFrameBase<Column>{Column<i64>{{i64{5}}}, Column<double>{{12.5}}}, Column<double>{{13.5}},
            Column<double>{{ask}}}};
        auto view = FrameView<InheritedFrame>{frame_value};
        const auto scalar = Scalar<Frame<InheritedFrame>>::from(
            frame::details::frame_view_scalar_runtime_type<InheritedFrame>(), std::addressof(view));
        return ScalarValue<Frame<InheritedFrame>>{scalar, memory};
    };

    auto derived = make_derived(21.5);
    auto equal_derived = make_derived(21.5);
    auto changed_derived = make_derived(22.5);
    const auto derived_scalar = derived.view();
    const auto equal_derived_scalar = equal_derived.view();
    const auto changed_derived_scalar = changed_derived.view();
    const auto base_scalar = Scalar<Frame<InheritedFrameBase>>::from(derived_scalar);
    const auto equal_base_scalar = Scalar<Frame<InheritedFrameBase>>::from(equal_derived_scalar);
    const auto changed_base_scalar = Scalar<Frame<InheritedFrameBase>>::from(changed_derived_scalar);
    require(base_scalar.value().schema().field_count() == 3 && derived.equals(base_scalar) &&
                base_scalar.equals(derived.view()) && equal_derived.equals(base_scalar) &&
                base_scalar.equals(equal_derived.view()),
            "base and derived frame runtimes compared equal values as different");
    require(!derived.equals(changed_base_scalar) && !changed_base_scalar.equals(derived.view()),
            "base and derived frame equality ignored a derived-only field change");
    auto copied_base = ScalarValue<Frame<InheritedFrameBase>>{base_scalar, memory};
    const auto copied_base_scalar = copied_base.view();
    const auto copied_base_view = Scalar<Frame<InheritedFrameBase>>::from(copied_base_scalar).value();
    require(copied_base_view.schema().field_count() == 3 && derived.equals(copied_base_scalar) &&
                copied_base_scalar.equals(derived.view()),
            "owning copy through a base frame view lost the actual derived schema");

    auto base_frame = Frame<InheritedFrameBase>{InheritedFrameBase<Column>{
        Column<i64>{{i64{5}}}, Column<double>{{12.5}}}};
    auto base_frame_view = FrameView<InheritedFrameBase>{base_frame};
    auto base_value = ScalarValue<Frame<InheritedFrameBase>>{base_frame_view, memory};
    require(!derived.equals(base_value.view()) && !base_value.equals(derived.view()),
            "frame equality ignored a different actual schema");
}

template <typename Function>
void with_native_storage(const SchemaType &schema, allocator memory, Function &&function) {
    const auto &layout = bg::frame::details::native_frame_layout(schema);
    auto *allocation = ::operator new(layout.size, std::align_val_t{layout.alignment});
    auto *storage = std::construct_at(static_cast<NativeFrameStorage *>(allocation));
    storage->initialize(layout);
    auto release = util::scope_exit{[&]() noexcept {
        storage->release(memory);
        std::destroy_at(storage);
        ::operator delete(allocation, std::align_val_t{layout.alignment});
    }};
    std::invoke(std::forward<Function>(function), *storage);
}

template <template <template <typename, auto...> typename> typename Schema, typename Function>
void with_runtime_view_storage(allocator memory, Function &&function) {
    const auto &type = frame_time_series_type<Schema>(TimeSeriesGuarantee::ordinary);
    const auto &runtime = static_cast<const FrameSeriesRuntimeType &>(
        frame_view_time_series_runtime_type<Schema>(type));
    auto *allocation = ::operator new(runtime.value_size(), std::align_val_t{runtime.value_alignment()});
    runtime.construct_initial_value(allocation);
    auto release = util::scope_exit{[&]() noexcept {
        runtime.destroy_value(allocation, memory);
        ::operator delete(allocation, std::align_val_t{runtime.value_alignment()});
    }};
    std::invoke(std::forward<Function>(function), *static_cast<RuntimeFrameViewOutputStorage *>(allocation),
                runtime);
}

template <typename Predicate>
auto runtime_predicate(Predicate &predicate) {
    return std::pair{
        static_cast<void *>(std::addressof(predicate)),
        static_cast<RuntimeFrameViewOutputStorage::predicate_function>([](void *context, row_id_t id) {
            return std::invoke(*static_cast<Predicate *>(context), id);
        })};
}

struct FrameReadBindings {
    FrameReadStorage source;
    RowSet visible_rows{};
    bool override_visible_rows{};
    std::optional<std::size_t> overridden_index;
    FrameColumnReadBinding overridden_binding;

    static FrameColumnReadBinding column(const void *context, std::size_t index) {
        const auto &bindings = *static_cast<const FrameReadBindings *>(context);
        if (bindings.overridden_index == index)
            return bindings.overridden_binding;
        auto result = bindings.source.column(index);
        if (bindings.override_visible_rows) {
            result.visible_rows = bindings.visible_rows;
            result.coverage = bindings.visible_rows;
        }
        return result;
    }

    void apply(FrameReadStorage &input) {
        source = input;
        input.columns_context = this;
        input.column_binding = &column;
    }
};

template <template <template <typename, auto...> typename> typename Schema>
FrameReadStorage native_frame_input(const NativeFrame<Schema> &source) {
    auto input = frame::details::frame_read_storage(source.core(), std::addressof(source.core()), nullptr);
    input.valid = true;
    input.modified = source.change_time() != MIN_DT;
    input.last_updated = source.change_time();
    return input;
}

struct RowTraversalCounter {
    const NativeRows *rows{};
    mutable std::size_t traversals{};

    RowSet row_set() const noexcept {
        return RowSet{this,
                      rows,
                      rows->identity(),
                      [](const void *context) noexcept {
                          return static_cast<const RowTraversalCounter *>(context)->rows->size();
                      },
                      [](const void *context, row_id_t id) noexcept {
                          return static_cast<const RowTraversalCounter *>(context)->rows->contains(id);
                      },
                      [](const void *context, std::size_t position) {
                          return static_cast<const RowTraversalCounter *>(context)->rows->id_at(position);
                      },
                      [](const void *context, void *target, RowSet::visit_function visit) {
                          const auto &counter = *static_cast<const RowTraversalCounter *>(context);
                          ++counter.traversals;
                          for (auto slot : counter.rows->live_slots())
                              visit(target, counter.rows->id_at_slot(slot));
                      }};
    }
};

void test_native_frame_composite_keys() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(frame_schema_type<CompositeKeyRows>(), memory, [&](NativeFrameStorage &storage) {
        const auto add = [&](std::optional<i64> id, std::optional<i64> sequence, std::optional<double> price, i64 value,
                             EvaluationTime time) {
            const auto cell = std::optional<i64>{value};
            const auto values = std::array<const void *, 4>{std::addressof(id), std::addressof(sequence),
                                                            std::addressof(price), std::addressof(cell)};
            return storage.add(values, time, memory);
        };
        const auto first_time = MIN_DT + EvaluationTime::duration{1};
        const auto first = add(std::nullopt, 0, 0.0, 10, first_time);

        const auto first_key = std::array{KeyValue{std::monostate{}}, KeyValue{i64{0}}};
        require(storage.find_key(first_key) == first,
                "composite frame key did not match a null and integer component");

        const auto second = add(std::nullopt, 1, 1.0, 20, first_time);
        require(second != first && storage.find_key(first_key) == first,
                "composite frame key treated a nullable component as the whole key");

        auto duplicate_rejected = false;
        try {
            static_cast<void>(add(std::nullopt, 0, 2.0, 30, first_time));
        } catch (const std::invalid_argument &) {
            duplicate_rejected = true;
        }
        const auto other_key = std::array{KeyValue{std::monostate{}}, KeyValue{i64{1}}};
        require(duplicate_rejected && storage.size() == 2 && storage.find_key(first_key) == first &&
                    storage.find_key(other_key) == second,
                "duplicate composite key changed rows or damaged key lookup");

        const auto changed_sequence = std::optional<i64>{7};
        const auto changed_key_patch = std::array<frame::details::NativeFrameCellPatch, 4>{
            frame::details::NativeFrameCellPatch{},
            frame::details::NativeFrameCellPatch{.changed = true, .value = std::addressof(changed_sequence)},
            frame::details::NativeFrameCellPatch{}, frame::details::NativeFrameCellPatch{}};
        auto key_change_rejected = false;
        try {
            static_cast<void>(storage.set(first, changed_key_patch, first_time, memory));
        } catch (const std::invalid_argument &) {
            key_change_rejected = true;
        }
        const auto id_column = static_cast<const ColumnStorage<i64> *>(storage.column_storage(0));
        require(key_change_rejected && id_column->native->cell(storage.rows(), first) == std::nullopt &&
                    storage.find_key(first_key) == first,
                "rejected key-column update changed row values or damaged key lookup");

        const auto second_time = first_time + EvaluationTime::duration{1};
        require(storage.erase(first, second_time, memory), "keyed frame row could not be removed");
        const auto retained = storage.removed_key(first);
        require(retained.size() == 2 && std::holds_alternative<std::monostate>(retained[0]) &&
                    std::get<i64>(retained[1]) == 0 && !storage.find_key(first_key),
                "removed frame row did not retain its key or leave the live lookup");

        const auto replacement = add(std::nullopt, 0, -0.0, 40, second_time);
        require(replacement != first && storage.find_key(first_key) == replacement &&
                    storage.removed_key(first).size() == 2 && storage.removed().contains(first) &&
                    storage.added().contains(replacement),
                "same-cycle reinsert did not preserve the removed ID and publish a new live ID");

        const auto transient = add(i64{9}, i64{9}, 9.0, 90, second_time);
        const auto transient_key = std::array{KeyValue{i64{9}}, KeyValue{i64{9}}};
        require(storage.erase(transient, second_time, memory) && !storage.removed().contains(transient) &&
                    storage.removed_key(transient).empty() && !storage.find_key(transient_key),
                "same-cycle add/remove published a removal or retained a transient key");

        storage.clear(second_time + EvaluationTime::duration{1}, memory);
        require(storage.removed().contains(replacement) && storage.removed_key(replacement).size() == 2 &&
                    storage.removed_key(first).empty(),
                "clear did not retain current removals and expire the previous cycle's keys");
        storage.clear(second_time + EvaluationTime::duration{2}, memory);
        require(storage.removed_key(replacement).empty(), "removed frame key survived into the next cycle");
    });
    require(resource.allocations == resource.deallocations,
            "native keyed frame storage leaked allocations");
}

void test_frame_key_schema_validation() {
    using namespace bg;
    using namespace bg::frame;

    auto inherited_change_rejected = false;
    try {
        auto frame = NativeFrame<ChangedKeyQuotes>{};
        static_cast<void>(frame);
    } catch (const std::invalid_argument &) {
        inherited_change_rejected = true;
    }
    require(inherited_change_rejected,
            "direct native frame construction accepted an inherited key change");

    auto static_double_key_rejected = false;
    try {
        static_cast<void>(frame_type(frame_schema_type<DoubleKeyQuotes>()));
    } catch (const std::invalid_argument &) {
        static_double_key_rejected = true;
    }
    require(static_double_key_rejected, "static frame schema accepted a double key column");

    auto native_double_key_rejected = false;
    try {
        auto frame = NativeFrame<DoubleKeyQuotes>{};
        static_cast<void>(frame);
    } catch (const std::invalid_argument &) {
        native_double_key_rejected = true;
    }
    require(native_double_key_rejected, "direct native frame construction accepted a double key column");

    const auto fields = std::array{SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
                                  SchemaField{util::symbol{"other_id"}, &cpp_scalar_type<i64>()},
                                  SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()}};
    const auto &unkeyed = anonymous_schema(fields);
    const auto &unkeyed_type = frame_type(unkeyed);
    const auto &keyed_runtime = anonymous_schema(fields, key_metadata<"id">());
    const auto &different_key_runtime = anonymous_schema(fields, key_metadata<"other_id">());
    const auto &keyed_type = frame_type(keyed_runtime);
    const auto &different_key_type = frame_type(different_key_runtime);
    auto unkeyed_parameter = TypeMap{};
    require(unkeyed_type.match(keyed_type, unkeyed_parameter),
            "unkeyed frame parameter rejected a keyed frame with matching fields");
    auto keyed_parameter = TypeMap{};
    require(!keyed_type.match(unkeyed_type, keyed_parameter),
            "keyed frame parameter accepted an unkeyed frame schema");
    auto different_key_parameter = TypeMap{};
    require(!keyed_type.match(different_key_type, different_key_parameter),
            "runtime frame schemas with different key metadata matched");

    const auto &double_key_runtime = anonymous_schema(fields, key_metadata<"price">());
    auto runtime_double_key_rejected = false;
    try {
        static_cast<void>(frame_type(double_key_runtime));
    } catch (const std::invalid_argument &) {
        runtime_double_key_rejected = true;
    }
    require(runtime_double_key_rejected, "runtime frame schema accepted a double key column");

    auto runtime_series_double_key_rejected = false;
    try {
        static_cast<void>(frame_series_type(double_key_runtime, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        runtime_series_double_key_rejected = true;
    }
    require(runtime_series_double_key_rejected, "runtime frame time-series schema accepted a double key column");
}

void test_frame_scalar_key_validation() {
    using namespace bg;
    using namespace bg::frame;

    auto single_key_rejected = false;
    try {
        auto value = Frame<KeyedQuotes>{KeyedQuotes<Column>{.id = Column<i64>{{3, 3}},
                                                            .price = Column<double>{{10.0, 11.0}}}};
        static_cast<void>(value);
    } catch (const std::invalid_argument &) {
        single_key_rejected = true;
    }
    require(single_key_rejected, "raw Frame construction accepted duplicate keys");

    auto explicit_size_rejected = false;
    try {
        auto value = Frame<KeyedQuotes>{KeyedQuotes<Column>{.id = Column<i64>{{4, 4}},
                                                            .price = Column<double>{{10.0, 11.0}}},
                                         2};
        static_cast<void>(value);
    } catch (const std::invalid_argument &) {
        explicit_size_rejected = true;
    }
    require(explicit_size_rejected, "explicit-size Frame construction accepted duplicate keys");

    auto all_null_key_rejected = false;
    try {
        auto value = Frame<CompositeKeyRows>{CompositeKeyRows<Column>{
            .id = Column<i64>{{std::nullopt, std::nullopt}},
            .sequence = Column<i64>{{std::nullopt, std::nullopt}},
            .price = Column<double>{{10.0, 11.0}},
            .value = Column<i64>{{1, 2}}}};
        static_cast<void>(value);
    } catch (const std::invalid_argument &) {
        all_null_key_rejected = true;
    }
    require(all_null_key_rejected, "raw Frame construction accepted duplicate all-null composite keys");

    auto invalid_default_schema_rejected = false;
    try {
        auto value = Frame<DoubleKeyQuotes>{};
        static_cast<void>(value);
    } catch (const std::invalid_argument &) {
        invalid_default_schema_rejected = true;
    }
    require(invalid_default_schema_rejected, "default Frame construction bypassed key schema validation");

    const auto fields = std::array{SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
                                  SchemaField{util::symbol{"sequence"}, &cpp_scalar_type<i64>()},
                                  SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
                                  SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>()}};
    const auto &schema = anonymous_schema(fields, key_metadata<"id", "sequence">());
    auto ids = Column<i64>{{std::nullopt, std::nullopt}};
    auto sequences = Column<i64>{{std::nullopt, std::nullopt}};
    auto prices = Column<double>{{10.0, 11.0}};
    auto values = Column<i64>{{1, 2}};
    const auto columns = std::array<ColumnView<>, 4>{ColumnView<i64>{ids}, ColumnView<i64>{sequences},
                                                     ColumnView<double>{prices}, ColumnView<i64>{values}};
    auto resource = std::pmr::monotonic_buffer_resource{};
    auto memory = allocator{resource};
    auto runtime_duplicate_rejected = false;
    try {
        static_cast<void>(make_frame_value(memory, schema, 2, columns));
    } catch (const std::invalid_argument &) {
        runtime_duplicate_rejected = true;
    }
    require(runtime_duplicate_rejected, "runtime Frame value construction accepted duplicate all-null keys");
}

void test_native_frame_key_insert_rolls_back() {
    using namespace bg;
    using namespace bg::frame;

    auto failures = std::size_t{};
    auto succeeded = false;
    for (auto failure = std::size_t{1}; !succeeded; ++failure) {
        auto resource = FailingResource{};
        const auto memory = allocator{resource};
        resource.fail_at = failure;
        with_native_storage(frame_schema_type<KeyedQuotes>(), memory, [&](NativeFrameStorage &storage) {
            const auto id = std::optional<i64>{7};
            const auto price = std::optional<double>{12.5};
            const auto values = std::array<const void *, 2>{std::addressof(id), std::addressof(price)};
            const auto key = std::array{KeyValue{i64{7}}};
            try {
                static_cast<void>(storage.add(values, MIN_DT + EvaluationTime::duration{1}, memory));
                succeeded = true;
                require(storage.size() == 1 && storage.find_key(key).has_value(),
                        "keyed frame insert lost a row after allocation retry");
            } catch (const std::bad_alloc &) {
                ++failures;
                require(storage.size() == 0 && !storage.find_key(key) && storage.changed().empty(),
                        "failed keyed frame insert left a row, key, or row delta behind");

                resource.fail_at = 0;
                const auto inserted = storage.add(values, MIN_DT + EvaluationTime::duration{2}, memory);
                require(storage.find_key(key) == inserted,
                        "keyed frame insert retry found a stale partial key entry");
            }
        });
        require(resource.allocations == resource.deallocations,
                "keyed frame allocation failure leaked storage");
    }
    require(failures != 0, "keyed frame insert allocation retry did not exercise rollback");
}

void test_native_frame_key_removal_retention_rolls_back() {
    using namespace bg;
    using namespace bg::frame;

    auto failures = std::size_t{};
    auto succeeded = false;
    for (auto failure = std::size_t{1}; !succeeded; ++failure) {
        auto resource = FailingResource{};
        const auto memory = allocator{resource};
        with_native_storage(frame_schema_type<KeyedQuotes>(), memory, [&](NativeFrameStorage &storage) {
            const auto id_value = std::optional<i64>{7};
            const auto price = std::optional<double>{12.5};
            const auto values = std::array<const void *, 2>{std::addressof(id_value), std::addressof(price)};
            const auto id = storage.add(values, MIN_DT + EvaluationTime::duration{1}, memory);
            const auto key = std::array{KeyValue{i64{7}}};
            resource.fail_at = resource.attempts + failure;

            try {
                require(storage.erase(id, MIN_DT + EvaluationTime::duration{2}, memory),
                        "keyed frame row could not be removed during allocation retry");
                succeeded = true;
            } catch (const std::bad_alloc &) {
                ++failures;
                require(storage.rows().contains(id) && !storage.removed().contains(id) &&
                            storage.removed_key(id).empty() && storage.find_key(key) == id,
                        "failed key retention changed row membership or key lookup");

                resource.fail_at = 0;
                require(storage.erase(id, MIN_DT + EvaluationTime::duration{2}, memory) &&
                            storage.removed().contains(id) && storage.removed_key(id).size() == 1,
                        "key removal retry failed after retention allocation rollback");
            }
            if (succeeded)
                require(!storage.rows().contains(id) && storage.removed().contains(id) &&
                            storage.removed_key(id).size() == 1,
                        "successful key removal did not retain its delta key");
        });
        require(resource.allocations == resource.deallocations,
                "key retention allocation failure leaked frame storage");
    }
    require(failures != 0, "key retention allocation retry did not exercise rollback");
}

void test_runtime_native_storage_mixed_and_zero_column_schemas() {
    using namespace bg;
    using namespace bg::frame;

    const auto mixed_fields = std::array{
        SchemaField{util::symbol{"number"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
    };
    const auto &mixed_schema = anonymous_schema(mixed_fields);
    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(mixed_schema, memory, [&](NativeFrameStorage &storage) {
        const auto number = std::optional<i64>{42};
        const auto price = std::optional<double>{};
        const auto values = std::array<const void *, 2>{std::addressof(number), std::addressof(price)};
        const auto time = MIN_DT + EvaluationTime::duration{1};
        const auto id = storage.add(values, time, memory);
        const auto number_storage = static_cast<const ColumnStorage<i64> *>(storage.column_storage(0));
        const auto price_storage = static_cast<const ColumnStorage<double> *>(storage.column_storage(1));
        require(number_storage->native->cell(storage.rows(), id) == number &&
                    price_storage->native->cell(storage.rows(), id) == price,
                "runtime mixed schema lost its typed values or nulls");
        require(storage.changed_columns().contains(0) && storage.changed_columns().contains(1),
                "runtime mixed schema did not publish both columns");
    });

    const auto empty_fields = std::array<SchemaField, 0>{};
    const auto &empty_schema = anonymous_schema(empty_fields);
    with_native_storage(empty_schema, memory, [&](NativeFrameStorage &storage) {
        const auto id = storage.add({}, MIN_DT + EvaluationTime::duration{1}, memory);
        require(storage.size() == 1 && storage.contains(id) && storage.changed().contains(id),
                "zero-column runtime frame lost row membership");
        require(storage.changed_columns().empty(), "zero-column runtime frame published a column");
        require(storage.erase(id, MIN_DT + EvaluationTime::duration{2}, memory) && storage.size() == 0 &&
                    storage.removed().contains(id),
                "zero-column runtime frame could not remove row membership");
    });
}

void test_native_storage_validates_row_and_patch_shape_before_mutation() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(frame_schema_type<BidOnly>(), memory, [&](NativeFrameStorage &storage) {
        const auto first_time = MIN_DT + EvaluationTime::duration{1};
        auto rejected_row = false;
        try {
            storage.add({}, first_time, memory);
        } catch (const std::invalid_argument &) {
            rejected_row = true;
        }
        require(rejected_row && storage.size() == 0 && storage.change_time() == MIN_DT &&
                    storage.changed_columns().empty(),
                "invalid raw row shape mutated native frame state");

        const auto value = std::optional<double>{9.5};
        const auto values = std::array<const void *, 1>{std::addressof(value)};
        const auto id = storage.add(values, first_time, memory);
        auto rejected_patch = false;
        try {
            storage.set(id, {}, first_time + EvaluationTime::duration{1}, memory);
        } catch (const std::invalid_argument &) {
            rejected_patch = true;
        }
        const auto column = static_cast<const ColumnStorage<double> *>(storage.column_storage(0));
        require(rejected_patch && storage.change_time() == first_time && storage.changed().contains(id) &&
                    column->native->cell(storage.rows(), id) == value,
                "invalid raw patch shape changed frame deltas or values");

        const auto replacement = std::optional<double>{12.5};
        const auto patch = std::array<bg::frame::details::NativeFrameCellPatch, 1>{
            bg::frame::details::NativeFrameCellPatch{.changed = true, .value = std::addressof(replacement)}};
        const auto update_time = first_time + EvaluationTime::duration{1};
        require(storage.set(id, patch, update_time, memory) && storage.modified(0)->contains(id),
                "valid raw patch did not publish its row change");
        auto rejected_missing_row = false;
        try {
            static_cast<void>(storage.set(no_row, patch, update_time + EvaluationTime::duration{1}, memory));
        } catch (const std::out_of_range &) {
            rejected_missing_row = true;
        }
        require(rejected_missing_row && storage.change_time() == update_time && storage.changed().contains(id) &&
                    storage.modified(0)->contains(id) && storage.changed_columns().contains(0) &&
                    column->native->cell(storage.rows(), id) == replacement,
                "missing raw patch row changed the previous cycle's deltas or values");
    });
}

void test_native_storage_changed_columns_beyond_inline_capacity() {
    using namespace bg;
    using namespace bg::frame;

    auto names = std::vector<std::string>{};
    auto fields = std::vector<SchemaField>{};
    names.reserve(57);
    fields.reserve(57);
    for (auto index = std::size_t{}; index != 57; ++index) {
        names.push_back("column_" + std::to_string(index));
        const auto *type = index % 2 == 0 ? &cpp_scalar_type<i64>() : &cpp_scalar_type<double>();
        fields.push_back({util::symbol{names.back()}, type});
    }
    const auto &schema = anonymous_schema(fields);
    auto resource = FailingResource{};
    const auto memory = allocator{resource};
    with_native_storage(schema, memory, [&](NativeFrameStorage &storage) {
        auto integers = std::vector<std::optional<i64>>(29);
        auto doubles = std::vector<std::optional<double>>(28);
        auto values = std::vector<const void *>{};
        values.reserve(57);
        for (auto index = std::size_t{}; index != 57; ++index) {
            if (index % 2 == 0) {
                integers[index / 2] = static_cast<i64>(index);
                values.push_back(std::addressof(integers[index / 2]));
            } else {
                doubles[index / 2] = static_cast<double>(index) + 0.5;
                values.push_back(std::addressof(doubles[index / 2]));
            }
        }

        resource.fail_at = resource.attempts + 1;
        auto allocation_failed = false;
        try {
            storage.add(values, MIN_DT + EvaluationTime::duration{1}, memory);
        } catch (const std::bad_alloc &) {
            allocation_failed = true;
        }
        require(allocation_failed && storage.size() == 0 && storage.change_time() == MIN_DT &&
                    storage.changed_columns().empty(),
                "changed-column allocation failure published a partial frame row");

        resource.fail_at = 0;
        const auto time = MIN_DT + EvaluationTime::duration{1};
        const auto id = storage.add(values, time, memory);
        require(storage.size() == 1 && storage.changed_columns().contains(0) &&
                    storage.changed_columns().contains(55) && storage.changed_columns().contains(56),
                "changed-column index did not publish columns beyond inline capacity");

        const auto updated = std::optional<i64>{5600};
        auto patches = std::vector<bg::frame::details::NativeFrameCellPatch>(57);
        patches[56] = {.changed = true, .value = std::addressof(updated)};
        const auto update_time = time + EvaluationTime::duration{1};
        require(storage.set(id, patches, update_time, memory) && storage.modified(56)->contains(id) &&
                    storage.changed_columns().contains(56),
                "changed-column index did not retain a high-column modification");

        const auto &series_type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
        const auto &runtime = frame_time_series_runtime_type<SCHEMA>(series_type);
        const auto &field_layout = storage.layout().fields[56];
        const auto &item_layout = runtime.item_layout(56);
        const auto *frame_bytes = reinterpret_cast<const std::byte *>(std::addressof(storage));
        const auto child = runtime.item(std::addressof(storage), 56);
        require(runtime.item_count(std::addressof(storage)) == 57 &&
                    item_layout.last_updated_offset == field_layout.child_state_offset &&
                    item_layout.value_offset == field_layout.column_storage_offset &&
                    std::addressof(child.last_updated) ==
                        reinterpret_cast<const TimeSeriesState *>(frame_bytes + field_layout.child_state_offset) &&
                    child.value == storage.column_storage(56) && child.last_updated.time() == update_time,
                "high-column runtime item does not address its native child state and value");

        const auto high_column = static_cast<const ColumnStorage<i64> *>(child.value);
        require(high_column->native->cell(storage.rows(), id) == updated &&
                    runtime.item_updated(TimeSeriesState{update_time}, std::addressof(storage), 56) &&
                    !runtime.item_updated(TimeSeriesState{update_time}, std::addressof(storage), 55),
                "high-column runtime item update does not match the stored cell");

        auto updated_items = std::vector<item_id_t>{};
        runtime.for_each_updated_item(
            TimeSeriesState{update_time}, std::addressof(storage), std::addressof(updated_items),
            [](void *context, item_id_t item) { static_cast<std::vector<item_id_t> *>(context)->push_back(item); });
        require(updated_items == std::vector<item_id_t>{56},
                "high-column updated-item iteration disagrees with the changed-column index");

        auto modified_items = std::vector<TimeSeriesRuntimeType::ModifiedItem>{};
        runtime.for_each_modified_item(
            TimeSeriesState{time}, std::addressof(storage), update_time, std::addressof(modified_items),
            [](void *context, TimeSeriesRuntimeType::ModifiedItem item) {
                static_cast<std::vector<TimeSeriesRuntimeType::ModifiedItem> *>(context)->push_back(item);
            });
        require(modified_items.size() == 1 && modified_items[0].id == 56 &&
                    modified_items[0].runtime_type == std::addressof(child.runtime_type) &&
                    modified_items[0].last_updated == std::addressof(child.last_updated) &&
                    modified_items[0].value == child.value,
                "high-column modified-item iteration disagrees with the runtime child view");
    });
    require(resource.allocations == resource.deallocations,
            "runtime schema native frame leaked allocations after wide-column tests");
}

void test_shared_rows_and_column_changes() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto frame = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
    const auto &layout = bg::frame::details::native_frame_layout<Quotes>();
    const auto &runtime = frame_time_series_runtime_type<Quotes>(
        frame_time_series_type<Quotes>(TimeSeriesGuarantee::ordinary));
    const auto &field_layout = layout.fields[1];
    const auto &item_layout = runtime.item_layout(1);
    require(item_layout.last_updated_offset == field_layout.child_state_offset &&
                item_layout.value_offset == field_layout.column_storage_offset,
            "TSF child offsets differ from the native field layout");
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = frame.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                           .ask = std::nullopt},
                              first_time, memory);

    const auto child = runtime.item(&frame.core(), 1);
    const auto *frame_bytes = reinterpret_cast<const std::byte *>(std::addressof(frame.core()));
    require(std::addressof(child.last_updated) ==
                reinterpret_cast<const TimeSeriesState *>(frame_bytes + field_layout.child_state_offset) &&
                child.value == frame.core().column_storage(1),
            "TSF runtime child does not address the native field storage");

    require(frame.size() == 1 && frame.contains(id), "frame did not add one row");
    require(frame.cell<0>(id) == 5 && frame.cell<1>(id) == 10.5 && !frame.cell<2>(id),
            "frame columns do not share the inserted row");
    auto changed = std::vector<row_id_t>{};
    frame.for_each_changed([&](row_id_t value) { changed.push_back(value); });
    require(changed == std::vector<row_id_t>{id}, "frame-wide changed iteration duplicated membership changes");

    auto bid_patch = Quotes<PatchCell>{};
    bid_patch.bid = 11.25;
    const auto second_time = first_time + EvaluationTime::duration{1};
    frame.set(id, bid_patch, second_time, memory);
    require(frame.modified<1>().contains(id) && !frame.modified<2>().contains(id),
            "frame did not record the changed column");

    const auto third_time = second_time + EvaluationTime::duration{1};
    auto ask_patch = Quotes<PatchCell>{};
    ask_patch.ask = std::nullopt;
    frame.set(id, ask_patch, third_time, memory);
    require(frame.modified<1>().empty() && frame.modified<2>().contains(id),
            "column modification state survived another column's later epoch");
    require(!frame.cell<2>(id), "explicit null update did not clear the cell");

    const auto second = frame.add(Quotes<Cell>{.size = Cell<i64>{6}, .bid = Cell<double>{std::nullopt},
                                                .ask = Cell<double>{12.0}},
                                  third_time, memory);
    changed.clear();
    frame.for_each_changed([&](row_id_t value) { changed.push_back(value); });
    require(changed == std::vector<row_id_t>{id, second}, "changed iteration did not emit the union once");
    require(frame.erase(id, third_time, memory), "frame row erase failed");
    require(!frame.contains(id), "erased frame row is still live");
    auto removed = std::vector<row_id_t>{};
    frame.for_each_removed([&](row_id_t value) { removed.push_back(value); });
    require(removed == std::vector<row_id_t>{id}, "frame removal delta is incorrect");
}

void test_failed_multicolumn_insert_cleanup() {
    using namespace bg;
    using namespace bg::frame;

    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto failures = std::size_t{};
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource resource;
        auto succeeded = false;
        {
            const auto memory = allocator{resource};
            auto frame = NativeFrame<Quotes>{};
            auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
            resource.fail_at = resource.attempts + failure;
            try {
                frame.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                       .ask = Cell<double>{12.5}},
                          time, memory);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                ++failures;
                require(frame.size() == 0 && frame.rows().added().empty() && frame.changed_columns().empty(),
                        "failed multi-column insert left a partial row or publication");
            }
        }
        require(resource.allocations == resource.deallocations, "failed multi-column insert leaked storage");
        if (succeeded)
            break;
    }
    require(failures > 1, "multi-column insert did not exercise failures after the first allocation");
}

void test_recycled_rows_reuse_physical_slots() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto column = NativeColumn<i64>{};
    auto release = util::scope_exit{[&]() noexcept { column.release(memory); }};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first = column.add(10, first_time, memory);
    const auto second = column.add(20, first_time, memory);
    const auto slot_for = [&](row_id_t id) {
        auto slot = no_row;
        column.for_each_batch([&](const auto &batch) {
            for (auto position = std::size_t{}; position != batch.ids.size(); ++position)
                if (batch.ids[position] == id)
                    slot = batch.cells.first_slot + static_cast<row_id_t>(position);
        });
        require(slot != no_row, "live row did not appear in a native batch");
        return slot;
    };
    const auto first_slot = slot_for(first);

    const auto second_time = first_time + EvaluationTime::duration{1};
    require(column.erase(first, second_time, memory), "slot reuse test could not erase the first row");
    const auto third = column.add(30, second_time, memory);
    require(third != first && slot_for(third) == first_slot,
            "same-cycle row addition did not reuse the physical slot with a fresh row ID");

    auto rows = std::vector<std::pair<row_id_t, std::optional<i64>>>{};
    column.for_each_batch([&](const auto &batch) {
        require(batch.ids.size() == batch.cells.size, "column batch IDs and cells have different lengths");
        for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
            const auto value = batch.cells.is_valid(position)
                                   ? std::optional<i64>{batch.cells.values[position]}
                                   : std::nullopt;
            rows.emplace_back(batch.ids[position], value);
        }
    });
    require(rows == std::vector<std::pair<row_id_t, std::optional<i64>>>{{third, 30}, {second, 20}},
            "reused physical slot batch did not align row IDs and values");

    const auto third_time = second_time + EvaluationTime::duration{1};
    require(column.erase(third, third_time, memory), "slot reuse test could not erase the replacement row");
    const auto fourth = column.add(40, third_time, memory);
    require(fourth == first && slot_for(fourth) == first_slot,
            "a row ID was not recycled after its removal cycle while reusing its slot");

    rows.clear();
    column.for_each_batch([&](const auto &batch) {
        require(batch.ids.size() == batch.cells.size, "recycled ID batch has mismatched lengths");
        for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
            const auto value = batch.cells.is_valid(position)
                                   ? std::optional<i64>{batch.cells.values[position]}
                                   : std::nullopt;
            rows.emplace_back(batch.ids[position], value);
        }
    });
    require(rows == std::vector<std::pair<row_id_t, std::optional<i64>>>{{fourth, 40}, {second, 20}},
            "recycled row ID batch did not align with its current value");

    auto churn = NativeColumn<i64>{};
    auto release_churn = util::scope_exit{[&]() noexcept { churn.release(memory); }};
    auto current = churn.add(0, first_time, memory);
    for (auto cycle = std::uint32_t{1}; cycle <= 256; ++cycle) {
        const auto time = first_time + EvaluationTime::duration{cycle};
        require(churn.erase(current, time, memory), "bounded churn could not erase its live row");
        current = churn.add(static_cast<i64>(cycle), time, memory);
        require(churn.size() == 1 && churn.live_slots().extent() == 1,
                "bounded row churn expanded physical slot storage");
        require(churn.id_at(0) == current && churn.cell(current) == static_cast<i64>(cycle),
                "bounded row churn lost row and cell alignment");
        auto batch_rows = std::size_t{};
        churn.for_each_batch([&](const auto &batch) {
            require(batch.ids.size() == batch.cells.size && batch.cells.size == 1 &&
                        batch.ids[0] == current && batch.cells.is_valid(0) &&
                        batch.cells.values[0] == static_cast<i64>(cycle),
                    "bounded row churn batch lost ID and value alignment");
            ++batch_rows;
        });
        require(batch_rows == 1, "bounded row churn did not export exactly one batch");
    }
}

void test_failed_value_insert_rolls_back_reused_slot() {
    using namespace bg;
    using namespace bg::frame;

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    auto failures = std::size_t{};
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource resource;
        auto succeeded = false;
        {
            const auto memory = allocator{resource};
            auto frame = NativeFrame<BidOnly>{};
            auto release = util::scope_exit{[&]() noexcept { frame.release(memory); }};
            const auto first = frame.add(BidOnly<Cell>{.price = Cell<double>{std::nullopt}}, first_time, memory);
            const auto second = frame.add(BidOnly<Cell>{.price = Cell<double>{std::nullopt}}, first_time, memory);
            const auto second_time = first_time + EvaluationTime::duration{1};
            require(frame.erase(first, second_time, memory), "reused-slot failure test could not erase a row");

            resource.fail_at = resource.attempts + failure;
            try {
                frame.add(BidOnly<Cell>{.price = Cell<double>{42.0}}, second_time, memory);
                succeeded = true;
            } catch (const std::bad_alloc &) {
                ++failures;
                require(frame.size() == 1 && !frame.contains(first) && frame.contains(second) &&
                            !frame.cell<0>(second) && frame.rows().added().empty() &&
                            frame.rows().removed().contains(first) && frame.changed_columns().contains(0),
                        "failed value insertion corrupted rows or deltas after slot reuse");
            }
        }
        require(resource.allocations == resource.deallocations,
                "reused-slot insertion failure leaked frame storage");
        if (succeeded)
            break;
    }
    require(failures > 0, "reused-slot value insertion did not exercise an allocation failure");
}

void test_frame_scalar_views_and_snapshots() {
    using namespace bg;
    using namespace bg::frame;

    auto columns = Quotes<Column>{.size = Column<i64>{{i64{5}, i64{6}}},
                                  .bid = Column<double>{{10.5, 11.5}},
                                  .ask = Column<double>{{std::nullopt, 12.5}}};
    auto immutable = Frame<Quotes>{std::move(columns)};
    require(immutable.size() == 2, "immutable frame has wrong row count");
    auto immutable_view = FrameView<Quotes>{immutable};
    auto scalar = Scalar<Frame<Quotes>>::from(frame::details::frame_view_scalar_runtime_type<Quotes>(),
                                               std::addressof(immutable_view));
    const auto view = scalar.value();
    require(view.size() == 2 && view.columns().size.cell(1) == 6 && view.columns().bid.cell(0) == 10.5 &&
                !view.columns().ask.cell(0),
            "immutable frame scalar view lost columns, values, or nulls");
    require(&cpp_scalar_type<Frame<Quotes>>() == &cpp_scalar_type<Frame<Quotes>>(),
            "frame schema scalar identity is not canonical");
    require(TypeRegistry::instance().contains(cpp_scalar_type<Frame<Quotes>>()),
            "frame scalar semantic type was not registered");
    const auto &series_type = frame_time_series_type<Quotes>(TimeSeriesGuarantee::ordinary);
    require(&series_type == &frame_time_series_type<Quotes>(TimeSeriesGuarantee::ordinary),
            "frame schema time series identity is not canonical");
    require(TypeRegistry::instance().contains(series_type), "frame time series semantic type was not registered");
    auto scalar_type_name = std::string{};
    cpp_scalar_type<Frame<Quotes>>().format_name(scalar_type_name);
    require(scalar_type_name == "Frame<Quotes>", "frame scalar type formatted with the wrong name");
    auto series_type_name = std::string{};
    series_type.format_name(series_type_name);
    require(series_type_name == "TSF<Quotes>", "frame time series type formatted with the wrong name");
    require(&frame_time_series_runtime_type<Quotes>(series_type) ==
                &frame_time_series_runtime_type<Quotes>(series_type),
            "frame schema runtime identity is not canonical");

    auto empty = Frame<EmptySchema>{EmptySchema<Column>{}, 4};
    auto empty_view = FrameView<EmptySchema>{empty};
    auto empty_scalar = Scalar<Frame<EmptySchema>>::from(
        frame::details::frame_view_scalar_runtime_type<EmptySchema>(), std::addressof(empty_view));
    require(empty_scalar.value().size() == 4, "zero-column scalar frame lost its row count");

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto native = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept { native.release(memory); }};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto id = native.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                            .ask = Cell<double>{std::nullopt}},
                              time, memory);
    const auto &native_runtime = static_cast<const FrameSeriesRuntimeType &>(
        frame_time_series_runtime_type<Quotes>(series_type));
    auto native_scalar = Scalar<Frame<Quotes>>::from(native_runtime.scalar_runtime_type(),
                                                      std::addressof(native.core()));
    const auto snapshot_time = time + EvaluationTime::duration{1};
    FailingResource failing_memory;
    auto failing_allocator = allocator{failing_memory};
    auto succeeded = false;
    for (auto failure = std::size_t{1};; ++failure) {
        FailingResource attempt_memory;
        auto attempt_allocator = allocator{attempt_memory};
        attempt_memory.fail_at = attempt_memory.attempts + failure;
        try {
            auto owned = ScalarValue<Frame<Quotes>>{native_scalar, attempt_allocator};
            succeeded = true;
        } catch (const std::bad_alloc &) {
            require(attempt_memory.allocations == attempt_memory.deallocations,
                    "failed frame snapshot leaked an allocation");
        }
        require(attempt_memory.allocations == attempt_memory.deallocations,
                "frame snapshot cleanup leaked an allocation");
        if (succeeded)
            break;
    }

    {
        auto stable = ScalarValue<Frame<Quotes>>{native_scalar, failing_allocator};
        auto bid_patch = Quotes<PatchCell>{};
        bid_patch.bid = 99.0;
        native.set(id, bid_patch, snapshot_time, memory);
        require(stable.view().value().columns().bid.cell(0) == 10.5,
                "owning frame scalar changed after native row mutation");
    }
    require(failing_memory.allocations == failing_memory.deallocations, "owning frame snapshot leaked allocations");

    auto snapshot_allocator = allocator{};
    auto retained_snapshot = ScalarValue<Frame<Quotes>>{native_scalar, snapshot_allocator};
    const auto retained_view = retained_snapshot.view().value();
    const auto retained_source = scalar_storage_traits<Frame<Quotes>>::borrow_value(retained_view);
    const auto *retained_owner = frame::details::frame_snapshot_owner(retained_source);
    auto retained_copy = FrameSnapshotStorage{};
    const auto &snapshot_runtime = frame_snapshot_runtime_type(frame_type(frame_schema_type<Quotes>()));
    snapshot_runtime.construct_from_input(std::addressof(retained_copy), retained_source, snapshot_allocator);
    require(retained_owner != nullptr && retained_copy.owner == retained_owner,
            "frame snapshot construction did not retain its existing owner");
    auto *bundle_owner = bg::details::BundleAllocationOwner::create(snapshot_allocator);
    auto release_bundle_owner = util::scope_exit{[&]() noexcept { bundle_owner->release(); }};
    {
        const auto allocation_context = bg::details::ScalarAllocationContext{
            snapshot_allocator, *bundle_owner, std::addressof(retained_copy), 0};
        snapshot_runtime.assign_from_input(std::addressof(retained_copy), retained_source);
    }
    require(retained_copy.owner == retained_owner,
            "frame snapshot self-assignment did not retain its owner before releasing it");
    snapshot_runtime.destroy(std::addressof(retained_copy), snapshot_allocator);

    require(cpp_scalar_type<Frame<Quotes>>().accepts(cpp_scalar_type<Frame<EmptySchema>>()) == false,
            "frame semantic types with distinct schemas matched");

    const auto &generic_frame_type = frame_type(frame_schema_type<bg::SCHEMA>());
    const auto &concrete_frame_type = frame_type(frame_schema_type<Quotes>());
    auto generic_frame_name = std::string{};
    generic_frame_type.format_name(generic_frame_name);
    require(generic_frame_name == "Frame<SCHEMA>", "generic frame type formatted with the wrong name");
    auto frame_type_map = TypeMap{};
    require(generic_frame_type.match(concrete_frame_type, frame_type_map),
            "generic frame schema did not capture a concrete schema");
    require(!generic_frame_type.match(frame_type(frame_schema_type<EmptySchema>()), frame_type_map),
            "a repeated frame schema variable matched a different schema");
    require(&generic_frame_type.resolve(frame_type_map) == &concrete_frame_type,
            "generic frame schema did not resolve to the captured schema");

    const auto anonymous_fields = std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>()}};
    const auto &anonymous_schema = bg::anonymous_schema(anonymous_fields);
    require(&anonymous_schema == &bg::anonymous_schema(anonymous_fields),
            "equivalent anonymous schema descriptors were not canonical");
    require(&frame_type(anonymous_schema) == &frame_type(bg::anonymous_schema(anonymous_fields)),
            "frame types over canonical anonymous schemas were not canonical");
    require(&frame_series_type(anonymous_schema, TimeSeriesGuarantee::ordinary) ==
                &frame_series_type(bg::anonymous_schema(anonymous_fields), TimeSeriesGuarantee::ordinary),
            "frame series types over canonical anonymous schemas were not canonical");

    const auto optional_fields = std::array{SchemaField{util::symbol{"optional"}, &cpp_scalar_type<i64>(), true}};
    const auto &optional_schema = bg::anonymous_schema(optional_fields);
    auto optional_frame_rejected = false;
    auto optional_series_rejected = false;
    try {
        static_cast<void>(frame_type(optional_schema));
    } catch (const std::invalid_argument &) {
        optional_frame_rejected = true;
    }
    try {
        static_cast<void>(frame_series_type(optional_schema, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        optional_series_rejected = true;
    }
    require(optional_frame_rejected && optional_series_rejected,
            "frame scalar or time series accepted an optional column field");

    using OPTIONAL_CELL = ScalarVariable<"FRAME_OPTIONAL_CELL">;
    const auto optional_unresolved_fields = std::array{SchemaField{
        util::symbol{"optional_unresolved"}, &cpp_scalar_type<OPTIONAL_CELL>(), true}};
    const auto &optional_unresolved_schema = bg::anonymous_schema(optional_unresolved_fields);
    auto optional_unresolved_frame_rejected = false;
    auto optional_unresolved_series_rejected = false;
    try {
        static_cast<void>(frame_type(optional_unresolved_schema));
    } catch (const std::invalid_argument &) {
        optional_unresolved_frame_rejected = true;
    }
    try {
        static_cast<void>(frame_series_type(optional_unresolved_schema, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        optional_unresolved_series_rejected = true;
    }
    require(optional_unresolved_frame_rejected && optional_unresolved_series_rejected,
            "frame scalar or time series accepted an optional field with an unresolved scalar type");

    const auto non_scalar_fields = std::array{SchemaField{
        util::symbol{"nested"}, &column_time_series_type<i64>(TimeSeriesGuarantee::ordinary), false}};
    const auto &non_scalar_schema = bg::anonymous_schema(non_scalar_fields);
    auto non_scalar_frame_rejected = false;
    auto non_scalar_series_rejected = false;
    try {
        static_cast<void>(frame_type(non_scalar_schema));
    } catch (const std::invalid_argument &) {
        non_scalar_frame_rejected = true;
    }
    try {
        static_cast<void>(frame_series_type(non_scalar_schema, TimeSeriesGuarantee::ordinary));
    } catch (const std::invalid_argument &) {
        non_scalar_series_rejected = true;
    }
    require(non_scalar_frame_rejected && non_scalar_series_rejected,
            "frame scalar or time series accepted a non-scalar field");

    const auto &generic_series_type = frame_series_type(frame_schema_type<bg::SCHEMA>(),
                                                        TimeSeriesGuarantee::ordinary);
    auto generic_series_name = std::string{};
    generic_series_type.format_name(generic_series_name);
    require(generic_series_name == "TSF<SCHEMA>", "generic frame time series type formatted with the wrong name");
    auto series_type_map = TypeMap{};
    require(generic_series_type.match(series_type, series_type_map),
            "generic frame series schema did not capture a concrete schema");
    require(&generic_series_type.resolve(series_type_map) == &series_type,
            "generic frame series schema did not resolve to the captured schema");

    const auto &inherited_base_series =
        frame_series_type(frame_schema_type<InheritedFrameBase>(), TimeSeriesGuarantee::ordinary);
    const auto &inherited_derived_series =
        frame_series_type(frame_schema_type<InheritedFrame>(), TimeSeriesGuarantee::ordinary);
    auto inherited_match_map = TypeMap{};
    require(inherited_base_series.match(inherited_derived_series, inherited_match_map),
            "inherited TSF did not match its named base schema");
    auto reverse_inherited_match_map = TypeMap{};
    require(!inherited_derived_series.match(inherited_base_series, reverse_inherited_match_map),
            "inherited TSF accepted its base schema as a derived schema");
    auto unrelated_schema_match_map = TypeMap{};
    require(!inherited_base_series.match(series_type, unrelated_schema_match_map),
            "inherited TSF accepted an unrelated named schema with matching fields");

    const auto integer_fields =
        std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>(), false}};
    const auto double_fields =
        std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<double>(), false}};
    const auto &integer_schema = bg::anonymous_schema(integer_fields);
    const auto &double_schema = bg::anonymous_schema(double_fields);
    const auto &integer_series = frame_series_type(integer_schema, TimeSeriesGuarantee::ordinary);
    const auto &double_series = frame_series_type(double_schema, TimeSeriesGuarantee::ordinary);
    auto mismatched_cell_type_map = TypeMap{};
    require(!integer_series.match(double_series, mismatched_cell_type_map),
            "TSF matched anonymous schemas with incompatible column cell types");

    const auto &ordinary_series = frame_series_type(frame_schema_type<Quotes>(), TimeSeriesGuarantee::ordinary);
    for (const auto limit : {TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_}) {
        auto rejected_guarantee = false;
        try {
            static_cast<void>(frame_series_type(frame_schema_type<Quotes>(), limit));
        } catch (const std::invalid_argument &) {
            rejected_guarantee = true;
        }
        require(rejected_guarantee, "TSF accepted a nonordinary guarantee");

        const auto &limited = dynamic_cast<const FrameSeriesType &>(
            ordinary_series.with_guarantee_limit(limit));
        require(std::addressof(limited) == std::addressof(ordinary_series),
                "TSF guarantee limit strengthened its canonical ordinary type");
        require(limited.guarantee().value() == TimeSeriesGuarantee::ordinary,
                "TSF guarantee limit changed its ordinary frame guarantee");
        for (auto index = std::size_t{}; index != limited.item_count(); ++index) {
            const auto &column = dynamic_cast<const ColumnSeriesType &>(limited.item_type(index));
            require(column.guarantee().value() == TimeSeriesGuarantee::ordinary,
                    "TSF guarantee limit changed its ordinary child TSC guarantee");
        }
    }

    auto shared_schema_type_map = TypeMap{};
    require(generic_frame_type.match(concrete_frame_type, shared_schema_type_map),
            "generic frame scalar did not capture a shared schema");
    require(generic_series_type.match(series_type, shared_schema_type_map),
            "generic frame time series did not reuse the scalar schema capture");
    require(&generic_series_type.resolve(shared_schema_type_map) == &series_type,
            "generic frame time series resolved to a different schema than the scalar");
    const auto &shared_schema_variable = dynamic_cast<const Variable &>(generic_frame_type.schema_type());
    const auto *captured_schema = shared_schema_type_map.find(shared_schema_variable);
    const auto &mismatched_series_type = frame_series_type(frame_schema_type<EmptySchema>(),
                                                           TimeSeriesGuarantee::ordinary);
    require(!generic_series_type.match(mismatched_series_type, shared_schema_type_map),
            "generic frame time series accepted a different schema in a shared type map");
    require(shared_schema_type_map.find(shared_schema_variable) == captured_schema,
            "a failed time series match changed the shared scalar schema capture");

    const auto &anonymous_series_type = frame_series_type(anonymous_schema, TimeSeriesGuarantee::ordinary);
    auto anonymous_type_map = TypeMap{};
    require(generic_frame_type.match(frame_type(anonymous_schema), anonymous_type_map) &&
                generic_series_type.match(anonymous_series_type, anonymous_type_map),
            "Frame and TSF did not share an anonymous runtime schema capture");
    require(&generic_frame_type.resolve(anonymous_type_map) == &frame_type(anonymous_schema) &&
                &generic_series_type.resolve(anonymous_type_map) == &anonymous_series_type,
            "Frame and TSF did not resolve to the shared anonymous runtime schema");

    const auto &empty_series_type = frame_series_type(frame_schema_type<EmptySchema>(),
                                                      TimeSeriesGuarantee::ordinary);
    require(generic_series_type.item_count() == std::dynamic_extent && empty_series_type.item_count() == 0,
            "unknown frame schema arity was confused with an empty schema");

    constexpr auto FRAME_GUARANTEE = guarantee_variable<"FRAME_SCHEMA_GUARANTEE">;
    const auto guarantee_parameter = bg::details::guarantee_parameter<FRAME_GUARANTEE>();
    const auto &schema_and_guarantee_type = frame_series_type(frame_schema_type<bg::SCHEMA>(), guarantee_parameter);
    auto guarantee_type_map = TypeMap{};
    require(schema_and_guarantee_type.match(series_type, guarantee_type_map),
            "frame series schema and guarantee variables did not match");
    require(&schema_and_guarantee_type.resolve(guarantee_type_map) == &series_type,
            "frame series schema and guarantee variables did not resolve");

    static const auto minimum_deduplicated =
        GuaranteeVariable{util::symbol{"FRAME_SCHEMA_MIN_DEDUPLICATED"}, TimeSeriesGuarantee::deduplicated};
    const auto &constrained_series_type = frame_series_type(
        frame_schema_type<bg::SCHEMA>(), GuaranteeParameter{minimum_deduplicated});
    const auto &schema_variable = dynamic_cast<const Variable &>(constrained_series_type.schema_type());
    auto rollback_type_map = TypeMap{};
    require(!constrained_series_type.match(series_type, rollback_type_map),
            "a frame guarantee variable matched below its minimum guarantee");
    require(rollback_type_map.find(schema_variable) == nullptr,
            "failed frame series matching retained a schema variable mapping");
    require(rollback_type_map.find(minimum_deduplicated) == nullptr,
            "failed frame series matching retained a guarantee variable mapping");
}

void test_runtime_frame_value_factory() {
    using GenericFrame = Frame<bg::SCHEMA>;
    using GenericValue = ScalarValue<GenericFrame>;

    const auto fields = std::array{SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>()}};
    const auto &schema = anonymous_schema(fields);
    auto source = std::optional<GenericValue>{};
    auto memory_resource = FailingResource{};
    auto memory = allocator{memory_resource};
    {
        auto column = Column<i64>{{11, 22, 33}};
        const auto columns = std::array{ColumnView<>{column.view()}};
        static_assert(std::same_as<decltype(make_frame_value(memory, schema, 3, columns)), GenericValue>);
        source.emplace(make_frame_value(memory, schema, 3, columns));
    }

    const auto view = source->view().value();
    require(view.size() == 3 && view.schema().field_count() == 1 &&
                view.schema().field(0).name == util::symbol{"value"} &&
                view.column("value").view<i64>().cell(0) == 11 &&
                view.column("value").view<i64>().cell(2) == 33,
            "runtime-schema frame value lost its schema, values, or source independence");

    const auto empty_fields = std::array<SchemaField, 0>{};
    const auto &empty_schema = anonymous_schema(empty_fields);
    auto empty = std::optional<GenericValue>{};
    empty.emplace(make_frame_value(memory, empty_schema, 5, std::span<const ColumnView<>>{}));
    require(empty->view().value().size() == 5 && empty->view().value().schema().field_count() == 0,
            "zero-column frame value lost its nonzero row count or schema");

    const auto mixed_fields = std::array{
        SchemaField{util::symbol{"number"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
    };
    const auto &mixed_schema = anonymous_schema(mixed_fields);
    auto numbers = Column<i64>{{11, std::nullopt}};
    auto prices = Column<double>{{std::nullopt, 2.5}};
    const auto mixed_columns = std::array{ColumnView<>{numbers.view()}, ColumnView<>{prices.view()}};
    auto mixed = std::optional<GenericValue>{make_frame_value(memory, mixed_schema, 2, mixed_columns)};
    const auto mixed_view = mixed->view().value();
    require(mixed_view.column("number").cell<i64>(0) == 11 && !mixed_view.column("number").cell<i64>(1) &&
                !mixed_view.column("price").cell<double>(0) && mixed_view.column("price").cell<double>(1) == 2.5,
            "runtime-schema frame factory changed mixed numeric values or nulls");

    const auto rejects_columns = [&](const bg::Schema &target_schema, std::size_t row_count,
                                     std::span<const ColumnView<>> values) {
        try {
            static_cast<void>(make_frame_value(memory, target_schema, row_count, values));
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    const auto wrong_count = std::array{ColumnView<>{numbers.view()}};
    require(rejects_columns(mixed_schema, 2, wrong_count),
            "runtime-schema frame factory accepted a mismatched column count");
    const auto wrong_type = std::array{ColumnView<>{prices.view()}, ColumnView<>{prices.view()}};
    require(rejects_columns(mixed_schema, 2, wrong_type),
            "runtime-schema frame factory accepted a mismatched column type");
    auto short_prices = Column<double>{{2.5}};
    const auto wrong_length = std::array{ColumnView<>{numbers.view()}, ColumnView<>{short_prices.view()}};
    require(rejects_columns(mixed_schema, 2, wrong_length),
            "runtime-schema frame factory accepted columns with different lengths");

    source.reset();
    mixed.reset();
    empty.reset();
    require(memory_resource.allocations == memory_resource.deallocations,
            "runtime-schema frame values did not release successful allocations");

    auto failures = std::size_t{};
    auto succeeded = false;
    for (auto failure = std::size_t{1};; ++failure) {
        auto attempt_resource = FailingResource{};
        auto attempt_memory = allocator{attempt_resource};
        attempt_resource.fail_at = failure;
        try {
            auto column = Column<i64>{{7, 9}};
            const auto columns = std::array{ColumnView<>{column.view()}};
            auto value = make_frame_value(attempt_memory, schema, 2, columns);
            require(value.view().value().column("value").view<i64>().cell(1) == 9,
                    "runtime-schema frame value changed during allocation retry");
            succeeded = true;
        } catch (const std::bad_alloc &) {
            ++failures;
        }
        require(attempt_resource.allocations == attempt_resource.deallocations,
                "runtime-schema frame value factory leaked during allocation failure or success");
        if (succeeded)
            break;
    }
    require(failures > 0, "runtime-schema frame value factory did not exercise allocation rollback");
}

void test_scalar_variable_frame_input() {
    using namespace bg;
    using namespace bg::frame;

    const auto &generic_schema = frame_schema_type<GenericQuote>();
    auto type_map = TypeMap{};
    type_map.set(bg::details::scalar_variable_instance<SCALAR>(), &cpp_scalar_type<double>());
    const auto &schema = dynamic_cast<const bg::Schema &>(generic_schema.resolve(type_map));
    auto column = Column<double>{{12.5}};
    const auto columns = std::array{ColumnView<>{column.view()}};
    auto memory = allocator{};
    auto source = make_frame_value(memory, schema, 1, columns);
    auto erased_source = ScalarValue<>{source};
    auto ticks = std::vector<std::optional<ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, erased_source.view());
    const auto inputs = std::array{testing::TestGraphInput{erased_source.runtime_type(), ticks}};

    const auto read = node([](In<TS<Frame<GenericQuote>>> input, Out<TS<double>> output) {
        const auto value = input.value_view().value();
        const auto bid = value.columns().bid.view<double>();
        require(bid.cell(0) == value.column("bid").view<double>().cell(0),
                "generic frame view field did not expose its resolved cell type");
        try {
            static_cast<void>(value.columns().bid.view<i64>());
            throw std::runtime_error{"generic frame view accepted the wrong cell type"};
        } catch (const std::invalid_argument &) {
        }
        output.set(bid.cell(0).value());
    });
    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "generic_quote_input", [&](wp<TS<Frame<SCHEMA>>> input) {
        return read(input);
    });
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});

    require(result.equals<double>({12.5}),
            "generic frame input did not read a scalar-variable field by name");
}

void test_inherited_frame_fields_and_views() {
    using namespace bg;
    using namespace bg::frame;
    using Reflection = bg::details::hierarchical_scalar_schema_reflection<InheritedFrame>;

    static_assert(Reflection::field_count == 3);
    static_assert(Reflection::field_name<0>() == "size");
    static_assert(Reflection::field_name<1>() == "bid");
    static_assert(Reflection::field_name<2>() == "ask");
    require(Reflection::field_index("bid") == 1,
            "inherited frame reflection did not preserve the shadowed field's logical position");

    auto immutable = Frame<InheritedFrame>{InheritedFrame<Column>{
        InheritedFrameBase<Column>{Column<i64>{{5, 6}}, Column<double>{{99.0, 100.0}}},
        Column<double>{{12.5, 23.0}}, Column<double>{{std::nullopt, 21.5}}}};
    auto immutable_copy = Frame<InheritedFrame>{immutable};
    auto immutable_view = FrameView<InheritedFrame>{immutable_copy};
    const auto immutable_fields = Reflection::fields(immutable_view.columns());
    require(immutable_view.size() == 2 && std::get<0>(immutable_fields).cell(1) == 6 &&
                std::get<1>(immutable_fields).cell(0) == 12.5 && !std::get<2>(immutable_fields).cell(0),
            "inherited scalar frame construction or copy exposed the hidden base field");

    auto memory = allocator{};
    auto source = NativeFrame<InheritedFrame>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first = source.add(
        InheritedFrame<Cell>{InheritedFrameBase<Cell>{Cell<i64>{5}, Cell<double>{99.0}},
                             Cell<double>{12.5}, Cell<double>{std::nullopt}},
        first_time, memory);
    const auto second = source.add(
        InheritedFrame<Cell>{InheritedFrameBase<Cell>{Cell<i64>{6}, Cell<double>{100.0}},
                             Cell<double>{23.0}, Cell<double>{21.5}},
        first_time, memory);
    require(source.cell<0>(first) == 5 && source.cell<1>(first) == 12.5 && !source.cell<2>(first) &&
                source.cell<0>(second) == 6 && source.cell<1>(second) == 23.0 && source.cell<2>(second) == 21.5,
            "inherited native frame rows lost base, direct, or null cells");

    const auto &series_type = frame_time_series_type<InheritedFrame>(TimeSeriesGuarantee::ordinary);
    const auto &native_runtime = static_cast<const FrameSeriesRuntimeType &>(
        frame_time_series_runtime_type<InheritedFrame>(series_type));
    auto scalar = Scalar<Frame<InheritedFrame>>::from(native_runtime.scalar_runtime_type(),
                                                       std::addressof(source.core()));
    auto stable = ScalarValue<Frame<InheritedFrame>>{scalar, memory};
    auto base_scalar = Scalar<Frame<InheritedFrameBase>>::from(native_runtime.scalar_runtime_type(),
                                                                std::addressof(source.core()));
    const auto base_view = base_scalar.value();
    require(base_view.columns().size.cell(0) == 5 && base_view.columns().bid.cell(0) == 12.5 &&
                base_view.schema().field_count() == 3 && !base_view.column("ask").view<double>().cell(0),
            "base scalar view did not expose its declared and retained derived fields");
    auto stable_base = ScalarValue<Frame<InheritedFrameBase>>{base_scalar, memory};

    auto derived_snapshot = ScalarValue<Frame<InheritedFrame>>{scalar, memory};
    const auto derived_snapshot_view = derived_snapshot.view();
    const auto base_annotation = Scalar<Frame<InheritedFrameBase>>::from(derived_snapshot_view);
    auto retained_base_snapshot = ScalarValue<Frame<InheritedFrameBase>>{base_annotation, memory};
    const auto *derived_snapshot_owner =
        frame::details::frame_snapshot_owner(bg::details::scalar_data(derived_snapshot));
    const auto *base_snapshot_owner =
        frame::details::frame_snapshot_owner(bg::details::scalar_data(retained_base_snapshot));
    require(derived_snapshot_owner != nullptr && base_snapshot_owner == derived_snapshot_owner &&
                retained_base_snapshot.view().value().schema().field_count() == 3,
            "base annotation did not retain the derived snapshot owner and schema");

    auto patch = InheritedFrame<PatchCell>{InheritedFrameBase<PatchCell>{PatchCell<i64>{}, PatchCell<double>{99.5}},
                                           PatchCell<double>{std::nullopt}, PatchCell<double>{15.5}};
    const auto second_time = first_time + EvaluationTime::duration{1};
    require(source.set(first, patch, second_time, memory), "inherited native frame rejected a row patch");
    require(source.cell<0>(first) == 5 && !source.cell<1>(first) && source.cell<2>(first) == 15.5 &&
                stable.view().value().columns().bid.cell(0) == 12.5,
            "inherited native frame patch or scalar snapshot used the shadowed base field");
    const auto stable_base_view = stable_base.view().value();
    require(stable_base_view.schema().field_count() == 3 &&
                stable_base_view.columns().bid.cell(0) == 12.5 &&
                !stable_base_view.column("ask").view<double>().cell(0),
            "base scalar copy lost its derived-only column after source mutation");
    require(source.changed_columns().contains(1) && source.changed_columns().contains(2),
            "inherited native frame patch did not publish logical column ordinals");

    auto input = native_frame_input(source);
    auto predicate = [&](row_id_t id) { return source.cell<1>(id).value_or(0.0) > 10.0; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);
    const auto predicate_index = input.field_index(util::symbol{"bid"});
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto bid_field = std::array{predicate_index};
    with_runtime_view_storage<InheritedFrame>(memory, [&](auto &filter, const auto &) {
        require(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function, second_time,
                            memory),
                "inherited frame filter did not bind its logical predicate field");
        require(!filter.selected_rows.contains(first) && filter.selected_rows.contains(second),
                "inherited frame filter selected rows using the shadowed base field");

        with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &runtime) {
            require(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, second_time, memory),
                    "inherited frame projection did not bind a direct field");
            auto projected = runtime.input_storage(std::addressof(projection));
            require(projected.generic_column(0).template view<double>().cell(1) == 23.0,
                    "inherited frame projection read the wrong direct field");

            const auto third_time = second_time + EvaluationTime::duration{1};
            const auto ask_field = std::array{input.field_index(util::symbol{"ask"})};
            require(projection.bind(input, ask_field, std::nullopt, nullptr, nullptr, third_time, memory),
                    "inherited frame projection did not rebind to another logical field");
            projected = runtime.input_storage(std::addressof(projection));
            require(projected.generic_column(0).template view<double>().cell(1) == 21.5,
                    "inherited frame projection rebind exposed a hidden base field");
        });

        with_runtime_view_storage<InheritedFrameBase>(memory, [&](auto &base_filter, const auto &runtime) {
            require(base_filter.bind(input, {}, 1, predicate_context, predicate_function, second_time, memory),
                    "base-schema filter did not map its declared fields to the derived input schema");
            const auto output = runtime.input_storage(std::addressof(base_filter));
            require(output.schema->field_count() == 2 && output.size == 1 &&
                        base_filter.selected_rows.contains(second) && !base_filter.selected_rows.contains(first) &&
                        output.generic_column(1).template view<double>().cell(0) == 23.0,
                    "base-schema filter did not retain its output field count or select the derived bid column");
        });

        const auto third_time = second_time + EvaluationTime::duration{1};
        require(source.erase(second, third_time, memory), "inherited native frame could not remove a row");
        input = native_frame_input(source);
        require(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function, third_time,
                            memory) && filter.removed_rows.contains(second),
                "inherited frame filter did not forward row removal");
    });
}

void test_scalar_frame_time_series_graph() {
    using namespace bg;
    using namespace bg::frame;

    using FrameValue = Frame<Quotes>;
    using Columns = Quotes<Column>;
    const auto first = FrameValue{Columns{.size = Column<i64>{{i64{5}}}, .bid = Column<double>{{10.5}},
                                          .ask = Column<double>{{std::nullopt}}}};
    const auto second = FrameValue{Columns{.size = Column<i64>{{i64{6}}}, .bid = Column<double>{{11.5}},
                                           .ask = Column<double>{{12.5}}}};
    const auto copy = bg::node([](In<TS<FrameValue>> input, Out<TS<FrameValue>> output) {
        const auto value = input.value();
        const auto scalar = Scalar<FrameValue>::from(frame::details::frame_view_scalar_runtime_type<Quotes>(),
                                                      std::addressof(value));
        output.set_value(scalar);
    });
    const auto graph = [&](wp<TS<FrameValue>> input) { return copy(input); };
    const auto result = testing::test_graph(graph, std::vector{first, second});
    require(result.size() == 2 && result[0] && result[1], "TS<Frame> graph lost scalar frame values");
    require(result[0]->view().value().columns().bid.cell(0) == 10.5 &&
                !result[0]->view().value().columns().ask.cell(0) &&
                result[1]->view().value().columns().size.cell(0) == 6,
            "TS<Frame> graph changed the scalar frame data");
}

struct frame_writer_state {
    row_id_t row{no_row};
};

struct borrowed_frame_observation {
    const TimeSeriesRuntimeType *runtime_type{};
    bool zero_copy_source{};
    bool zero_copy_column{};
    bool modified{};
    EvaluationTime last_updated{MIN_DT};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> size_modified;
    std::vector<row_id_t> bid_modified;
    std::vector<row_id_t> ask_modified;
    std::vector<std::optional<i64>> sizes;
    std::vector<std::optional<double>> bids;
    std::vector<std::optional<double>> asks;
};

struct borrowed_frame_sink {
    In<TSF<Quotes>, optional> input;

    void evaluate() const {
        const auto scalar = input.value_view();
        auto observation = borrowed_frame_observation{
            .runtime_type = std::addressof(input.runtime_type()),
            .zero_copy_source = input.input_data().data() == expected_data &&
                                scalar.data() == expected_data,
            .zero_copy_column = false,
            .modified = input.modified(),
            .last_updated = input.last_updated(),
        };
        auto frame = scalar.value();
        frame.columns().bid.batches([&](const auto &batch) {
            if (batch.values != nullptr && expected_bid_values != nullptr)
                observation.zero_copy_column = batch.values == expected_bid_values;
        });
        input.for_each_row([&](row_id_t id, std::size_t) {
            observation.rows.push_back(id);
            observation.sizes.push_back(input.cell<0>(id));
            observation.bids.push_back(input.cell<1>(id));
            observation.asks.push_back(input.cell<2>(id));
        });
        input.for_each_changed([&](row_id_t id) { observation.changed.push_back(id); });
        input.for_each_modified<0>([&](row_id_t id) { observation.size_modified.push_back(id); });
        input.for_each_modified<1>([&](row_id_t id) { observation.bid_modified.push_back(id); });
        input.for_each_modified<2>([&](row_id_t id) { observation.ask_modified.push_back(id); });
        observations.push_back(std::move(observation));
    }

    inline static const double *expected_bid_values{};
    inline static const void *expected_data{};
    inline static std::vector<borrowed_frame_observation> observations;
};

struct borrowed_frame_runner {
    In<TSF<Quotes>, optional> input;
    In<TSF<Quotes>, optional> source;

    void start(EvaluationContext *context) const {
        auto &parent = context->runtime_graph();
        evaluator = parent.allocation().new_object<RuntimeGraphEvaluator>();
        evaluator->bind_owner(parent, context->node_index());
        child = parent.allocation().new_object<RuntimeGraph>(*compiled_graph, parent.allocation(), *evaluator,
                                                             context->node_index());

        const auto data = input.input_data();
        const auto inputs = std::array{RuntimeGraphInput{std::addressof(data.last_updated_ref()), data.data(),
                                                         std::addressof(data.runtime_type()),
                                                         RuntimeGraphInputChange::unchanged}};
        auto child_context = context->child(*child, inputs.data());
        child->start(child_context, inputs.data());
    }

    void evaluate(EvaluationContext *context) const {
        const auto input_data = input.input_data();
        borrowed_frame_sink::expected_data = input_data.data();
        const auto source_value = source.value_view().value();
        source_value.columns().bid.batches([&](const auto &batch) {
            if (batch.values != nullptr)
                borrowed_frame_sink::expected_bid_values = batch.values;
        });

        auto change = RuntimeGraphInputChange::unchanged;
        if (input.became_invalid())
            change = RuntimeGraphInputChange::invalidation;
        else if (input.became_valid())
            change = RuntimeGraphInputChange::revalidation;
        else if (input.modified())
            change = RuntimeGraphInputChange::update;

        const auto inputs = std::array{RuntimeGraphInput{std::addressof(input_data.last_updated_ref()),
                                                         input_data.data(),
                                                         std::addressof(input_data.runtime_type()), change}};
        auto child_context = context->child(*child, inputs.data());
        child->evaluate(child_context, inputs.data());
    }

    void stop(EvaluationContext *context) const noexcept {
        if (child == nullptr)
            return;

        const auto data = input.input_data();
        const auto inputs = std::array{RuntimeGraphInput{std::addressof(data.last_updated_ref()), data.data(),
                                                         std::addressof(data.runtime_type()),
                                                         RuntimeGraphInputChange::unchanged}};
        auto child_context = context->child(*child, inputs.data());
        child->stop(child_context, inputs.data());
        child->finalize(child_context);
        context->runtime_graph().allocation().delete_object(child);
        child = nullptr;
        context->runtime_graph().allocation().delete_object(evaluator);
        evaluator = nullptr;
    }

    inline static const CompiledGraph *compiled_graph{};
    inline static RuntimeGraphEvaluator *evaluator{};
    inline static RuntimeGraph *child{};
};

CompiledGraph make_borrowed_frame_graph() {
    auto wiring = GraphWiring{};
    const auto input = wiring.borrowed_input(TSF<Quotes>::type());
    const auto sink = node<borrowed_frame_sink>();
    {
        auto context = WiringContext{wiring};
        sink(input);
    }
    wiring.finalize();
    return CompiledGraph{wiring};
}

struct frame_observation {
    bool valid{};
    bool became_valid{};
    bool became_invalid{};
    bool modified{};
    std::size_t size{};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> removed;
    std::vector<row_id_t> bid_modified;
};

struct column_observation {
    bool valid{};
    bool became_valid{};
    bool became_invalid{};
    bool modified{};
    bool zero_copy{};
    std::size_t size{};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> removed;
};

struct tsf_writer {
    In<TS<i64>> command;
    State<frame_writer_state> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->row = output.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                                 .ask = Cell<double>{std::nullopt}});
            return;
        case 2: {
            output.set(state->row, RowPatch<Quotes>{.bid = 11.5});
            return;
        }
        case 3: {
            auto patch = Quotes<PatchCell>{};
            patch.ask = 12.5;
            output.set(state->row, patch);
            return;
        }
        case 4:
            output.set(state->row, Quotes<PatchCell>{});
            return;
        case 5:
            require(output.erase(state->row), "TSF writer could not remove its row");
            return;
        case 6:
            output.invalidate();
            return;
        case 7:
            output.clear();
            return;
        case 8: {
            const auto temporary = output.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{8.5},
                                                            .ask = Cell<double>{std::nullopt}});
            require(output.erase(temporary), "TSF writer could not erase a new row");
            return;
        }
        default:
            throw std::runtime_error{"unknown TSF test command"};
        }
    }
};

struct inherited_tsf_writer {
    In<TS<i64>> command;
    State<frame_writer_state> state;
    Out<TSF<InheritedFrame>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->row = output.add(InheritedFrame<Cell>{
                InheritedFrameBase<Cell>{Cell<i64>{5}, Cell<double>{99.0}}, Cell<double>{12.5},
                Cell<double>{std::nullopt}});
            return;
        case 2: {
            auto patch = InheritedFrame<PatchCell>{InheritedFrameBase<PatchCell>{PatchCell<i64>{},
                                                                                 PatchCell<double>{99.5}},
                                                        PatchCell<double>{std::nullopt}, PatchCell<double>{15.5}};
            output.set(state->row, patch);
            return;
        }
        case 3:
            require(output.erase(state->row), "inherited TSF writer could not remove its row");
            return;
        default:
            throw std::runtime_error{"unknown inherited TSF test command"};
        }
    }
};

struct runtime_schema_tsf_writer_state {
    row_id_t first{no_row};
    row_id_t second{no_row};
    row_id_t generic{no_row};
    row_id_t generic_partial{no_row};
    row_id_t inherited{no_row};
};

struct runtime_schema_tsf_writer {
    In<TS<Frame<SCHEMA>>> schema_seed;
    In<TS<i64>> command;
    State<runtime_schema_tsf_writer_state> state;
    Out<TSF<SCHEMA>> output;

    void evaluate() const {
        require(output.schema().field_count() == 3 && output.schema().field(0).name == util::symbol{"size"} &&
                    output.schema().field(1).name == util::symbol{"bid"} &&
                    output.schema().field(2).name == util::symbol{"ask"},
                "runtime-schema TSF output did not expose its resolved schema");
        static_cast<void>(schema_seed);

        switch (*command) {
        case 0:
            return;
        case 1: {
            const auto size = i64{5};
            const auto bid = 10.5;
            const auto size_scalar = Scalar<i64>::from(size);
            const auto bid_scalar = Scalar<double>::from(bid);
            const auto row = std::array<std::optional<Scalar<>>, 3>{
                Scalar<>::from(size_scalar), Scalar<>::from(bid_scalar), std::nullopt};
            state->first = output.add(row);
            return;
        }
        case 2: {
            const auto bid = 11.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto patch = std::array{
                std::pair{util::symbol{"bid"},
                          std::optional<Scalar<>>{Scalar<>::from(typed_bid)}}};
            output.set(state->first, patch);
            return;
        }
        case 3:
            state->second = output.add(ReorderedQuotes<Cell>{.ask = Cell<double>{21.0}, .size = Cell<i64>{8},
                                                             .bid = Cell<double>{20.5}});
            return;
        case 4:
            output.set(state->second, RowPatch<ReorderedQuotes>{.bid = std::nullopt});
            return;
        case 5: {
            const auto wrong_count = std::array<std::optional<Scalar<>>, 1>{};
            auto rejected = false;
            try {
                static_cast<void>(output.add(wrong_count));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "runtime-schema TSF output accepted a row with the wrong field count");
            return;
        }
        case 6:
            output.set(state->first, std::span<const std::pair<util::symbol, std::optional<Scalar<>>>>{});
            return;
        case 7: {
            const auto patch = std::array{
                std::pair{util::symbol{"bid"}, std::optional<Scalar<>>{}}};
            output.set(state->first, patch);
            return;
        }
        case 8: {
            const auto wrong_size_value = 5.0;
            const auto bid_value = 12.5;
            const auto wrong_size = Scalar<double>::from(wrong_size_value);
            const auto bid = Scalar<double>::from(bid_value);
            const auto wrong_type = std::array<std::optional<Scalar<>>, 3>{
                Scalar<>::from(wrong_size), Scalar<>::from(bid), std::nullopt};
            auto type_rejected = false;
            try {
                static_cast<void>(output.add(wrong_type));
            } catch (const std::invalid_argument &) {
                type_rejected = true;
            }
            auto name_rejected = false;
            try {
                const auto unknown_name = std::array{
                    std::pair{util::symbol{"unknown"}, std::optional<Scalar<>>{}}};
                output.set(state->first, unknown_name);
            } catch (const std::out_of_range &) {
                name_rejected = true;
            }
            auto duplicate_rejected = false;
            try {
                const auto generic_bid = Scalar<>::from(bid);
                const auto duplicate = std::array{
                    std::pair{util::symbol{"bid"}, std::optional<Scalar<>>{generic_bid}},
                    std::pair{util::symbol{"bid"}, std::optional<Scalar<>>{}},
                };
                output.set(state->first, duplicate);
            } catch (const std::invalid_argument &) {
                duplicate_rejected = true;
            }
            require(type_rejected && name_rejected && duplicate_rejected,
                    "runtime-schema TSF output accepted a wrong type, unknown name, or duplicate patch field");
            return;
        }
        case 9:
            static_cast<void>(output.add(RowPatch<Quotes>{.bid = 30.5}));
            return;
        case 10:
            static_cast<void>(output.add(RowPatch<Quotes>{}));
            return;
        case 11: {
            const auto bid = 40.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            state->generic = output.add(MixedGenericQuote<Cell>{
                .size = Cell<i64>{9}, .bid = Cell<SCALAR>{generic_bid}, .ask = Cell<double>{41.5}});
            return;
        }
        case 12: {
            const auto bid = 42.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            output.set(state->generic, RowPatch<MixedGenericQuote>{.bid = PatchCell<SCALAR>{generic_bid}});
            return;
        }
        case 13:
            output.set(state->generic, RowPatch<MixedGenericQuote>{.bid = std::nullopt});
            return;
        case 14: {
            const auto bid = 50.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            state->generic_partial = output.add(RowPatch<MixedGenericQuote>{
                .bid = PatchCell<SCALAR>{generic_bid}});
            return;
        }
        case 15:
            static_cast<void>(output.add(RowPatch<MixedGenericQuote>{}));
            return;
        case 16: {
            const auto wrong_bid = i64{60};
            const auto typed_bid = Scalar<i64>::from(wrong_bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto rejected = false;
            try {
                static_cast<void>(output.add(MixedGenericQuote<Cell>{
                    .size = Cell<i64>{10}, .bid = Cell<SCALAR>{generic_bid}, .ask = Cell<double>{61.5}}));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "runtime-schema TSF writer accepted a generic cell with the wrong runtime type");
            return;
        }
        case 17: {
            const auto bid = 62.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto rejected = false;
            try {
                output.set(state->generic, RowPatch<WrongMixedGenericQuote>{
                    .bid = PatchCell<SCALAR>{generic_bid}});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "generic patch skipped validation of an omitted concrete source field");
            return;
        }
        case 18:
            output.set(state->generic_partial, RowPatch<MixedGenericQuote>{});
            return;
        case 19: {
            const auto bid = 65.5;
            const auto ask = 66.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto typed_ask = Scalar<double>::from(ask);
            const auto generic_bid = Scalar<>::from(typed_bid);
            const auto generic_ask = Scalar<>::from(typed_ask);
            auto rejected = false;
            try {
                static_cast<void>(output.add(RepeatedGenericQuote<Cell>{
                    .size = Cell<SCALAR>{std::nullopt}, .bid = Cell<SCALAR>{generic_bid},
                    .ask = Cell<SCALAR>{generic_ask}}));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "repeated generic field variable matched different output field types");
            return;
        }
        case 20: {
            auto rejected = false;
            try {
                output.set(state->generic, RowPatch<ConstrainedGenericQuote>{.ask = PatchCell<double>{70.5}});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "constrained generic field matched an incompatible output field type");
            return;
        }
        case 21: {
            const auto wrong_bid = i64{75};
            const auto typed_bid = Scalar<i64>::from(wrong_bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto rejected = false;
            try {
                output.set(state->generic, RowPatch<MixedGenericQuote>{
                    .size = PatchCell<i64>{100}, .bid = PatchCell<SCALAR>{generic_bid}});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "generic patch accepted a wrong later cell after decoding an earlier valid cell");
            return;
        }
        case 22: {
            const auto bid = 71.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            state->inherited = output.add(InheritedGenericQuote<Cell>{
                InheritedGenericQuoteBase<Cell>{Cell<i64>{70}, Cell<double>{99.0}},
                Cell<SCALAR>{generic_bid}, Cell<double>{72.5}});
            return;
        }
        case 23: {
            const auto bid = 73.5;
            const auto typed_bid = Scalar<double>::from(bid);
            const auto generic_bid = Scalar<>::from(typed_bid);
            auto patch = InheritedGenericQuote<PatchCell>{
                InheritedGenericQuoteBase<PatchCell>{PatchCell<i64>{}, PatchCell<double>{98.5}},
                PatchCell<SCALAR>{generic_bid}, PatchCell<double>{}};
            output.set(state->inherited, patch);
            return;
        }
        default:
            throw std::runtime_error{"unknown runtime-schema TSF test command"};
        }
    }
};

struct tsf_switchable_projection {
    In<TSF<Quotes>> input;
    In<TS<i64>> selection;
    Out<TSF<BidOnly>, view> output;

    void evaluate() const {
        if (*selection == 0)
            output.bind_projection(input, std::array<std::size_t, 1>{1});
        else
            output.bind_projection(input, columns<"ask">());
    }
};

struct filter_writer_state {
    row_id_t first{no_row};
    row_id_t first_identity{no_row};
    row_id_t second{no_row};
};

struct filter_writer {
    In<TS<i64>> command;
    State<filter_writer_state> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->first = output.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                                   .ask = Cell<double>{10.0}});
            state->first_identity = state->first;
            return;
        case 2:
            state->second = output.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                                    .ask = Cell<double>{20.0}});
            return;
        case 3:
            output.set(state->first, Quotes<PatchCell>{.bid = 11.5});
            return;
        case 4:
            output.set(state->first, Quotes<PatchCell>{.size = 3});
            return;
        case 5:
            output.set(state->first, Quotes<PatchCell>{.size = 8});
            return;
        case 6:
            require(output.erase(state->first), "filter writer could not remove its first row");
            return;
        case 7:
            state->first = output.add(Quotes<Cell>{.size = Cell<i64>{9}, .bid = Cell<double>{12.5},
                                                   .ask = Cell<double>{12.0}});
            require(state->first == state->first_identity, "filter writer did not reuse a retired row ID");
            return;
        case 8:
            output.invalidate();
            return;
        case 9:
            output.clear();
            return;
        case 10:
            output.set(state->second, Quotes<PatchCell>{.bid = 14.0});
            return;
        case 11:
            output.set(state->second, Quotes<PatchCell>{.bid = 22.0});
            return;
        case 12:
            static_cast<void>(output.add(Quotes<Cell>{.size = Cell<i64>{10}, .bid = Cell<double>{std::nullopt},
                                                       .ask = Cell<double>{std::nullopt}}));
            return;
        case 13:
            static_cast<void>(output.add(Quotes<Cell>{.size = Cell<i64>{11}, .bid = Cell<double>{30.0},
                                                       .ask = Cell<double>{std::nullopt}}));
            return;
        default:
            throw std::runtime_error{"unknown filter writer command"};
        }
    }
};

struct wide_column_writer_state {
    std::array<row_id_t, 257> rows{};
};

struct wide_column_writer {
    In<TS<i64>> command;
    State<wide_column_writer_state> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        if (*command == 0) {
            for (auto index = std::size_t{}; index != 257; ++index) {
                const auto bid = index == 126 || index == 128 ? Cell<double>{std::nullopt}
                                                               : Cell<double>{static_cast<double>(index * 10)};
                const auto ask = index == 127 || index == 129 ? Cell<double>{std::nullopt}
                                                               : Cell<double>{static_cast<double>(index) + 0.5};
                state->rows[index] = output.add(Quotes<Cell>{.size = Cell<i64>{static_cast<i64>(index)},
                                                             .bid = bid, .ask = ask});
            }
            return;
        }
        if (*command == 1) {
            output.set(state->rows[128], Quotes<PatchCell>{.bid = 1280.0});
            return;
        }
        throw std::runtime_error{"unknown wide column writer command"};
    }
};

void test_double_column_addition_updates_and_nulls() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool became_valid{};
        bool became_invalid{};
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> values;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };

    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .became_valid = input.became_valid(),
                                  .became_invalid = input.became_invalid()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back(input.cell(id));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        return observe(operators::add(select_column<"bid">(frame), select_column<"ask">(frame)), tick);
    };

    const auto commands = std::array<i64, 10>{0, 1, 2, 10, 3, 6, 7, 8, 9, 1};
    const auto ticks = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "double column addition observer missed a cycle");
    require(observations[0].valid && observations[0].rows.empty(),
            "double column addition did not materialize an empty row domain");
    require(observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].values == std::vector<std::optional<double>>{20.5} &&
                observations[1].changed == std::vector<row_id_t>{0},
            "double column addition did not compute the first row");
    require(observations[2].rows == std::vector<row_id_t>{0, 1} &&
                observations[2].values == std::vector<std::optional<double>>{20.5, 40.5} &&
                observations[2].changed == std::vector<row_id_t>{1},
            "double column addition did not compute an added row");
    require(observations[3].values == std::vector<std::optional<double>>{20.5, 34.0} &&
                observations[3].changed == std::vector<row_id_t>{1},
            "double column addition did not propagate a sparse operand update");
    require(observations[4].values == std::vector<std::optional<double>>{21.5, 34.0} &&
                observations[4].changed == std::vector<row_id_t>{0},
            "double column addition did not update the changed row alone");
    require(observations[5].rows == std::vector<row_id_t>{1} &&
                observations[5].removed == std::vector<row_id_t>{0},
            "double column addition did not propagate row removal");
    require(observations[6].rows == std::vector<row_id_t>{0, 1} &&
                observations[6].values == std::vector<std::optional<double>>{24.5, 34.0} &&
                observations[6].changed == std::vector<row_id_t>{0},
            "double column addition did not recompute after row identity reuse");
    require(!observations[7].valid && observations[7].became_invalid,
            "double column addition did not invalidate its output with its inputs");
    require(observations[8].valid && observations[8].became_valid && observations[8].rows.empty(),
            "double column addition did not revalidate its output after an empty source bind");
    require(observations[9].rows == std::vector<row_id_t>{0} &&
                observations[9].values == std::vector<std::optional<double>>{20.5} &&
                observations[9].changed == std::vector<row_id_t>{0},
            "double column addition did not rebuild after revalidation");
}

void test_double_column_addition_crosses_scratch_batch_boundaries() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::vector<std::optional<double>> values;
        std::vector<row_id_t> changed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<wide_column_writer>();
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) { result.values.push_back(input.cell(id)); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto sum = operators::add(select_column<"bid">(frame), select_column<"ask">(frame));
        return observe(operators::add(sum, sum), tick);
    };

    const auto commands = std::array<i64, 2>{0, 1};
    const auto ticks = std::array<i64, 2>{0, 1};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "wide column addition observer missed a cycle");
    require(observations[0].values.size() == 257 && observations[0].values[125] == 2751.0 &&
                !observations[0].values[126] && !observations[0].values[127] &&
                !observations[0].values[128] && !observations[0].values[129] &&
                observations[0].values[130] == 2861.0 && observations[0].values[256] == 5633.0,
            "column addition mishandled values or nulls across scratch batch boundaries");
    require(observations[1].values.size() == 257 && observations[1].values[128] == 2817.0 &&
                observations[1].changed == std::vector<row_id_t>{128},
            "column addition did not propagate one sparse update across a scratch batch boundary");
}

void test_double_column_subtraction_order_nulls_and_updates() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::array<std::vector<std::optional<double>>, 4> values;
        std::array<std::vector<row_id_t>, 4> changed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<wide_column_writer>();
    const auto observe = node([&](In<TSC<double>> column_minus_parameter, In<TSC<double>> parameter_minus_column,
                                  In<TSC<double>> constant_minus_column, In<TSC<double>> column_minus_constant,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        const auto collect = [&](const auto &input, auto &values, auto &changed) {
            input.for_each_row([&](row_id_t id, std::size_t) { values.push_back(input.cell(id)); });
            input.for_each_changed([&](row_id_t id) { changed.push_back(id); });
        };
        collect(column_minus_parameter, result.values[0], result.changed[0]);
        collect(parameter_minus_column, result.values[1], result.changed[1]);
        collect(constant_minus_column, result.values[2], result.changed[2]);
        collect(column_minus_constant, result.values[3], result.changed[3]);
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<double>> parameter, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto difference = operators::sub(select_column<"bid">(frame), select_column<"ask">(frame));
        return observe(operators::sub(difference, parameter), operators::sub(parameter, difference),
                       operators::sub(3.0, difference), operators::sub(difference, 3.0), tick);
    };

    const auto commands = std::array<std::optional<i64>, 4>{0, 1, std::nullopt, std::nullopt};
    const auto parameters = std::array<std::optional<double>, 4>{2.0, std::nullopt, std::nullopt, 3.0};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    static_cast<void>(testing::test_graph(graph, commands, parameters, ticks));

    require(observations.size() == commands.size(), "column subtraction observer missed a cycle");
    const auto &initial = observations[0].values;
    require(initial[0].size() == 257 && initial[0][0] == -2.5 && initial[1][0] == 2.5 &&
                initial[2][0] == 3.5 && initial[3][0] == -3.5 && initial[0][130] == 1167.5 &&
                initial[1][130] == -1167.5 && initial[0][256] == 2301.5 && initial[1][256] == -2301.5,
            "column subtraction did not preserve operand order across block boundaries");
    for (auto row = std::size_t{126}; row != 130; ++row)
        for (const auto &values : initial)
            require(!values[row], "column subtraction did not propagate a null operand");

    const auto &sparse = observations[1];
    require(sparse.values[0][128] == 1149.5 && sparse.values[1][128] == -1149.5 &&
                sparse.values[2][128] == -1148.5 && sparse.values[3][128] == 1148.5 &&
                std::ranges::all_of(sparse.changed, [](const auto &rows) {
                    return rows == std::vector<row_id_t>{128};
                }),
            "column subtraction did not update one changed row sparsely");
    require(std::ranges::all_of(observations[2].changed, [](const auto &rows) { return rows.empty(); }),
            "unchanged subtraction inputs published output changes");

    const auto &parameter_update = observations[3];
    require(parameter_update.values[0][0] == -3.5 && parameter_update.values[1][0] == 3.5 &&
                parameter_update.values[0][128] == 1148.5 && parameter_update.values[1][128] == -1148.5 &&
                parameter_update.changed[0].size() == 257 && parameter_update.changed[1].size() == 257 &&
                parameter_update.changed[2].empty() && parameter_update.changed[3].empty(),
            "column subtraction did not recompute broadcast parameter outputs on a parameter tick");
}

void test_double_column_addition_accepts_projected_columns_with_shared_selection() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> values;
        std::vector<row_id_t> changed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back(input.cell(id));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto filtered = filter_greater_than<"size">(frame, i64{4});
        const auto projected = project<BidOnly, "bid">(filtered);
        const auto sum = operators::add(select_column<"price">(projected), select_column<"ask">(filtered));
        return observe(operators::add(sum, sum), tick);
    };

    const auto commands = std::array<i64, 4>{0, 1, 2, 3};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size() && observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].values == std::vector<std::optional<double>>{41.0} &&
                observations[2].rows == std::vector<row_id_t>{0, 1} &&
                observations[2].values == std::vector<std::optional<double>>{41.0, 81.0} &&
                observations[3].values == std::vector<std::optional<double>>{43.0, 81.0} &&
                observations[3].changed == std::vector<row_id_t>{0},
            "column addition did not reuse a computed expression over one filtered selection");
}

void test_double_column_addition_rejects_different_domains_and_membership() {
    using namespace bg;
    using namespace bg::frame;

    const auto observe = node([](In<TSC<double>>, In<TS<i64>> tick, Out<TS<i64>> output) { output.set(*tick); });
    const auto first_writer = node<filter_writer>();
    const auto second_writer = node<filter_writer>();
    const auto domain_graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto first = first_writer(command);
        const auto second = second_writer(command);
        return observe(operators::add(select_column<"bid">(first), select_column<"bid">(second)), tick);
    };

    auto domain_rejected = false;
    try {
        static_cast<void>(testing::test_graph(domain_graph, std::array<i64, 1>{1}, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        domain_rejected = true;
    }
    require(domain_rejected, "column addition accepted inputs from different row domains");

    const auto write = node<filter_writer>();
    const auto membership_graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto filtered = filter_greater_than<"size">(frame, i64{4});
        return observe(operators::add(select_column<"bid">(frame), select_column<"bid">(filtered)), tick);
    };
    auto membership_rejected = false;
    try {
        static_cast<void>(testing::test_graph(membership_graph, std::array<i64, 1>{1}, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        membership_rejected = true;
    }
    require(membership_rejected, "column addition accepted a native column and filter with equal contents");

    const auto distinct_filters_write = node<filter_writer>();
    const auto distinct_filters_graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = distinct_filters_write(command);
        const auto first = filter_greater_than<"size">(frame, i64{4});
        const auto second = filter_greater_than<"size">(frame, i64{3});
        return observe(operators::add(select_column<"bid">(first), select_column<"ask">(second)), tick);
    };
    auto distinct_filters_rejected = false;
    try {
        static_cast<void>(testing::test_graph(distinct_filters_graph, std::array<i64, 1>{1},
                                              std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        distinct_filters_rejected = true;
    }
    require(distinct_filters_rejected,
            "column addition accepted distinct filters with equal contents");
}

void test_double_column_addition_rejects_mixed_integer_columns() {
    using namespace bg;
    using namespace bg::frame;

    auto rejected = false;
    try {
        auto wiring = GraphWiring{};
        auto context = WiringContext{wiring};
        const auto integer_column = wiring.borrowed_input(TSC<i64>::type());
        const auto double_column = wiring.borrowed_input(TSC<double>::type());
        static_cast<void>(operators::add(integer_column, double_column));
    } catch (const WiringError &) {
        rejected = true;
    }
    require(rejected, "column addition promoted a mixed i64 and double operand pair");
}

void test_double_column_addition_leaves_scalar_addition_to_bg() {
    using namespace bg;

    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto lhs = wiring.borrowed_input(TS<double>::type());
    const auto rhs = wiring.borrowed_input(TS<double>::type());
    const auto result = operators::add(lhs, rhs);

    require(std::addressof(result.type()) == std::addressof(TS<double>::type()),
            "frame column addition claimed an ordinary TS<double> addition");
}

void test_double_column_addition_broadcasts_owned_scalar_values() {
    using namespace bg;

    struct observation {
        std::vector<std::optional<double>> column_plus_scalar;
        std::vector<std::optional<double>> scalar_plus_column;
        std::vector<row_id_t> column_plus_scalar_changed;
        std::vector<row_id_t> scalar_plus_column_changed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>> lhs, In<TSC<double>> rhs, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto result = observation{};
        lhs.for_each_row([&](row_id_t id, std::size_t) { result.column_plus_scalar.push_back(lhs.cell(id)); });
        rhs.for_each_row([&](row_id_t id, std::size_t) { result.scalar_plus_column.push_back(rhs.cell(id)); });
        lhs.for_each_changed([&](row_id_t id) { result.column_plus_scalar_changed.push_back(id); });
        rhs.for_each_changed([&](row_id_t id) { result.scalar_plus_column_changed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto column = select_column<"bid">(frame);
        return observe(operators::add(column, 2.0), operators::add(2.0, column), tick);
    };

    const auto commands = std::array<i64, 3>{1, 2, 10};
    const auto ticks = std::array<i64, 3>{0, 1, 2};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "scalar column addition observer missed a cycle");
    require(observations[0].column_plus_scalar == std::vector<std::optional<double>>{12.5} &&
                observations[0].scalar_plus_column == std::vector<std::optional<double>>{12.5},
            "column scalar addition did not retain a temporary scalar value");
    require(observations[1].column_plus_scalar == std::vector<std::optional<double>>{12.5, 22.5} &&
                observations[1].scalar_plus_column == std::vector<std::optional<double>>{12.5, 22.5} &&
                observations[1].column_plus_scalar_changed == std::vector<row_id_t>{1} &&
                observations[1].scalar_plus_column_changed == std::vector<row_id_t>{1},
            "column scalar addition did not sparsely update a new row");
    require(observations[2].column_plus_scalar == std::vector<std::optional<double>>{12.5, 16.0} &&
                observations[2].scalar_plus_column == std::vector<std::optional<double>>{12.5, 16.0} &&
                observations[2].column_plus_scalar_changed == std::vector<row_id_t>{1} &&
                observations[2].scalar_plus_column_changed == std::vector<row_id_t>{1},
            "column scalar addition did not update a modified row");
}

void test_double_column_addition_parameter_updates_recompute_rows() {
    using namespace bg;

    struct observation {
        std::vector<std::optional<double>> values;
        std::vector<row_id_t> changed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) { result.values.push_back(input.cell(id)); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<double>> parameter, wp<TS<i64>> tick) {
        const auto frame = write(command);
        return observe(operators::add(select_column<"bid">(frame), parameter), tick);
    };

    const auto commands = std::array<std::optional<i64>, 5>{1, 2, std::nullopt, 10, std::nullopt};
    const auto parameters = std::array<std::optional<double>, 5>{2.0, std::nullopt, 5.0, 7.0, std::nullopt};
    const auto ticks = std::array<i64, 5>{0, 1, 2, 3, 4};
    static_cast<void>(testing::test_graph(graph, commands, parameters, ticks));

    require(observations.size() == commands.size(), "parameterized column addition observer missed a cycle");
    require(observations[0].values == std::vector<std::optional<double>>{12.5} &&
                observations[0].changed == std::vector<row_id_t>{0},
            "parameterized column addition did not compute the first row");
    require(observations[1].values == std::vector<std::optional<double>>{12.5, 22.5} &&
                observations[1].changed == std::vector<row_id_t>{1},
            "column-only update did not recompute only the added row");
    require(observations[2].values == std::vector<std::optional<double>>{15.5, 25.5} &&
                observations[2].changed == std::vector<row_id_t>{0, 1},
            "parameter-only update did not recompute every visible row");
    require(observations[3].values == std::vector<std::optional<double>>{17.5, 21.0} &&
                observations[3].changed == std::vector<row_id_t>{0, 1},
            "simultaneous parameter and column updates did not recompute every visible row");
    require(observations[4].values == std::vector<std::optional<double>>{17.5, 21.0} &&
                observations[4].changed.empty(),
            "an unchanged parameter and column set produced output changes");
}

struct invalidating_double_source {
    In<TS<double>> value;
    In<TS<bool>> invalidate;
    Out<TS<double>> output;

    void evaluate() const {
        if (*invalidate) {
            output.invalidate();
            return;
        }
        output.copy(value);
    }
};

void test_double_column_addition_parameter_invalidation_and_revalidation() {
    using namespace bg;

    struct observation {
        bool valid{};
        bool became_valid{};
        bool became_invalid{};
        std::vector<std::optional<double>> values;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto parameter_source = node<invalidating_double_source>();
    const auto observe = node([&](In<TSC<double>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .became_valid = input.became_valid(),
                                  .became_invalid = input.became_invalid()};
        input.for_each_row([&](row_id_t id, std::size_t) { result.values.push_back(input.cell(id)); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<double>> value, wp<TS<bool>> invalidate,
                           wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto parameter = parameter_source(value, invalidate);
        const auto stable_parameter = operators::const_(10.0).as<TS<double>>();
        const auto first_sum = operators::add(select_column<"bid">(frame), parameter);
        return observe(operators::add(first_sum, stable_parameter), tick);
    };

    const auto commands = std::array<std::optional<i64>, 4>{1, std::nullopt, std::nullopt, std::nullopt};
    const auto values = std::array<double, 4>{2.0, 3.0, 3.0, 4.0};
    const auto invalidations = std::array<bool, 4>{false, true, false, false};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    static_cast<void>(testing::test_graph(graph, commands, values, invalidations, ticks));

    require(observations.size() == commands.size(), "parameter validity observer missed a cycle");
    require(observations[0].valid && observations[0].values == std::vector<std::optional<double>>{22.5},
            "parameterized column addition did not become valid with two scalar inputs");
    require(!observations[1].valid && observations[1].became_invalid,
            "parameter invalidation did not invalidate column addition output");
    require(observations[2].valid && observations[2].became_valid &&
                observations[2].values == std::vector<std::optional<double>>{23.5},
            "parameter revalidation did not rebuild column addition output");
    require(observations[3].valid && observations[3].values == std::vector<std::optional<double>>{24.5},
            "column addition did not update after parameter revalidation");
}

void test_double_column_addition_deduplicates_repeated_parameter_inputs() {
    using namespace bg;

    const auto consume = node([](In<TSC<double>>) {});
    const auto frame_source = node([](Out<TSF<Quotes>> output) {
        static_cast<void>(output.add(Quotes<Cell>{.size = Cell<i64>{1}, .bid = Cell<double>{10.0},
                                                  .ask = Cell<double>{std::nullopt}}));
    });
    const auto parameter_source = node([](Out<TS<double>> output) { output.set(2.0); });
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto column = select_column<"bid">(frame_source());
        const auto parameter = parameter_source();
        const auto first = operators::add(column, parameter);
        consume(operators::add(first, parameter));
    }
    wiring.finalize();

    auto parameter_snapshot_size = std::size_t{};
    for (const auto &candidate : wiring.nodes()) {
        const auto &signature = candidate.signature();
        if (signature.parameters.size() != 1 || signature.parameters[0].name.text() != "parameters" ||
            signature.output == nullptr)
            continue;
        const auto *bundle = type_cast<TimeSeriesBundleType>(signature.parameters[0].type);
        if (bundle != nullptr)
            parameter_snapshot_size = bundle->item_count();
    }
    require(parameter_snapshot_size == 1, "repeated scalar expression inputs were not deduplicated");
}

void test_bound_column_expression_owns_captures_and_binds_parameters() {
    using namespace bg;
    using namespace bg::literals;

    struct observation {
        std::vector<std::optional<double>> named_parameter;
        std::vector<std::optional<double>> captured_time_series;
        std::vector<std::optional<double>> captured_scalar;
        std::vector<std::optional<double>> captured_time_series_minus_parameter;
        std::vector<std::optional<double>> captured_parameter_minus_time_series;
        std::vector<std::optional<double>> captured_scalar_minus_column;
        std::vector<std::optional<double>> captured_column_minus_scalar;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>> named, In<TSC<double>> captured_series,
                                  In<TSC<double>> captured_scalar,
                                  In<TSC<double>> captured_time_series_minus_parameter,
                                  In<TSC<double>> captured_parameter_minus_time_series,
                                  In<TSC<double>> captured_scalar_minus_column,
                                  In<TSC<double>> captured_column_minus_scalar, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto result = observation{};
        named.for_each_row([&](row_id_t id, std::size_t) { result.named_parameter.push_back(named.cell(id)); });
        captured_series.for_each_row(
            [&](row_id_t id, std::size_t) { result.captured_time_series.push_back(captured_series.cell(id)); });
        captured_scalar.for_each_row(
            [&](row_id_t id, std::size_t) { result.captured_scalar.push_back(captured_scalar.cell(id)); });
        captured_time_series_minus_parameter.for_each_row([&](row_id_t id, std::size_t) {
            result.captured_time_series_minus_parameter.push_back(captured_time_series_minus_parameter.cell(id));
        });
        captured_parameter_minus_time_series.for_each_row([&](row_id_t id, std::size_t) {
            result.captured_parameter_minus_time_series.push_back(captured_parameter_minus_time_series.cell(id));
        });
        captured_scalar_minus_column.for_each_row([&](row_id_t id, std::size_t) {
            result.captured_scalar_minus_column.push_back(captured_scalar_minus_column.cell(id));
        });
        captured_column_minus_scalar.for_each_row([&](row_id_t id, std::size_t) {
            result.captured_column_minus_scalar.push_back(captured_column_minus_scalar.cell(id));
        });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<double>> parameter, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto named = wp<TSC<double>>{
            bind_expression(frame, col("bid") + param("offset"), "offset"_n = parameter)};
        const auto direct_capture = wp<TSC<double>>{bind_expression(frame, col("ask") + parameter)};
        const auto scalar_capture = [&] {
            const auto scalar = ScalarValue<double>{3.0};
            return col("bid") + col("ask") - col("ask") + capture(scalar);
        }();
        const auto captured_scalar = wp<TSC<double>>{bind_expression(frame, scalar_capture)};
        const auto time_series_minus_parameter =
            wp<TSC<double>>{bind_expression(frame, col("ask") - parameter)};
        const auto parameter_minus_time_series =
            wp<TSC<double>>{bind_expression(frame, parameter - col("ask"))};
        const auto scalar_minus_column = wp<TSC<double>>{bind_expression(frame, 20.0 - col("ask"))};
        const auto column_minus_scalar = wp<TSC<double>>{bind_expression(frame, col("ask") - 3.0)};
        return observe(named, direct_capture, captured_scalar, time_series_minus_parameter,
                       parameter_minus_time_series, scalar_minus_column, column_minus_scalar, tick);
    };

    const auto commands = std::array<std::optional<i64>, 3>{1, 2, std::nullopt};
    const auto parameters = std::array<std::optional<double>, 3>{2.0, std::nullopt, 5.0};
    const auto ticks = std::array<i64, 3>{0, 1, 2};
    static_cast<void>(testing::test_graph(graph, commands, parameters, ticks));

    require(observations.size() == commands.size(), "bound column expression observer missed a cycle");
    require(observations[0].named_parameter == std::vector<std::optional<double>>{12.5} &&
                observations[0].captured_time_series == std::vector<std::optional<double>>{12.0} &&
                observations[0].captured_scalar == std::vector<std::optional<double>>{13.5} &&
                observations[0].captured_time_series_minus_parameter == std::vector<std::optional<double>>{8.0} &&
                observations[0].captured_parameter_minus_time_series == std::vector<std::optional<double>>{-8.0} &&
                observations[0].captured_scalar_minus_column == std::vector<std::optional<double>>{10.0} &&
                observations[0].captured_column_minus_scalar == std::vector<std::optional<double>>{7.0},
            "bound column expression did not read its parameter and owned captures");
    require(observations[1].named_parameter == std::vector<std::optional<double>>{12.5, 22.5} &&
                observations[1].captured_time_series == std::vector<std::optional<double>>{12.0, 22.0} &&
                observations[1].captured_scalar == std::vector<std::optional<double>>{13.5, 23.5} &&
                observations[1].captured_time_series_minus_parameter == std::vector<std::optional<double>>{8.0, 18.0} &&
                observations[1].captured_parameter_minus_time_series ==
                    std::vector<std::optional<double>>{-8.0, -18.0} &&
                observations[1].captured_scalar_minus_column == std::vector<std::optional<double>>{10.0, 0.0} &&
                observations[1].captured_column_minus_scalar == std::vector<std::optional<double>>{7.0, 17.0},
            "bound column expression did not update captured inputs for an added row");
    require(observations[2].named_parameter == std::vector<std::optional<double>>{15.5, 25.5} &&
                observations[2].captured_time_series == std::vector<std::optional<double>>{15.0, 25.0} &&
                observations[2].captured_scalar == std::vector<std::optional<double>>{13.5, 23.5} &&
                observations[2].captured_time_series_minus_parameter ==
                    std::vector<std::optional<double>>{5.0, 15.0} &&
                observations[2].captured_parameter_minus_time_series ==
                    std::vector<std::optional<double>>{-5.0, -15.0} &&
                observations[2].captured_scalar_minus_column == std::vector<std::optional<double>>{10.0, 0.0} &&
                observations[2].captured_column_minus_scalar == std::vector<std::optional<double>>{7.0, 17.0},
            "bound column expression did not recompute named parameters or retain a temporary scalar capture");
}

std::size_t counted_column_expression_calls{};
std::size_t counted_column_expression_rows{};
std::size_t double_column_materializer_rows{};
std::size_t mixed_i64_parameter_calls{};
std::size_t mixed_double_parameter_calls{};

void add_i64_parameter_to_double(std::span<const row_id_t> ids, const ColumnOperandBatch<i64> &column,
                                 const ColumnOperandBatch<i64> &parameter,
                                 MutableColumnBatch<double> output) {
    ++mixed_i64_parameter_calls;
    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        const auto valid = column.is_valid(row) && parameter.is_valid(row);
        output.set_valid(row, valid);
        if (valid)
            output.values[row] = static_cast<double>(column.value(row) + parameter.value(row));
    }
}

void add_double_parameter(std::span<const row_id_t> ids, const ColumnOperandBatch<double> &column,
                          const ColumnOperandBatch<double> &parameter, MutableColumnBatch<double> output) {
    ++mixed_double_parameter_calls;
    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        const auto valid = column.is_valid(row) && parameter.is_valid(row);
        output.set_valid(row, valid);
        if (valid)
            output.values[row] = column.value(row) + parameter.value(row);
    }
}

void test_mixed_column_expression_fuses_and_collects_parameters() {
    using namespace bg;

    register_column_block_function<double, i64, i64>();
    register_column_block_function<double, double, double>();

    auto observations = std::vector<std::vector<std::optional<double>>>{};
    const auto write = node<filter_writer>();
    const auto make_result = [](auto frame, auto integer_parameter, auto double_parameter) {
        const auto integer_expression = make_column_expression(
            ColumnBlockFunction<double, i64, i64>{&add_i64_parameter_to_double},
            select_column<"size">(frame), integer_parameter);
        const auto double_expression = make_column_expression(
            ColumnBlockFunction<double, double, double>{&add_double_parameter},
            select_column<"bid">(frame), double_parameter);
        return operators::add(integer_expression, double_expression);
    };
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto values = std::vector<std::optional<double>>{};
        input.for_each_row([&](row_id_t id, std::size_t) { values.push_back(input.cell(id)); });
        observations.push_back(std::move(values));
        output.set(*tick);
    });
    const auto command_source = node([](Out<TS<i64>>) {});
    const auto integer_source = node([](Out<TS<i64>>) {});
    const auto double_source = node([](Out<TS<double>>) {});
    const auto consume = node([](In<TSC<double>>) {});

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto frame = write(command_source());
        consume(make_result(frame, integer_source(), double_source()));
    }
    wiring.finalize();
    const auto &nodes = wiring.nodes();
    const auto expression_nodes = static_cast<std::size_t>(std::ranges::count_if(
        nodes, [](const auto &candidate) {
            const auto &signature = candidate.signature();
            return signature.parameters.size() == 3 && signature.parameters[0].name.text() == "expression" &&
                   signature.parameters[1].name.text() == "columns" &&
                   signature.parameters[2].name.text() == "parameters" && signature.output != nullptr &&
                   signature.output == std::addressof(TSC<double>::type());
        }));
    require(expression_nodes == 1, "mixed column expression did not fuse into one expression computation");

    auto parameter_collectors = std::size_t{};
    for (const auto &candidate : nodes) {
        const auto &signature = candidate.signature();
        if (signature.parameters.size() != 1 || signature.parameters[0].name.text() != "parameters" ||
            signature.output == nullptr)
            continue;
        const auto *output_type = type_cast<TimeSeriesType>(signature.output);
        if (output_type == nullptr)
            continue;
        const auto *value = dynamic_cast<const TimeSeriesValueType *>(
            std::addressof(output_type->dereferenced_type()));
        if (value == nullptr)
            continue;
        const auto *bundle = dynamic_cast<const ResolvedHierarchicalScalarType *>(
            std::addressof(value->value_type()));
        if (bundle == nullptr)
            continue;
        ++parameter_collectors;
        require(bundle->field_count() == 2 && bundle->field(0).type == std::addressof(cpp_scalar_type<i64>()) &&
                    bundle->field(1).type == std::addressof(cpp_scalar_type<double>()),
                "mixed column expression collector did not preserve both parameter types in one snapshot");
    }
    require(parameter_collectors == 1,
            "mixed column expression did not use one heterogeneous parameter collector");

    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> integer_parameter, wp<TS<double>> double_parameter,
                           wp<TS<i64>> tick) {
        const auto frame = write(command);
        return observe(make_result(frame, integer_parameter, double_parameter), tick);
    };

    mixed_i64_parameter_calls = 0;
    mixed_double_parameter_calls = 0;
    const auto commands = std::array<i64, 1>{1};
    const auto integer_parameters = std::array<i64, 1>{7};
    const auto double_parameters = std::array{0.5};
    const auto ticks = std::array<i64, 1>{0};
    static_cast<void>(testing::test_graph(graph, commands, integer_parameters, double_parameters, ticks));

    require(observations == std::vector<std::vector<std::optional<double>>>{{23.0}} &&
                mixed_i64_parameter_calls == 1 && mixed_double_parameter_calls == 1,
            "fused mixed column expression did not evaluate both typed parameters on their shared row domain");
}

void count_column_expression(std::span<const row_id_t> ids, const ColumnOperandBatch<double> &lhs,
                             const ColumnOperandBatch<double> &rhs, MutableColumnBatch<double> output) {
    ++counted_column_expression_calls;
    counted_column_expression_rows += ids.size();
    for (auto row = std::size_t{}; row != ids.size(); ++row) {
        const auto valid = lhs.is_valid(row) && rhs.is_valid(row);
        output.set_valid(row, valid);
        if (valid)
            output.values[row] = lhs.value(row) + rhs.value(row);
    }
}

struct double_column_materializer {
    In<TSC<double>> lhs;
    In<TSC<double>> rhs;
    Out<TSC<double>, computed<>> output;

    void evaluate() const {
        static_cast<void>(output.bind(lhs));
        lhs.for_each_row([&](row_id_t id, std::size_t) {
            ++double_column_materializer_rows;
            const auto left = lhs.cell(id);
            const auto right = rhs.cell(id);
            output.set(id, left && right ? std::optional<double>{*left + *right} : std::nullopt);
        });
    }
};

void test_double_column_addition_reuses_expression_and_sparse_rows() {
    using namespace bg;
    using namespace bg::literals;
    using namespace bg::frame;

    using Expression = TSB<BinaryColumnExpression<TSC<double>, TSC<double>, double>::template schema>;

    const auto deferred_add = graph("name"_n = "test_counted_column_add", [](wp<TSC<double>> lhs,
                                                                              wp<TSC<double>> rhs) {
        auto &wiring = WiringContext::current().wiring();
        auto result = wiring.create_wp<TSC<double>>(lhs, rhs);
        result.materialisation_option<Expression>([](wp<TSC<double>> left, wp<TSC<double>> right) {
            return bundle<Expression>(
                "lhs"_n = left,
                "rhs"_n = right,
                "compute"_n = operators::const_(
                                 ScalarValue<ColumnBlockFunction<double, double, double>>{
                                     ColumnBlockFunction<double, double, double>{&count_column_expression}})
                                 .as<TS<ColumnBlockFunction<double, double, double>, TimeSeriesGuarantee::const_>>());
        }, 0.0);
        return result;
    });

    struct observation {
        std::vector<std::optional<double>> values;
        std::size_t callback_calls{};
        std::size_t callback_rows{};
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.callback_calls = counted_column_expression_calls,
                                  .callback_rows = counted_column_expression_rows};
        input.for_each_row([&](row_id_t id, std::size_t) { result.values.push_back(input.cell(id)); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto expression = deferred_add(select_column<"bid">(frame), select_column<"ask">(frame));
        const auto selected = WiringContext::current().wiring().materialisation_form(expression, Expression::type());
        const auto expression_bundle = wp<Expression>{selected};
        return observe(operators::add(expression_bundle, expression_bundle), tick);
    };

    counted_column_expression_calls = 0;
    counted_column_expression_rows = 0;
    double_column_materializer_rows = 0;
    const auto commands = std::array<i64, 4>{0, 1, 2, 10};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "column expression observer missed a cycle");
    require(observations[0].callback_calls == 0 && observations[0].callback_rows == 0,
            "column expression evaluated an empty row set");
    require(observations[1].values == std::vector<std::optional<double>>{41.0} &&
                observations[1].callback_calls == 1 && observations[1].callback_rows == 1,
            "column addition did not evaluate one shared expression row once");
    require(observations[2].values == std::vector<std::optional<double>>{41.0, 81.0} &&
                observations[2].callback_calls == 2 && observations[2].callback_rows == 2,
            "column addition did not evaluate only the added expression row");
    require(observations[3].values == std::vector<std::optional<double>>{41.0, 68.0} &&
                observations[3].callback_calls == 3 && observations[3].callback_rows == 3,
            "column addition did not evaluate only the modified expression row");
}

void test_double_column_addition_reuses_forced_materialisation_form() {
    using namespace bg;
    using namespace bg::literals;
    using namespace bg::frame;

    using Expression = TSB<BinaryColumnExpression<TSC<double>, TSC<double>, double>::template schema>;

    const auto concrete_materializer = node<double_column_materializer>();
    const auto deferred_add = graph("name"_n = "test_forced_counted_column_add",
                                    [&concrete_materializer](wp<TSC<double>> lhs, wp<TSC<double>> rhs) {
        auto &wiring = WiringContext::current().wiring();
        auto result = wiring.create_wp<TSC<double>>(lhs, rhs);
        result.materialisation_option<Expression>([](wp<TSC<double>> left, wp<TSC<double>> right) {
            return bundle<Expression>(
                "lhs"_n = left,
                "rhs"_n = right,
                "compute"_n = operators::const_(
                                 ScalarValue<ColumnBlockFunction<double, double, double>>{
                                     ColumnBlockFunction<double, double, double>{&count_column_expression}})
                                 .as<TS<ColumnBlockFunction<double, double, double>, TimeSeriesGuarantee::const_>>());
        }, 0.0);
        result.materialisation_option<TSC<double>>([&concrete_materializer](wp<TSC<double>> left,
                                                                            wp<TSC<double>> right) {
            return concrete_materializer(left, right);
        }, 1.0);
        return result;
    });

    const auto force_concrete = node([](In<TSC<double>>) {});
    struct observation {
        std::vector<std::optional<double>> values;
        std::size_t callback_calls{};
        std::size_t callback_rows{};
        std::size_t materializer_rows{};
    };
    auto observations = std::vector<observation>{};
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.callback_calls = counted_column_expression_calls,
                                  .callback_rows = counted_column_expression_rows,
                                  .materializer_rows = double_column_materializer_rows};
        input.for_each_row([&](row_id_t id, std::size_t) { result.values.push_back(input.cell(id)); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto write = node<filter_writer>();
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        const auto expression = deferred_add(select_column<"bid">(frame), select_column<"ask">(frame));
        const auto selected = WiringContext::current().wiring().materialisation_form(expression, Expression::type());
        const auto expression_bundle = wp<Expression>{selected};
        const auto result = operators::add(operators::add(expression_bundle, expression_bundle),
                                           select_column<"bid">(frame));
        force_concrete(expression);
        return observe(result, tick);
    };

    counted_column_expression_calls = 0;
    counted_column_expression_rows = 0;
    double_column_materializer_rows = 0;
    const auto commands = std::array<i64, 4>{0, 1, 2, 10};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "forced materialisation observer missed a cycle");
    require(observations[0].values.empty() && observations[0].callback_calls == 0 &&
                observations[0].callback_rows == 0,
            "forced materialisation evaluated an empty row set");
    require(observations[1].values == std::vector<std::optional<double>>{51.5} &&
                observations[1].callback_calls == 0 && observations[1].callback_rows == 0 &&
                observations[1].materializer_rows == 1,
            "selected expression form repeated its forced concrete materialisation");
    require(observations[2].values == std::vector<std::optional<double>>{51.5, 101.5} &&
                observations[2].callback_calls == 0 && observations[2].callback_rows == 0 &&
                observations[2].materializer_rows == 3,
            "selected expression form repeated its forced concrete materialisation for an added row");
    require(observations[3].values == std::vector<std::optional<double>>{51.5, 82.0} &&
                observations[3].callback_calls == 0 && observations[3].callback_rows == 0 &&
                observations[3].materializer_rows == 5,
            "selected expression form repeated its forced concrete materialisation for a changed row");
}

void test_double_column_arithmetic_fusion_and_forced_materialization() {
    using namespace bg;
    using namespace bg::literals;
    using namespace bg::frame;

    const auto source = node([](Out<TSF<Quotes>>) {});
    const auto consume = node([](In<TSC<double>>) {});
    const auto count_expression_nodes = [&](auto build) {
        auto wiring = GraphWiring{};
        {
            auto context = WiringContext{wiring};
            const auto frame = source();
            const auto bid = select_column<"bid">(frame);
            const auto ask = select_column<"ask">(frame);
            const auto middle = operators::add(bid, ask);
            build(middle, bid);
        }
        wiring.finalize();
        const auto nodes = wiring.nodes();
        const auto parameter_inputs = static_cast<std::size_t>(std::ranges::count_if(
            nodes, [](const auto &candidate) {
                const auto &signature = candidate.signature();
                return std::ranges::any_of(signature.parameters, [](const auto &parameter) {
                    return parameter.name.text() == "parameters";
                });
            }));
        require(parameter_inputs != 0, "column expression topology did not expose its parameter input");
        const auto parameter_updates = static_cast<std::size_t>(std::ranges::count_if(
            nodes, [](const auto &candidate) {
                const auto &signature = candidate.signature();
                if (signature.parameters.size() != 1 || signature.parameters[0].name.text() != "parameters" ||
                    signature.output == nullptr)
                    return false;
                const auto *output = type_cast<TimeSeriesType>(signature.output);
                if (output == nullptr)
                    return false;
                const auto *value = dynamic_cast<const TimeSeriesValueType *>(
                    std::addressof(output->dereferenced_type()));
                return value != nullptr &&
                       dynamic_cast<const ResolvedHierarchicalScalarType *>(std::addressof(value->value_type())) !=
                           nullptr;
            }));
        require(parameter_updates == 0, "pure column expressions created a parameter update node");
        return static_cast<std::size_t>(std::ranges::count_if(nodes, [](const auto &candidate) {
            const auto &signature = candidate.signature();
            return signature.parameters.size() == 3 && signature.parameters[0].name.text() == "expression" &&
                   signature.parameters[1].name.text() == "columns" &&
                   signature.parameters[2].name.text() == "parameters" && signature.output != nullptr &&
                   signature.output == std::addressof(TSC<double>::type());
        }));
    };

    require(count_expression_nodes([&](auto middle, auto bid) { consume(middle + bid); }) == 1,
            "column addition did not fuse an unforced expression chain");
    require(count_expression_nodes([&](auto middle, auto bid) { consume(middle - bid + bid); }) == 1,
            "mixed column arithmetic did not fuse an unforced expression chain");
    require(count_expression_nodes([&](auto middle, auto bid) {
                consume(middle);
                consume(operators::sub(middle, bid));
            }) == 2,
            "column subtraction did not reuse a forced concrete intermediate");
    require(count_expression_nodes([&](auto middle, auto) {
                consume(middle);
                consume(operators::add(middle, middle));
            }) == 2,
            "column addition did not reuse one forced intermediate for both operands");
}

void test_double_column_addition_preserves_null_cells() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> values;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.push_back(input.cell(id));
        });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        return observe(operators::add(select_column<"bid">(frame), select_column<"ask">(frame)), tick);
    };

    const auto commands = std::array<i64, 3>{0, 12, 13};
    const auto ticks = std::array<i64, 3>{0, 1, 2};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size(), "nullable column addition observer missed a cycle");
    require(observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].values == std::vector<std::optional<double>>{std::nullopt},
            "column addition did not preserve an all-null result cell");
    require(observations[2].rows == std::vector<row_id_t>{0, 1} &&
                observations[2].values == std::vector<std::optional<double>>{std::nullopt, std::nullopt},
            "column addition did not preserve nulls when one operand is valid");
}

struct computed_frame_writer_state {
    row_id_t first{no_row};
};

struct computed_frame_writer {
    In<TS<i64>> command;
    State<computed_frame_writer_state> state;
    Out<TSF<Quotes>> output;

    void evaluate() const {
        switch (*command) {
        case 0:
            output.clear();
            return;
        case 1:
            state->first = output.add(
                Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.0}, .ask = Cell<double>{12.0}});
            return;
        case 2:
            output.set(state->first, RowPatch<Quotes>{.bid = 12.0});
            return;
        case 3:
            static_cast<void>(output.add(RowPatch<Quotes>{}));
            return;
        case 4:
        case 5:
        case 11:
            return;
        case 6:
            require(output.erase(state->first), "computed TSF writer could not remove its first row");
            return;
        case 7: {
            const auto recycled = output.add(
                Quotes<Cell>{.size = Cell<i64>{9}, .bid = Cell<double>{30.0}, .ask = Cell<double>{32.0}});
            require(recycled == state->first, "computed TSF writer did not reuse its retired row ID");
            return;
        }
        case 8:
            output.invalidate();
            return;
        case 9:
            output.clear();
            return;
        case 10:
            static_cast<void>(output.add(
                Quotes<Cell>{.size = Cell<i64>{11}, .bid = Cell<double>{40.0}, .ask = Cell<double>{42.0}}));
            return;
        default:
            throw std::runtime_error{"unknown computed TSF writer command"};
        }
    }
};

struct computed_quote_node {
    In<TSF<Quotes>> input;
    In<TS<i64>> command;
    Out<TSF<ComputedQuote>, frame::computed<"mid">> output;

    void evaluate() const {
        static_cast<void>(command);
        if (*command != 5)
            output.bind(input);
        if (!input.valid())
            return;

        auto ids = std::vector<row_id_t>{};
        auto values = std::vector<double>{};
        auto valid = std::vector<std::uint8_t>{};
        input.for_each_row([&](row_id_t id, std::size_t position) {
            if (position % 8 == 0)
                valid.push_back(0);
            ids.push_back(id);
            const auto bid = input.cell<1>(id);
            const auto ask = input.cell<2>(id);
            if (bid && ask) {
                values.push_back((*bid + *ask) / 2.0);
                valid.back() |= std::uint8_t{1} << (position % 8);
            } else {
                values.push_back(0.0);
            }
        });

        if (*command == 11) {
            require(!ids.empty(), "computed TSF batch validation test has no source row");
            const auto invalid_ids = std::array{ids.front(), no_row};
            const auto invalid_values = std::array{999.0, 1000.0};
            auto rejected = false;
            try {
                output.template set_batch<"mid">(
                    invalid_ids,
                    ColumnBatch<double>{0, invalid_ids.size(), invalid_values.data(),
                                        ColumnBatch<double>::Validity::all_valid()});
            } catch (const std::out_of_range &) {
                rejected = true;
            }
            require(rejected, "computed TSF batch accepted a row outside its source domain");
            return;
        }

        if (*command == 3) {
            output.template set_batch<"mid">(
                ids, ColumnBatch<double>{0, ids.size(), nullptr, ColumnBatch<double>::Validity::all_invalid()});
            return;
        }
        output.template set_batch<"mid">(
            ids, ColumnBatch<double>{0, ids.size(), values.empty() ? nullptr : values.data(),
                                     ColumnBatch<double>::Validity::from_bitmap(valid.data(), 0)});
    }
};

struct computed_bid_aggregate {
    In<TSF<ComputedQuote>> input;
    Out<TSF<ComputedQuote>, frame::computed<"bid">> output;

    inline static std::size_t runtime_selections{};

    static const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &arguments) {
        ++runtime_selections;
        return frame_computed_time_series_runtime_type<ComputedQuote>(
            *arguments.resolved_output_type(), computed_option<"bid">{});
    }

    void evaluate() const {
        output.bind(input);
        if (!input.valid())
            return;
        input.for_each_row([&](row_id_t id, std::size_t) {
            if (const auto bid = input.cell<2>(id))
                output.template set<"bid">(id, *bid + 10.0);
        });
    }
};

struct frame_command_source {
    int slot;
    Out<TS<i64>> output;

    void evaluate() const { output.set(commands[slot]); }

    inline static std::array<i64, 1> commands{1};
};

struct frame_trigger_source {
    Out<TS<i64>> output;

    void evaluate() const { output.set(1); }
};

struct frame_runtime_capture {
    In<TSF<Quotes>> frame;
    In<TS<i64>> trigger;

    void evaluate() const {
        static_cast<void>(trigger);
        captured_bid = frame.value_view().value().columns().bid.cell(0).value_or(-1.0);
    }

    inline static double captured_bid{};
};

struct frame_view_runtime_capture {
    In<TSF<Quotes>> frame;
    In<TS<i64>> trigger;

    void evaluate() const {
        static_cast<void>(trigger);
        const auto value = frame.value_view().value();
        captured_size = value.size();
        captured_bid = value.columns().bid.cell(0).value_or(-1.0);
    }

    inline static std::size_t captured_size{};
    inline static double captured_bid{};
};

void test_compiled_graph_runtime_storage_is_independent() {
    using namespace bg;
    using namespace bg::frame;

    const auto command = node<frame_command_source>();
    const auto trigger = node<frame_trigger_source>();
    const auto write = node<tsf_writer>();
    const auto capture = node<frame_runtime_capture>();
    const auto capture_view = node<frame_view_runtime_capture>();
    GraphWiring wiring;
    auto command_identity = std::optional<NodeCallIdentity>{};
    auto trigger_identity = std::optional<NodeCallIdentity>{};
    {
        WiringContext context{wiring};
        const auto command_input = command(0);
        const auto trigger_input = trigger();
        command_identity = wiring.node_call_identity(static_cast<WiringPoint>(command_input));
        trigger_identity = wiring.node_call_identity(static_cast<WiringPoint>(trigger_input));
        const auto frame = write(command_input);
        capture(frame, trigger_input);
        capture_view(filter_greater_than<"size">(frame, 4), trigger_input);
    }

    wiring.finalize();
    const auto command_index = wiring.node_index(*command_identity);
    const auto trigger_index = wiring.node_index(*trigger_identity);
    const auto compiled = CompiledGraph{wiring};
    auto first_global_state = GlobalState{};
    auto second_global_state = GlobalState{};
    auto first = RuntimeGraph{compiled, first_global_state};
    auto second = RuntimeGraph{compiled, second_global_state};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    first.start(first_time);
    second.start(first_time);
    first.schedule(command_index);
    first.schedule(trigger_index);
    second.schedule(command_index);
    second.schedule(trigger_index);
    first.evaluate(first_time);
    second.evaluate(first_time);
    require(frame_runtime_capture::captured_bid == 10.5, "runtime frame did not publish its initial row");
    require(frame_view_runtime_capture::captured_size == 1 && frame_view_runtime_capture::captured_bid == 10.5,
            "runtime filtered frame did not publish its initial row");

    frame_command_source::commands[0] = 2;
    const auto second_time = first_time + EvaluationTime::duration{1};
    first.schedule(command_index);
    first.evaluate(second_time);
    require(frame_runtime_capture::captured_bid == 11.5, "first runtime frame did not apply its row patch");
    require(frame_view_runtime_capture::captured_size == 1 && frame_view_runtime_capture::captured_bid == 11.5,
            "first runtime filtered view did not apply its row patch");

    const auto third_time = second_time + EvaluationTime::duration{1};
    second.schedule(trigger_index);
    second.evaluate(third_time);
    require(frame_runtime_capture::captured_bid == 10.5,
            "one runtime graph's frame mutation changed another runtime graph's frame");
    require(frame_view_runtime_capture::captured_size == 1 && frame_view_runtime_capture::captured_bid == 10.5,
            "one runtime graph's filtered view mutation changed another runtime graph's view");
}

void test_tsf_graph_behavior() {
    using namespace bg;
    using namespace bg::frame;

    auto observations = std::vector<frame_observation>{};
    auto column_observations = std::vector<column_observation>{};
    auto bid_consumer_observations = std::vector<column_observation>{};
    auto ask_consumer_observations = std::vector<column_observation>{};
    auto filtered_bid_consumer_observations = std::vector<column_observation>{};
    auto filtered_ask_consumer_observations = std::vector<column_observation>{};
    auto frame_consumer_observations = std::vector<frame_observation>{};
    const auto write = node<tsf_writer>();
    const auto observe = node([&](In<TSF<Quotes>, optional> input, In<TS<i64>> tick,
                                  Out<TS<Frame<Quotes>>> snapshot) {
        static_cast<void>(tick);
        auto result = frame_observation{.valid = input.valid(),
                                        .became_valid = input.became_valid(),
                                        .became_invalid = input.became_invalid(),
                                        .modified = input.modified(),
                                        .size = input.size()};
        input.for_each_row([&](row_id_t id, std::size_t) { result.rows.push_back(id); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        input.for_each_modified<1>([&](row_id_t id) { result.bid_modified.push_back(id); });
        if (input.valid())
            snapshot.set_value(input.value_view());
        observations.push_back(std::move(result));
    });
    const auto observe_column = node([&](In<TSC<double>, optional> input, In<TS<i64>> tick) {
        static_cast<void>(tick);
        auto result = column_observation{.valid = input.valid(),
                                         .became_valid = input.became_valid(),
                                         .became_invalid = input.became_invalid(),
                                         .modified = input.modified(),
                                         .size = input.size()};
        input.for_each_row([&](row_id_t id, std::size_t) { result.rows.push_back(id); });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        auto native_values = std::vector<const double *>{};
        input.for_each_batch([&](const auto &batch) { native_values.push_back(batch.cells.values); });
        auto scalar_values = std::vector<const double *>{};
        if (input.valid())
            input.value_view().value().batches([&](const auto &batch) { scalar_values.push_back(batch.values); });
        result.zero_copy = native_values == scalar_values;
        column_observations.push_back(std::move(result));
    });
    const auto observe_child = [](auto &consumer_observations) {
        return node([&consumer_observations](In<TSC<double>, optional> input) {
            auto result = column_observation{.valid = input.valid(),
                                             .became_valid = input.became_valid(),
                                             .became_invalid = input.became_invalid(),
                                             .modified = input.modified(),
                                             .size = input.size()};
            input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
            input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
            consumer_observations.push_back(std::move(result));
        });
    };
    const auto observe_bid = observe_child(bid_consumer_observations);
    const auto observe_ask = observe_child(ask_consumer_observations);
    const auto observe_filtered_bid = observe_child(filtered_bid_consumer_observations);
    const auto observe_filtered_ask = observe_child(filtered_ask_consumer_observations);
    const auto observe_frame = node([&](In<TSF<Quotes>, optional> input) {
        const auto read_storage = input.extension_descriptor();
        require(read_storage.source_runtime == std::addressof(input.runtime_type()),
                "TSF shared read storage lost its runtime binding");
        if (input.valid()) {
            auto bid = ColumnView<double>{};
            read_storage.bind_column_view(util::symbol{"bid"}, bid);
            require(bid.size() == input.size(), "TSF shared read storage returned the wrong column size");
            if (input.size() != 0)
                require(bid.cell(0) == input.cell<1>(read_storage.rows.at(0)),
                        "TSF shared read storage bound the wrong native column");
        }
        auto result = frame_observation{.valid = input.valid(),
                                        .became_valid = input.became_valid(),
                                        .became_invalid = input.became_invalid(),
                                        .modified = input.modified(),
                                        .size = input.size()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        frame_consumer_observations.push_back(std::move(result));
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        observe_column(select_column<"bid">(frame), tick);
        observe_bid(select_column<"bid">(frame));
        observe_ask(select_column<"ask">(frame));
        const auto filtered = filter_greater_than<"size">(frame, 4);
        observe_filtered_bid(select_column<"bid">(filtered));
        observe_filtered_ask(select_column<"ask">(filtered));
        observe_frame(frame);
        return observe(frame, tick);
    };
    const auto commands = std::array<i64, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto ticks = std::array<i64, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size(), "TSF observer did not run for every cycle");
    if (column_observations.size() != commands.size())
        throw std::runtime_error{"TSF column observer did not run for every cycle: " +
                                 std::to_string(column_observations.size()) + " of " +
                                 std::to_string(commands.size())};
    require(bid_consumer_observations.size() == 7 && ask_consumer_observations.size() == 7,
            "column consumers ran for an empty patch or missed a column publication");
    require(filtered_bid_consumer_observations.size() == 6 && filtered_ask_consumer_observations.size() == 6,
            "no-tick consumers missed filtered column membership or payload publications");
    require(filtered_bid_consumer_observations[1].changed == observations[1].rows &&
                filtered_bid_consumer_observations[2].changed == observations[1].rows &&
                filtered_ask_consumer_observations[1].changed == observations[1].rows &&
                filtered_ask_consumer_observations[2].changed == observations[1].rows &&
                filtered_bid_consumer_observations[3].removed == observations[1].rows &&
                filtered_ask_consumer_observations[3].removed == observations[1].rows &&
                filtered_bid_consumer_observations[4].became_invalid &&
                filtered_ask_consumer_observations[4].became_invalid,
            "filtered column consumers did not follow membership, payload, and validity changes");
    require(frame_consumer_observations.size() == 8,
            "frame-only consumer ran for an empty patch or missed a frame publication");
    for (const auto &entry : column_observations)
        require(entry.zero_copy, "TSF child and scalar column views did not share cell storage");
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].size == 0 && observations[0].changed.empty(),
            "TSF clear did not publish a valid empty frame");
    require(observations[1].valid && observations[1].size == 1 && observations[1].rows.size() == 1 &&
                observations[1].changed == observations[1].rows && observations[1].bid_modified.empty(),
            "TSF row insertion did not publish row membership");
    require(observations[2].valid && observations[2].changed == observations[1].rows &&
                observations[2].bid_modified == observations[1].rows,
            "TSF column modification was not visible in the frame delta");
    require(observations[3].valid && observations[3].changed == observations[1].rows &&
                observations[3].bid_modified.empty(),
            "TSF modification tracking leaked across columns");
    require(observations[4].valid && !observations[4].modified && observations[4].size == 1 &&
                observations[4].changed.empty() && observations[4].removed.empty(),
            "TSF omitted patch published an unchanged frame");
    require(observations[5].valid && observations[5].size == 0 && observations[5].removed == observations[1].rows &&
                observations[5].changed.empty(),
            "TSF row removal did not publish its removal delta");
    require(!observations[6].valid && observations[6].became_invalid && observations[6].size == 0 &&
                observations[6].changed.empty() && observations[6].removed.empty(),
            "TSF invalidation exposed stale row deltas");
    require(observations[7].valid && observations[7].became_valid && observations[7].size == 0 &&
                observations[7].changed.empty(),
            "TSF empty revalidation did not publish a valid frame");
    require(observations[8].valid && observations[8].modified && observations[8].size == 0 &&
                observations[8].changed.empty() && observations[8].removed.empty(),
            "TSF add-then-erase did not publish an empty frame with canceled row deltas");

    require(column_observations[0].valid && column_observations[0].became_valid &&
                column_observations[0].modified && column_observations[0].size == 0,
            "TSF column did not become a valid empty series with its frame");
    require(column_observations[1].valid && column_observations[1].size == 1 &&
                column_observations[1].changed == observations[1].rows,
            "TSF child did not observe added row membership");
    require(column_observations[2].valid && column_observations[2].changed == observations[1].rows,
            "TSF child did not observe its changed cell");
    require(column_observations[3].valid && !column_observations[3].modified &&
                column_observations[3].changed.empty(),
            "TSF child notified after a different column changed");
    require(column_observations[4].valid && !column_observations[4].modified &&
                column_observations[4].changed.empty(),
            "TSF child notified for an omitted row patch");
    require(column_observations[5].valid && column_observations[5].removed == observations[1].rows,
            "TSF child did not observe removed row membership");
    require(!column_observations[6].valid && column_observations[6].became_invalid &&
                column_observations[6].changed.empty() && column_observations[6].removed.empty(),
            "TSF invalidation did not reach its wired child");
    require(column_observations[7].valid && column_observations[7].became_valid &&
                column_observations[7].size == 0,
            "TSF revalidation did not reach its wired child");
    require(column_observations[8].valid && column_observations[8].modified &&
                column_observations[8].changed.empty() && column_observations[8].removed.empty(),
            "TSF child did not receive add-then-erase publication");

    require(bid_consumer_observations[2].modified &&
                bid_consumer_observations[2].changed == observations[1].rows &&
                ask_consumer_observations[2].modified &&
                ask_consumer_observations[2].changed == observations[1].rows,
            "column consumers missed their respective bid and ask patches");
    require(bid_consumer_observations[3].removed == observations[1].rows &&
                ask_consumer_observations[3].removed == observations[1].rows,
            "column consumers did not resume on row removal after selective column updates");
    require(frame_consumer_observations[2].modified && frame_consumer_observations[3].modified,
            "frame-only consumer missed a column patch");

    require(snapshots.size() == commands.size() && snapshots[0] && snapshots[1] && snapshots[2] && snapshots[3] &&
                snapshots[4] && snapshots[5] && snapshots[7] && snapshots[8],
            "TSF did not export valid whole-frame scalar snapshots");
    require(snapshots[0]->view().value().size() == 0 &&
                snapshots[1]->view().value().columns().bid.cell(0) == 10.5 &&
                snapshots[2]->view().value().columns().bid.cell(0) == 11.5 &&
                !snapshots[2]->view().value().columns().ask.cell(0) &&
                snapshots[3]->view().value().columns().ask.cell(0) == 12.5 &&
                snapshots[4]->view().value().columns().bid.cell(0) == 11.5 &&
                snapshots[5]->view().value().size() == 0 && snapshots[7]->view().value().size() == 0 &&
                snapshots[8]->view().value().size() == 0,
            "TSF scalar snapshots changed after later row mutations");
}

struct frame_literal_observation {
    bool valid{};
    bool became_valid{};
    bool became_invalid{};
    bool modified{};
    std::size_t size{};
    std::vector<row_id_t> rows;
    std::vector<row_id_t> changed;
    std::vector<row_id_t> removed;
    std::vector<row_id_t> key_modified;
    std::vector<row_id_t> price_modified;
    std::vector<std::optional<double>> prices;
};

void test_tsf_literal_updates() {
    using namespace bg;
    using namespace bg::frame;

    auto observations = std::vector<frame_literal_observation>{};
    const auto observe = node([&](In<TSF<KeyedQuotes>, optional> input, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto result = frame_literal_observation{.valid = input.valid(),
                                                .became_valid = input.became_valid(),
                                                .became_invalid = input.became_invalid(),
                                                .modified = input.modified(),
                                                .size = input.size()};
        const auto prices = input.column("price").input<double>();
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.prices.push_back(prices.cell(id));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        input.for_each_modified<0>([&](row_id_t id) { result.key_modified.push_back(id); });
        input.for_each_modified<1>([&](row_id_t id) { result.price_modified.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TSF<KeyedQuotes>> input, wp<TS<i64>> tick) { return observe(input, tick); };
    const auto result = testing::test_graph(
        graph,
        testing::input(TSF<KeyedQuotes>::type(),
                       {literal::TimeSeriesLiteral{}, {{{"id", i64{1}}, {"price", 10.0}}},
                        {{{"id", i64{1}}, {"price", 11.0}}}, {{{"id", i64{2}}}},
                        {{{"id", i64{1}}}}, literal::TimeSeriesLiteral{},
                        {literal::REMOVE({{"id", i64{1}}}), {{"id", i64{1}}, {"price", 12.0}}},
                        {literal::REMOVE({{"id", i64{99}}})}, literal::SKIP, literal::INVALID,
                        literal::TimeSeriesLiteral{}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(),
                       {i64{0}, i64{1}, i64{2}, i64{3}, i64{4}, i64{5}, i64{6}, i64{7}, i64{8}, i64{9},
                        i64{10}}));

    require(result.equals<i64>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) && observations.size() == 11,
            "TSF literal test graph missed an observation cycle");
    require(observations[0].valid && observations[0].became_valid && observations[0].size == 0,
            "empty TSF literal did not establish a valid empty frame");
    require(observations[1].valid && observations[1].size == 1 &&
                observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].changed == std::vector<row_id_t>{0} && observations[1].prices[0] == 10.0,
            "keyed TSF literal did not insert a row");
    require(observations[2].valid && observations[2].size == 1 &&
                observations[2].changed == std::vector<row_id_t>{0} &&
                observations[2].key_modified.empty() &&
                observations[2].price_modified == std::vector<row_id_t>{0} &&
                observations[2].prices[0] == 11.0,
            "keyed TSF literal did not patch only the supplied data column");
    require(observations[3].valid && observations[3].size == 2 &&
                observations[3].rows == std::vector<row_id_t>{0, 1} &&
                observations[3].prices[1] == std::nullopt,
            "keyed TSF literal did not insert omitted data columns as null");
    require(!observations[4].modified && observations[4].changed.empty() &&
                observations[4].key_modified.empty() && observations[4].size == 2,
            "key-only keyed TSF literal published a row modification");
    require(!observations[5].modified && observations[5].changed.empty() && observations[5].size == 2,
            "empty TSF literal cleared an already valid nonempty frame");
    require(observations[6].valid && observations[6].size == 2 &&
                observations[6].removed == std::vector<row_id_t>{0} &&
                observations[6].changed == std::vector<row_id_t>{2} &&
                std::ranges::is_permutation(observations[6].rows, std::array<row_id_t, 2>{1, 2}) &&
                observations[6].prices[std::ranges::find(observations[6].rows, row_id_t{2}) -
                                       observations[6].rows.begin()] == 12.0,
            "same-tick key removal and reinsertion did not preserve the removed row identity");
    require(!observations[7].modified && observations[7].removed.empty() && observations[7].size == 2,
            "keyed TSF literal published a missing-key removal");
    require(!observations[8].modified && observations[8].size == 2,
            "SKIP changed the TSF literal output");
    require(!observations[9].valid && observations[9].became_invalid && observations[9].size == 0,
            "INVALID did not invalidate the TSF literal output");
    require(observations[10].valid && observations[10].became_valid && observations[10].size == 0,
            "empty TSF literal did not revalidate the output");
}

void test_tsf_literal_uses_actual_derived_schema() {
    using namespace bg;
    using namespace bg::frame;

    auto observed_prices = std::vector<std::optional<double>>{};
    auto observed_bids = std::vector<std::optional<double>>{};
    const auto observe = node([&](In<TSF<KeyedQuotesBase>, optional> input, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        static_cast<void>(tick);
        if (input.valid()) {
            const auto prices = input.column("price").input<double>();
            const auto bids = input.column("bid").input<double>();
            input.for_each_row([&](row_id_t id, std::size_t) {
                observed_prices.push_back(prices.cell(id));
                observed_bids.push_back(bids.cell(id));
            });
        }
        output.set(static_cast<i64>(observed_prices.size()));
    });
    const auto graph = [&](wp<TSF<KeyedQuotesBase>> input, wp<TS<i64>> tick) { return observe(input, tick); };
    const auto &derived_runtime = frame_time_series_runtime_type<KeyedQuotesDerived>(TSF<KeyedQuotesDerived>::type());
    const auto result = testing::test_graph(
        graph,
        testing::input(derived_runtime,
                       {literal::TimeSeriesLiteral{}, {{{"id", i64{5}}, {"price", 10.0}, {"bid", 20.0}}},
                        {{{"id", i64{5}}, {"price", 11.0}}}}),
        testing::input(cpp_time_series_runtime_type<TS<i64>>(), {i64{0}, i64{1}, i64{2}}));

    require(result.size() == 3 && observed_prices == std::vector<std::optional<double>>{10.0, 11.0} &&
                observed_bids == std::vector<std::optional<double>>{20.0, 20.0},
            "TSF literal did not use the actual Derived schema through a Base input");
}

void test_tsf_literal_unkeyed_append_and_typed_output() {
    using namespace bg;
    using namespace bg::frame;

    auto sizes = std::vector<std::size_t>{};
    auto row_ids = std::vector<std::vector<row_id_t>>{};
    const auto observe = node([&](In<TSF<Quotes>, optional> input, Out<TS<i64>> output) {
        sizes.push_back(input.size());
        auto rows = std::vector<row_id_t>{};
        input.for_each_row([&](row_id_t id, std::size_t) { rows.push_back(id); });
        row_ids.push_back(std::move(rows));
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TSF<Quotes>> input) { return observe(input); };
    const auto &runtime = frame_time_series_runtime_type<Quotes>(TSF<Quotes>::type());
    const auto result = testing::test_graph(
        graph, testing::input(runtime, {{{{"bid", 10.5}}}, {{{"ask", 12.5}}}}));
    require(result.size() == 2 && sizes == std::vector<std::size_t>{1, 2} &&
                row_ids == std::vector<std::vector<row_id_t>>{{0}, {0, 1}},
            "unkeyed TSF literal did not append rows with consecutive row ids");

    auto observed_prices = std::vector<std::optional<double>>{};
    const auto write = node([](In<TS<i64>> tick, Out<TSF<KeyedQuotes>> output) {
        if (*tick == 1)
            output.apply({{{"id", i64{9}}, {"price", 15.5}}});
    });
    const auto sink = node([&](In<TSF<KeyedQuotes>> input, Out<TS<i64>> output) {
        input.for_each_row([&](row_id_t id, std::size_t) {
            observed_prices.push_back(input.column("price").input<double>().cell(id));
        });
        output.set(static_cast<i64>(input.size()));
    });
    const auto typed_graph = [&](wp<TS<i64>> tick) { return sink(write(tick)); };
    const auto typed_result = testing::test_graph(typed_graph, {0, 1});
    require(typed_result.size() == 2 && observed_prices == std::vector<std::optional<double>>{15.5},
            "typed TSF output apply did not apply a frame literal");
}

void test_tsf_literal_anonymous_composite_key() {
    using namespace bg;
    using namespace bg::frame;

    const auto fields = std::array{
        SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"sequence"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"value"}, &cpp_scalar_type<double>()},
    };
    const auto &schema = anonymous_schema(fields, key_metadata<"id", "sequence">());
    const auto &series_type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    const auto &runtime = frame_time_series_runtime_type<SCHEMA>(series_type);

    auto observations = std::vector<std::pair<std::size_t, std::vector<std::optional<double>>>>{};
    const auto observe = node([&](In<TSF<SCHEMA>, optional> input, Out<TS<i64>> output) {
        auto values = std::vector<std::optional<double>>{};
        const auto column = input.column("value").input<double>();
        input.for_each_row([&](row_id_t id, std::size_t) { values.push_back(column.cell(id)); });
        std::ranges::sort(values);
        observations.emplace_back(input.size(), std::move(values));
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TSF<SCHEMA>> input) { return observe(input); };
    const auto result = testing::test_graph(
        graph,
        testing::input(runtime,
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{1}}, {"sequence", i64{2}}, {"value", 10.0}}},
                        {{{"id", i64{1}}, {"sequence", i64{2}}, {"value", 11.0}}},
                        {{{"id", i64{1}}, {"sequence", i64{3}}, {"value", 12.0}}},
                        {literal::REMOVE({{"id", i64{1}}, {"sequence", i64{2}}})},
                        {{{"id", NULL_VALUE}, {"sequence", i64{4}}, {"value", 20.0}}},
                        {{{"id", NULL_VALUE}, {"sequence", i64{4}}, {"value", 21.0}}},
                        {literal::REMOVE({{"id", NULL_VALUE}, {"sequence", i64{4}}})}}));
    require(result.size() == 8 &&
                observations == std::vector<std::pair<std::size_t, std::vector<std::optional<double>>>>{
                                    {0, {}}, {1, {10.0}}, {1, {11.0}}, {2, {11.0, 12.0}}, {1, {12.0}},
                                    {2, {12.0, 20.0}}, {2, {12.0, 21.0}}, {1, {12.0}}},
            "anonymous-schema TSF literal did not apply composite key updates and removals");
}

void test_tsf_literal_rejections() {
    using namespace bg;
    using namespace bg::frame;

    const auto observe = node([](In<TSF<KeyedQuotes>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TSF<KeyedQuotes>> input) { return observe(input); };
    const auto rejects = [&](const TimeSeriesRuntimeType &runtime,
                             std::initializer_list<literal::TimeSeriesLiteral> ticks) {
        try {
            static_cast<void>(testing::test_graph(graph, testing::input(runtime, ticks)));
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    const auto &native_runtime = frame_time_series_runtime_type<KeyedQuotes>(TSF<KeyedQuotes>::type());
    require(rejects(native_runtime, {{{{"price", 10.0}}}}),
            "keyed TSF literal accepted a row with an omitted key");
    require(rejects(native_runtime, {{{{"id", i64{1}}, {"id", i64{2}}}}}),
            "keyed TSF literal accepted a duplicate field");
    require(rejects(native_runtime, {{literal::REMOVE({{"id", i64{1}}, {"price", 10.0}})}}),
            "keyed TSF literal accepted a non-key removal field");

    const auto unkeyed_observe = node([](In<TSF<Quotes>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto unkeyed_graph = [&](wp<TSF<Quotes>> input) { return unkeyed_observe(input); };
    const auto &unkeyed_runtime = frame_time_series_runtime_type<Quotes>(TSF<Quotes>::type());
    auto unkeyed_remove_rejected = false;
    try {
        static_cast<void>(testing::test_graph(
            unkeyed_graph,
            testing::input(unkeyed_runtime, {{literal::REMOVE({{"id", i64{1}}})}})));
    } catch (const std::invalid_argument &) {
        unkeyed_remove_rejected = true;
    }
    require(unkeyed_remove_rejected, "unkeyed TSF literal accepted a removal selector");

    const auto &view_runtime = frame_view_time_series_runtime_type<KeyedQuotes>(TSF<KeyedQuotes>::type());
    auto view_rejected = false;
    try {
        static_cast<void>(testing::test_graph(
            graph, testing::input(view_runtime, {{{{"id", i64{1}}, {"price", 10.0}}}})));
    } catch (const std::invalid_argument &) {
        view_rejected = true;
    }
    require(view_rejected, "TSF literal dispatch cast a view runtime to native storage");
}

void test_keyed_tsf_delta_capture() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<CompositeKeyRows>> input) { return input; };
    const auto &runtime = frame_time_series_runtime_type<CompositeKeyRows>(TSF<CompositeKeyRows>::type());
    const auto result = testing::test_graph(
        identity,
        testing::input(runtime,
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}, {"value", i64{4}}},
                         {{"id", i64{2}}, {"sequence", i64{1}}, {"price", 20.0}, {"value", i64{5}}}},
                        {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}}},
                        {{{"id", i64{3}}, {"sequence", i64{1}}},
                         literal::REMOVE({{"id", i64{3}}, {"sequence", i64{1}}})},
                        {literal::REMOVE({{"id", i64{1}}, {"sequence", i64{1}}}),
                         {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 12.0}}},
                        {literal::REMOVE({{"id", i64{2}}, {"sequence", i64{1}}})}}));

    require(result.equals({{},
                           {{{"id", i64{2}}, {"sequence", i64{1}}, {"price", 20.0}, {"value", i64{5}}},
                            {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}, {"value", i64{4}}}},
                           {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}}},
                           literal::TimeSeriesLiteral{},
                           {literal::REMOVE({{"id", i64{1}}, {"sequence", i64{1}}}),
                            {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 12.0}}},
                           {literal::REMOVE({{"id", i64{2}}, {"sequence", i64{1}}})}}),
            "keyed TSF capture did not match additions, sparse patches, and key-based removals");
    require(!result.equals({{},
                            {{{"id", i64{2}}, {"sequence", i64{1}}, {"price", 20.0}, {"value", i64{5}}},
                             {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}, {"value", i64{4}}}},
                            {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}, {"value", i64{4}}}},
                            literal::TimeSeriesLiteral{},
                            {literal::REMOVE({{"id", i64{1}}, {"sequence", i64{1}}}),
                             {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 12.0}}},
                            {literal::REMOVE({{"id", i64{2}}, {"sequence", i64{1}}})}}),
            "keyed TSF capture treated an unchanged column as part of a sparse patch");
    require(result.equals({{},
                           {{{"id", i64{2}}, {"sequence", i64{1}}, {"price", 20.0}, {"value", i64{5}}},
                            {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}, {"value", i64{4}}}},
                           {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}}},
                           literal::TimeSeriesLiteral{},
                           {literal::REMOVE({{"id", i64{1}}, {"sequence", i64{1}}}),
                            {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 12.0}}},
                           {literal::REMOVE({{"id", i64{2}}, {"sequence", i64{1}}})}}),
            "a failed keyed TSF capture comparison affected a later comparison");

    require(!result.equals({{},
                            {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}},
                             {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}}},
                            {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}}},
                            literal::TimeSeriesLiteral{},
                            {literal::REMOVE({{"id", i64{1}}, {"sequence", i64{1}}}),
                             {{"id", i64{1}}, {"sequence", i64{1}}, {"price", 12.0}}},
                            {literal::REMOVE({{"id", i64{2}}, {"sequence", i64{1}}})}}),
            "keyed TSF capture accepted duplicate expected row keys");
}

void test_keyed_tsf_capture_treats_omitted_added_cells_as_null() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<KeyedQuotes>> input) { return input; };
    const auto result = testing::test_graph(identity, {{}, {{{"id", i64{7}}}}});
    require(result.equals({{}, {{{"id", i64{7}}}}}),
            "keyed TSF capture did not treat omitted addition cells as null");
}

void test_keyed_tsf_capture_uses_actual_derived_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<KeyedQuotesBase>> input) { return input; };
    const auto &runtime = frame_time_series_runtime_type<KeyedQuotesDerived>(TSF<KeyedQuotesDerived>::type());
    const auto result = testing::test_graph(
        identity,
        testing::input(runtime,
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{5}}, {"price", 10.0}, {"bid", 20.0}}}}));
    require(result.equals({{}, {{{"id", i64{5}}, {"price", 10.0}, {"bid", 20.0}}}}),
            "keyed TSF capture did not compare the actual Derived schema through a Base output");
}

void test_keyed_tsf_capture_uses_keyed_runtime_schema_for_unkeyed_base_output() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<UnkeyedQuotesBase>> input) { return input; };
    const auto &runtime = frame_time_series_runtime_type<KeyedQuotesDerivedFromUnkeyed>(
        TSF<KeyedQuotesDerivedFromUnkeyed>::type());
    const auto result = testing::test_graph(
        identity,
        testing::input(runtime,
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{5}}, {"price", 10.0}, {"bid", 9.0}}}}));
    require(result.equals({{}, {{{"id", i64{5}}, {"price", 10.0}, {"bid", 9.0}}}}),
            "keyed TSF capture did not use the actual keyed runtime schema for a Base output");
}

void test_keyed_tsf_capture_uses_runtime_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto fields = std::array{
        SchemaField{util::symbol{"id"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"price"}, &cpp_scalar_type<double>()},
    };
    const auto &schema = anonymous_schema(fields, key_metadata<"id">());
    const auto &series_type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    const auto &runtime = frame_time_series_runtime_type<SCHEMA>(series_type);
    const auto identity = [](wp<TSF<SCHEMA>> input) { return input; };
    const auto result = testing::test_graph(
        identity, testing::input(runtime, {literal::TimeSeriesLiteral{}, {{{"id", i64{5}}, {"price", 10.0}}}}));
    require(result.equals({{}, {{{"id", i64{5}}, {"price", 10.0}}}}),
            "keyed TSF capture did not parse its runtime-defined schema");
}

void test_keyed_tsf_capture_supports_null_composite_keys() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<CompositeKeyRows>> input) { return input; };
    const auto &runtime = frame_time_series_runtime_type<CompositeKeyRows>(TSF<CompositeKeyRows>::type());
    const auto result = testing::test_graph(
        identity,
        testing::input(runtime,
                       {literal::TimeSeriesLiteral{},
                        {{{"id", NULL_VALUE}, {"sequence", i64{4}}, {"price", 20.0}, {"value", i64{8}}}},
                        {literal::REMOVE({{"id", NULL_VALUE}, {"sequence", i64{4}}})}}));
    require(result.equals({{},
                           {{{"id", NULL_VALUE}, {"sequence", i64{4}}, {"price", 20.0}, {"value", i64{8}}}},
                           {literal::REMOVE({{"id", NULL_VALUE}, {"sequence", i64{4}}})}}),
            "keyed TSF capture did not match a null component in a composite key");
}

void test_keyed_tsf_capture_invalidates_and_revalidates_without_old_deltas() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<KeyedQuotes>> input) { return input; };
    const auto &runtime = frame_time_series_runtime_type<KeyedQuotes>(TSF<KeyedQuotes>::type());
    const auto result = testing::test_graph(
        identity,
        testing::input(runtime,
                       {{{{"id", i64{1}}, {"price", 10.0}}}, literal::INVALID, literal::TimeSeriesLiteral{}}));
    require(result.equals({{{{"id", i64{1}}, {"price", 10.0}}}, literal::INVALID, literal::TimeSeriesLiteral{}}),
            "keyed TSF capture leaked old row changes into an empty revalidated frame");
}

void test_keyed_tsf_capture_rejects_missing_actual_patch_fields() {
    using namespace bg;
    using namespace bg::frame;

    const auto identity = [](wp<TSF<CompositeKeyRows>> input) { return input; };
    const auto &runtime = frame_time_series_runtime_type<CompositeKeyRows>(TSF<CompositeKeyRows>::type());
    const auto result = testing::test_graph(
        identity,
        testing::input(runtime,
                       {literal::TimeSeriesLiteral{},
                        {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}, {"value", i64{4}}}},
                        {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}, {"value", i64{5}}}}}));
    require(!result.equals({{},
                            {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 10.0}, {"value", i64{4}}}},
                            {{{"id", i64{1}}, {"sequence", i64{1}}, {"price", 11.0}}}}),
            "keyed TSF capture accepted an expected patch that omitted an actual modified field");
}

void test_borrowed_tsf_input_with_native_and_view_runtimes() {
    using namespace bg;
    using namespace bg::frame;

    const auto child_graph = make_borrowed_frame_graph();
    borrowed_frame_runner::compiled_graph = std::addressof(child_graph);
    const auto write = node<tsf_writer>();
    const auto run_borrowed = node<borrowed_frame_runner>();
    const auto commands = std::array<i64, 2>{1, 2};

    const auto run = [&](auto graph, const TimeSeriesRuntimeType &expected_runtime) {
        borrowed_frame_sink::observations.clear();
        borrowed_frame_sink::expected_bid_values = nullptr;
        borrowed_frame_sink::expected_data = nullptr;
        borrowed_frame_runner::child = nullptr;
        borrowed_frame_runner::evaluator = nullptr;

        const auto result = testing::test_graph(graph, commands);
        require(result.size() == commands.size() && borrowed_frame_sink::observations.size() == commands.size(),
                "borrowed TSF sink did not run for every graph cycle");
        const auto &first = borrowed_frame_sink::observations[0];
        const auto &second = borrowed_frame_sink::observations[1];
        require(first.runtime_type == std::addressof(expected_runtime) &&
                    second.runtime_type == std::addressof(expected_runtime),
                "borrowed TSF input changed its runtime descriptor");
        require(first.zero_copy_source && second.zero_copy_source,
                "borrowed TSF input changed its source storage pointer");
        require(first.zero_copy_column && second.zero_copy_column,
                "borrowed TSF input changed its column storage pointer");
        require(first.rows == std::vector<row_id_t>{0} && second.rows == std::vector<row_id_t>{0} &&
                    first.sizes == std::vector<std::optional<i64>>{5} &&
                    second.sizes == std::vector<std::optional<i64>>{5} &&
                    first.bids == std::vector<std::optional<double>>{10.5} &&
                    second.bids == std::vector<std::optional<double>>{11.5} &&
                    first.asks == std::vector<std::optional<double>>{std::nullopt} &&
                    second.asks == std::vector<std::optional<double>>{std::nullopt},
                "borrowed TSF input changed row or cell values");
        require(first.modified && second.modified && second.last_updated > first.last_updated,
                "borrowed TSF input lost its modification state or time");
        require(second.changed == std::vector<row_id_t>{0} &&
                    second.size_modified.empty() && second.bid_modified == std::vector<row_id_t>{0} &&
                    second.ask_modified.empty(),
                "borrowed TSF input reported incorrect row or column deltas");
    };

    const auto native_graph = [&](wp<TS<i64>> command) {
        const auto frame = write(command);
        run_borrowed(frame, frame);
        return command;
    };
    run(native_graph, frame_time_series_runtime_type<Quotes>(TSF<Quotes>::type()));

    const auto view_graph = [&](wp<TS<i64>> command) {
        const auto frame = write(command);
        const auto view = project<Quotes, "size", "bid", "ask">(frame);
        run_borrowed(view, frame);
        return command;
    };
    run(view_graph, frame_view_time_series_runtime_type<Quotes>(TSF<Quotes>::type()));
}

void test_generic_tsf_input_schema_and_columns() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::size_t field_count{};
        std::array<util::symbol, 3> field_names{};
        std::optional<i64> size;
        std::optional<double> bid;
    };

    auto observations = std::vector<observation>{};
    auto selected_bids = std::vector<double>{};
    const auto write = node<tsf_writer>();
    const auto observe_selected = node([&](In<TSC<SCALAR>> input) {
        const auto column = input.input<double>();
        column.for_each_row(
            [&](row_id_t id, std::size_t) { selected_bids.push_back(column.cell(id).value()); });
    });
    const auto observe = node([&](In<TSF<SCHEMA>, optional> input, Out<TS<i64>> output) {
        require(&input.input_data().runtime_type() == &input.runtime_type(),
                "TSF input did not expose its input data");
        auto result = observation{};
        const auto &schema = input.schema();
        result.field_count = schema.field_count();
        for (auto index = std::size_t{}; index < result.field_count; ++index)
            result.field_names[index] = schema.field(index).name;

        if (input.valid()) {
            require(input.input_data().valid() == input.valid(), "TSF input data did not preserve validity");
            const auto rejects_column_index = [&](std::size_t index) {
                try {
                    static_cast<void>(input.column(index));
                } catch (const std::out_of_range &) {
                    return true;
                }
                return false;
            };
            require(rejects_column_index(schema.field_count()),
                    "generic TSF input accepted an ordinal at the field count");
            const auto wrapped_index = static_cast<std::size_t>(std::numeric_limits<item_id_t>::max()) + 1;
            require(rejects_column_index(wrapped_index),
                    "generic TSF input narrowed an invalid ordinal to a valid child index");
            auto row = row_id_t{};
            input.for_each_row([&](row_id_t id, std::size_t) { row = id; });
            result.size = input.column(0).view<i64>().cell(row);
            result.bid = input.column("bid").view<double>().cell(row);
        }

        observations.push_back(std::move(result));
        output.set(1);
    });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto source = wp<TSF<SCHEMA>>{write(command)};
        static_cast<void>(observe_selected(select_column<"bid">(source)));
        return observe(source);
    };
    const auto commands = std::array<i64, 1>{1};
    static_cast<void>(testing::test_graph(graph, commands));

    require(observations.size() == 1 && observations[0].field_count == 3 &&
                observations[0].field_names ==
                    std::array{util::symbol{"size"}, util::symbol{"bid"}, util::symbol{"ask"}} &&
                observations[0].size == 5 && observations[0].bid == 10.5 && selected_bids == std::vector<double>{10.5},
            "generic TSF input did not expose its resolved schema and scalar-variable columns");
}

void test_typed_patch_rows_insert_nullable_rows() {
    using namespace bg;
    using namespace bg::frame;

    using row_values = std::tuple<std::optional<i64>, std::optional<double>, std::optional<double>>;
    struct observation {
        std::vector<row_id_t> rows;
        std::vector<row_values> values;
    };
    auto observations = std::vector<observation>{};

    const auto write = node([](In<TS<i64>> command, Out<TSF<Quotes>> output) {
        if (*command == 0)
            static_cast<void>(output.add(RowPatch<Quotes>{.bid = 10.5}));
        else
            static_cast<void>(output.add(RowPatch<Quotes>{}));
    });
    const auto observe = node([&](In<TSF<Quotes>> input, In<TS<i64>> command, Out<TS<i64>> output) {
        auto result = observation{};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.values.emplace_back(input.cell<0>(id), input.cell<1>(id), input.cell<2>(id));
        });
        observations.push_back(std::move(result));
        output.set(*command);
    });
    const auto graph = [&](wp<TS<i64>> command) { return observe(write(command), command); };
    const auto commands = std::array<i64, 2>{0, 1};
    const auto result = testing::test_graph(graph, commands);

    require(result.size() == 2 && observations.size() == 2 && observations[0].rows == std::vector<row_id_t>{0} &&
                observations[0].values == std::vector<row_values>{{std::nullopt, 10.5, std::nullopt}} &&
                observations[1].rows == std::vector<row_id_t>{0, 1} &&
                observations[1].values ==
                    std::vector<row_values>{{std::nullopt, 10.5, std::nullopt},
                                            {std::nullopt, std::nullopt, std::nullopt}},
            "typed RowPatch insertion did not initialize omitted fields to null");
}

void test_runtime_schema_tsf_writes_and_named_projection() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> prices;
    };
    auto observations = std::vector<observation>{};
    struct row_observation {
        std::vector<row_id_t> rows;
        std::vector<std::optional<i64>> sizes;
        std::vector<std::optional<double>> bids;
        std::vector<std::optional<double>> asks;
    };
    auto source_observations = std::vector<row_observation>{};

    const auto schema_fields = std::array{
        SchemaField{util::symbol{"size"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"bid"}, &cpp_scalar_type<double>()},
        SchemaField{util::symbol{"ask"}, &cpp_scalar_type<double>()},
    };
    const auto &schema = anonymous_schema(schema_fields);
    auto empty_size = Column<i64>{std::vector<std::optional<i64>>{}};
    auto empty_bid = Column<double>{std::vector<std::optional<double>>{}};
    auto empty_ask = Column<double>{std::vector<std::optional<double>>{}};
    const auto seed_columns = std::array{ColumnView<>{empty_size.view()}, ColumnView<>{empty_bid.view()},
                                         ColumnView<>{empty_ask.view()}};
    auto memory = allocator{};
    auto seed = make_frame_value(memory, schema, 0, seed_columns);
    auto erased_seed = ScalarValue<>{seed, memory};

    auto seed_ticks = std::vector<std::optional<ScalarValue<>>>{};
    auto command_ticks = std::vector<std::optional<ScalarValue<>>>{};
    const auto commands = std::array<i64, 24>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                               12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    for (const auto command : commands) {
        seed_ticks.emplace_back(erased_seed.view());
        command_ticks.emplace_back(ScalarValue<i64>{command});
    }
    const auto inputs = std::array{
        testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks},
        testing::TestGraphInput{cpp_scalar_runtime_type<i64>(), command_ticks},
    };

    const auto write = node<runtime_schema_tsf_writer>();
    const auto pass_through = node([&](In<TSF<SCHEMA>, optional> input, In<TS<i64>> command,
                                       Out<TSF<SCHEMA>, view> output) {
        static_cast<void>(command);
        auto result = row_observation{};
        if (input.valid()) {
            const auto sizes = input.column("size").view<i64>();
            const auto bids = input.column("bid").view<double>();
            const auto asks = input.column("ask").view<double>();
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.sizes.push_back(sizes.cell(id));
                result.bids.push_back(bids.cell(id));
                result.asks.push_back(asks.cell(id));
            });
        }
        source_observations.push_back(std::move(result));
        const auto source_names = std::array{util::symbol{"size"}, util::symbol{"bid"}, util::symbol{"ask"}};
        static_cast<void>(output.bind_projection(input, std::span<const util::symbol>{source_names}));
    });
    const auto project = node([](In<TSF<SCHEMA>, optional> input, Out<TSF<BidOnly>, view> output) {
        const auto source_names = std::array{util::symbol{"bid"}};
        static_cast<void>(output.bind_projection(input, std::span<const util::symbol>{source_names}));
    });
    const auto observe = node([&](In<TSF<BidOnly>, optional> input, In<TS<i64>> command,
                                  Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid()};
        if (input.valid()) {
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.prices.push_back(input.cell<0>(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(*command);
    });
    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "runtime_schema_tsf_writer", [&](wp<TS<Frame<SCHEMA>>> seed_input,
                                                                                wp<TS<i64>> command) {
        const auto frame = write(seed_input, command);
        return observe(project(pass_through(frame, command)), command);
    });
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});

    require(result.size() == commands.size() && observations.size() == commands.size(),
            "runtime-schema TSF writer graph missed a command cycle");
    require(!observations[0].valid && observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].prices == std::vector<std::optional<double>>{10.5} &&
                observations[2].rows == std::vector<row_id_t>{0} &&
                observations[2].prices == std::vector<std::optional<double>>{11.5},
            "runtime-schema TSF writer did not add or patch scalar row arguments");
    require(observations[3].rows == std::vector<row_id_t>{0, 1} &&
                observations[3].prices == std::vector<std::optional<double>>{11.5, 20.5} &&
                observations[4].prices == std::vector<std::optional<double>>{11.5, std::nullopt},
            "runtime-schema TSF writer did not adapt concrete rows or preserve unselected columns");
    require(observations[5].rows == std::vector<row_id_t>{0, 1} &&
                observations[5].prices == std::vector<std::optional<double>>{11.5, std::nullopt} &&
                observations[6].prices == std::vector<std::optional<double>>{11.5, std::nullopt} &&
                observations[7].prices == std::vector<std::optional<double>>{std::nullopt, std::nullopt} &&
                observations[8].rows == std::vector<row_id_t>{0, 1} &&
                observations[8].prices == std::vector<std::optional<double>>{std::nullopt, std::nullopt} &&
                observations[9].rows == std::vector<row_id_t>{0, 1, 2} &&
                observations[9].prices == std::vector<std::optional<double>>{std::nullopt, std::nullopt, 30.5} &&
                observations[10].rows == std::vector<row_id_t>{0, 1, 2, 3} &&
                observations[10].prices ==
                    std::vector<std::optional<double>>{std::nullopt, std::nullopt, 30.5, std::nullopt},
            "runtime-schema TSF validation or empty patch changed projected rows");
    require(source_observations.size() == commands.size() &&
                source_observations[11].rows == std::vector<row_id_t>{0, 1, 2, 3, 4} &&
                source_observations[11].sizes[4] == 9 && source_observations[11].bids[4] == 40.5 &&
                source_observations[11].asks[4] == 41.5 && source_observations[12].bids[4] == 42.5 &&
                !source_observations[13].bids[4] && source_observations[14].bids[5] == 50.5 &&
                !source_observations[14].sizes[5] && !source_observations[14].asks[5] &&
                source_observations[15].rows.size() == 7 && !source_observations[15].bids[6] &&
                source_observations[16].rows.size() == 7 && source_observations[17].sizes[4] == 9 &&
                source_observations[17].bids[4] == std::nullopt && source_observations[18].rows.size() == 7 &&
                source_observations[19].rows.size() == 7 && source_observations[20].rows.size() == 7 &&
                source_observations[21].sizes[4] == 9 && source_observations[21].bids[4] == std::nullopt &&
                source_observations[22].rows.size() == 8 && source_observations[22].sizes[7] == 70 &&
                source_observations[22].bids[7] == 71.5 && source_observations[22].asks[7] == 72.5 &&
                source_observations[23].bids[7] == 73.5,
            "generic typed row/patch adaptation changed cells, accepted invalid mappings, or exposed a shadowed field");
}

void test_generic_declared_tsf_rows_follow_resolved_cell_type() {
    using namespace bg;
    using namespace bg::frame;
    using observed_cell = std::variant<std::optional<i64>, std::optional<double>>;

    auto observations = std::vector<std::vector<observed_cell>>{};
    const auto write = node([](In<TS<Frame<GenericQuote>>> seed, In<TS<i64>> command,
                               Out<TSF<GenericQuote>> output) {
        static_cast<void>(seed);
        const auto &cell_type = *output.schema().field(0).type;
        const auto with_bid = [&](auto value, auto &&operation) {
            using Value = std::remove_cvref_t<decltype(value)>;
            const auto typed = Scalar<Value>::from(value);
            const auto borrowed = Scalar<>::from(typed);
            std::invoke(std::forward<decltype(operation)>(operation), borrowed);
            value = Value{};
        };

        switch (*command) {
        case 0:
            if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>())) {
                with_bid(i64{12}, [&](Scalar<> value) {
                    static_cast<void>(output.add(Row<GenericQuote>{.bid = Cell<SCALAR>{value}}));
                });
            } else {
                with_bid(12.5, [&](Scalar<> value) {
                    static_cast<void>(output.add(Row<GenericQuote>{.bid = Cell<SCALAR>{value}}));
                });
            }
            return;
        case 1:
            if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>())) {
                with_bid(i64{14}, [&](Scalar<> value) {
                    output.set(0, RowPatch<GenericQuote>{.bid = PatchCell<SCALAR>{value}});
                });
            } else {
                with_bid(14.5, [&](Scalar<> value) {
                    output.set(0, RowPatch<GenericQuote>{.bid = PatchCell<SCALAR>{value}});
                });
            }
            return;
        case 2:
            output.set(0, RowPatch<GenericQuote>{.bid = std::nullopt});
            return;
        case 3:
            static_cast<void>(output.add(RowPatch<GenericQuote>{}));
            return;
        default:
            output.set(0, RowPatch<GenericQuote>{});
            return;
        }
    });
    const auto observe = node([&](In<TSF<GenericQuote>, optional> input, In<TS<i64>> command,
                                  Out<TS<i64>> output) {
        static_cast<void>(command);
        auto result = std::vector<observed_cell>{};
        if (input.valid()) {
            const auto column = input.column("bid");
            input.for_each_row([&](row_id_t id, std::size_t) {
                if (std::addressof(column.column_view().cell_type()) == std::addressof(cpp_scalar_type<i64>()))
                    result.emplace_back(column.view<i64>().cell(id));
                else
                    result.emplace_back(column.view<double>().cell(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(1);
    });

    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "generic_declared_tsf_rows", [&](wp<TS<Frame<SCHEMA>>> seed,
                                                                                   wp<TS<i64>> command) {
        return observe(write(seed, command), command);
    });
    const auto commands = std::array<i64, 5>{0, 1, 2, 3, 4};

    for (const auto *cell_type : {&cpp_scalar_type<i64>(), &cpp_scalar_type<double>()}) {
        auto type_map = TypeMap{};
        type_map.set(bg::details::scalar_variable_instance<SCALAR>(), cell_type);
        const auto &resolved_schema = dynamic_cast<const bg::Schema &>(frame_schema_type<GenericQuote>().resolve(type_map));
        auto memory = allocator{};
        auto seed_value = std::optional<ScalarValue<Frame<SCHEMA>>>{};
        if (cell_type == std::addressof(cpp_scalar_type<i64>())) {
            auto empty = Column<i64>{std::vector<std::optional<i64>>{}};
            const auto columns = std::array{ColumnView<>{empty.view()}};
            seed_value.emplace(make_frame_value(memory, resolved_schema, 0, columns));
        } else {
            auto empty = Column<double>{std::vector<std::optional<double>>{}};
            const auto columns = std::array{ColumnView<>{empty.view()}};
            seed_value.emplace(make_frame_value(memory, resolved_schema, 0, columns));
        }
        auto erased_seed = ScalarValue<>{*seed_value, memory};
        auto seed_ticks = std::vector<std::optional<ScalarValue<>>>{};
        auto command_ticks = std::vector<std::optional<ScalarValue<>>>{};
        for (const auto command : commands) {
            seed_ticks.emplace_back(erased_seed.view());
            command_ticks.emplace_back(ScalarValue<i64>{command});
        }
        const auto inputs = std::array{
            testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks},
            testing::TestGraphInput{cpp_scalar_runtime_type<i64>(), command_ticks},
        };
        const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});
        require(result.size() == commands.size(), "generic declared TSF graph missed a command cycle");
    }

    require(observations.size() == 2 * commands.size(), "generic declared TSF did not run for both cell types");
    for (auto offset = std::size_t{}; offset != observations.size(); offset += commands.size()) {
        require(observations[offset].size() == 1 && observations[offset + 1].size() == 1 &&
                    observations[offset + 2].size() == 1 && observations[offset + 3].size() == 2 &&
                    observations[offset + 4].size() == 2,
                "generic declared TSF changed its first row shape across updates");
        const auto integer = std::holds_alternative<std::optional<i64>>(observations[offset][0]);
        if (integer) {
            require(std::get<std::optional<i64>>(observations[offset][0]) == 12 &&
                        std::get<std::optional<i64>>(observations[offset + 1][0]) == 14 &&
                        !std::get<std::optional<i64>>(observations[offset + 2][0]) &&
                        observations[offset + 3].size() == 2 && !std::get<std::optional<i64>>(observations[offset + 3][1]) &&
                        observations[offset + 4] == observations[offset + 3],
                    "generic declared TSF did not store borrowed i64 cells independently");
        } else {
            require(std::get<std::optional<double>>(observations[offset][0]) == 12.5 &&
                        std::get<std::optional<double>>(observations[offset + 1][0]) == 14.5 &&
                        !std::get<std::optional<double>>(observations[offset + 2][0]) &&
                        observations[offset + 3].size() == 2 &&
                        !std::get<std::optional<double>>(observations[offset + 3][1]) &&
                        observations[offset + 4] == observations[offset + 3],
                    "generic declared TSF did not store borrowed double cells independently");
        }
    }
}

void test_repeated_and_constrained_generic_tsf_fields_match_runtime_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto schema_fields = std::array{
        SchemaField{util::symbol{"size"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"bid"}, &cpp_scalar_type<i64>()},
        SchemaField{util::symbol{"ask"}, &cpp_scalar_type<i64>()},
    };
    const auto &schema = anonymous_schema(schema_fields);
    auto empty_size = Column<i64>{std::vector<std::optional<i64>>{}};
    auto empty_bid = Column<i64>{std::vector<std::optional<i64>>{}};
    auto empty_ask = Column<i64>{std::vector<std::optional<i64>>{}};
    const auto seed_columns = std::array{ColumnView<>{empty_size.view()}, ColumnView<>{empty_bid.view()},
                                         ColumnView<>{empty_ask.view()}};
    auto memory = allocator{};
    auto seed = make_frame_value(memory, schema, 0, seed_columns);
    auto erased_seed = ScalarValue<>{seed, memory};

    const auto write = node([](In<TS<Frame<SCHEMA>>> seed_input, Out<TSF<SCHEMA>> output) {
        static_cast<void>(seed_input);
        const auto size = i64{3};
        const auto bid = i64{5};
        const auto ask = i64{7};
        const auto typed_size = Scalar<i64>::from(size);
        const auto typed_bid = Scalar<i64>::from(bid);
        const auto typed_ask = Scalar<i64>::from(ask);
        const auto generic_size = Scalar<>::from(typed_size);
        const auto generic_bid = Scalar<>::from(typed_bid);
        const auto generic_ask = Scalar<>::from(typed_ask);
        static_cast<void>(output.add(Row<RepeatedConstrainedGenericQuote>{
            .size = Cell<SCALAR>{generic_size}, .bid = Cell<SCALAR>{generic_bid},
            .ask = Cell<I64Cell>{generic_ask}}));
    });
    const auto observe = node([](In<TSF<SCHEMA>> input, Out<TS<i64>> output) {
        const auto size = input.column("size").view<i64>().cell(0);
        const auto bid = input.column("bid").view<i64>().cell(0);
        const auto ask = input.column("ask").view<i64>().cell(0);
        require(size == 3 && bid == 5 && ask == 7,
                "repeated and constrained generic fields did not match compatible runtime columns");
        output.set(1);
    });

    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "repeated_constrained_generic_tsf_fields", [&](wp<TS<Frame<SCHEMA>>> seed_input) {
        return observe(write(seed_input));
    });
    const auto seed_ticks = std::array{std::optional<ScalarValue<>>{erased_seed.view()}};
    const auto inputs = std::array{testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks}};
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});
    require(result.size() == 1, "repeated and constrained generic TSF writer missed its cycle");
}

void test_inherited_tsf_graph_behavior() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool modified{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<std::optional<i64>> sizes;
        std::vector<std::optional<double>> bids;
        std::vector<std::optional<double>> asks;
        std::vector<std::optional<double>> base_bids;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<inherited_tsf_writer>();
    const auto observe = node([&](In<TSF<InheritedFrame>, optional> input,
                                  In<TSF<InheritedFrameBase>, optional> base,
                                  In<TS<i64>> tick, Out<TS<Frame<InheritedFrame>>> snapshot) {
        static_cast<void>(tick);
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.sizes.push_back(input.cell<0>(id));
            result.bids.push_back(input.cell<1>(id));
            result.asks.push_back(input.cell<2>(id));
            result.base_bids.push_back(base.cell<1>(id));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        if (input.valid())
            snapshot.set_value(input.value_view());
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto frame = write(command);
        return observe(frame, frame, tick);
    };
    const auto commands = std::array<i64, 4>{0, 1, 2, 3};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size() && observations[0].valid && observations[0].rows.empty() &&
                observations[1].rows == std::vector<row_id_t>{0} && observations[1].sizes[0] == 5 &&
                observations[1].bids[0] == 12.5 && !observations[1].asks[0] &&
                observations[1].base_bids[0] == observations[1].bids[0],
            "inherited TSF input did not read base and direct logical fields");
    require(observations[2].changed == std::vector<row_id_t>{0} && !observations[2].bids[0] &&
                observations[2].asks[0] == 15.5 && observations[2].base_bids[0] == observations[2].bids[0],
            "inherited TSF patch did not publish logical null and direct field updates");
    require(observations[3].rows.empty() && observations[3].removed == std::vector<row_id_t>{0},
            "inherited TSF removal did not reach typed derived and base inputs");
    require(snapshots.size() == commands.size() && snapshots[1] && snapshots[2] &&
                snapshots[1]->view().value().columns().bid.cell(0) == 12.5 &&
                !snapshots[2]->view().value().columns().bid.cell(0) &&
                snapshots[2]->view().value().columns().ask.cell(0) == 15.5,
            "inherited TSF scalar copies did not preserve logical field values");
}

void test_base_frame_output_retains_derived_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto write = node<inherited_tsf_writer>();
    const auto borrow_derived = [](In<TSF<InheritedFrame>, optional> input) { return input.value_view(); };
    const auto capture = node([&](In<TSF<InheritedFrame>, optional> input, In<TS<i64>> tick,
                                  Out<TS<Frame<InheritedFrameBase>>> output) {
        static_cast<void>(tick);
        if (input.valid()) {
            const auto borrowed = borrow_derived(input);
            output.set_value(Scalar<Frame<InheritedFrameBase>>::from(borrowed));
        }
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) { return capture(write(command), tick); };
    const auto commands = std::array<i64, 4>{0, 1, 2, 3};
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(snapshots.size() == commands.size() && snapshots[1] && snapshots[2],
            "base frame output did not publish owning snapshots");
    const auto first = snapshots[1]->view().value();
    const auto patched = snapshots[2]->view().value();
    require(first.schema().field_count() == 3 && first.columns().size.cell(0) == 5 &&
                first.columns().bid.cell(0) == 12.5 && !first.column("ask").view<double>().cell(0),
            "base frame output lost the actual derived schema or initial values");
    require(patched.schema().field_count() == 3 && patched.columns().size.cell(0) == 5 &&
                !patched.columns().bid.cell(0) && patched.column("ask").view<double>().cell(0) == 15.5,
            "base frame output lost derived fields during a patch");
    require(!first.column("ask").view<double>().cell(0),
            "base frame snapshot changed after later source mutation and graph teardown");
}

void test_tsf_projection_view() {
    using namespace bg;
    using namespace bg::frame;

    struct projection_observation {
        bool valid{};
        bool became_valid{};
        bool became_invalid{};
        bool modified{};
        bool zero_copy{};
        std::size_t size{};
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::optional<double> price;
    };

    auto observations = std::vector<projection_observation>{};
    const auto write = node<tsf_writer>();
    const auto observe = node([&](In<TSF<Quotes>, optional> source, In<TSF<BidOnly>, optional> input, In<TS<i64>> tick,
                                  Out<TS<i64>> output) {
        auto result = projection_observation{.valid = input.valid(),
                                             .became_valid = input.became_valid(),
                                             .became_invalid = input.became_invalid(),
                                             .modified = input.modified(),
                                             .size = input.size()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        if (input.valid() && input.size() != 0) {
            input.for_each_row([&](row_id_t id, std::size_t) { result.price = input.cell<0>(id); });
            auto source_values = std::vector<const double *>{};
            source.value_view().value().columns().bid.batches(
                [&](const auto &batch) { source_values.push_back(batch.values); });
            auto projected_values = std::vector<const double *>{};
            input.value_view().value().columns().price.batches(
                [&](const auto &batch) { projected_values.push_back(batch.values); });
            result.zero_copy = !source_values.empty() && source_values == projected_values;
        }
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto source = write(command);
        const auto projected = project<BidOnly, "bid">(source);
        return observe(source, projected, command);
    };
    const auto commands = std::array<i64, 8>{0, 1, 2, 3, 4, 5, 6, 7};
    static_cast<void>(testing::test_graph(graph, commands));

    require(observations.size() == commands.size(),
            ("TSF projection observer missed a cycle: " + std::to_string(observations.size()) + " of " +
             std::to_string(commands.size())).c_str());
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].size == 0,
            "TSF projection did not publish its first empty binding");
    require(observations[1].valid && observations[1].size == 1 && observations[1].changed.size() == 1 &&
                observations[1].zero_copy && observations[1].price == 10.5,
            "TSF projection did not expose the added source row zero-copy");
    require(observations[2].modified && observations[2].changed == observations[1].changed &&
                observations[2].zero_copy && observations[2].price == 11.5,
            "TSF projection did not propagate a mapped column modification");
    require(!observations[3].modified && observations[3].changed.empty() && observations[3].price == 11.5,
            "TSF projection published a source column that was not selected");
    require(!observations[4].modified && observations[4].changed.empty(),
            "TSF projection published an empty source patch");
    require(observations[5].modified && observations[5].size == 0 &&
                observations[5].removed == observations[1].changed,
            "TSF projection did not normalize row removal");
    require(!observations[6].valid && observations[6].became_invalid,
            "TSF projection did not propagate invalidation");
    require(observations[7].valid && observations[7].became_valid && observations[7].modified &&
                observations[7].size == 0,
            "TSF projection did not rebuild after revalidation");
}

void test_tsf_projection_empty_columns_match_target_schema() {
    using namespace bg;
    using namespace bg::frame;

    const auto reject_projection = node([](In<TSF<Quotes>> input, Out<TSF<BidOnly>, view> output) {
        output.bind_projection(input, columns<>());
    });
    const auto observe_bid_only = node([](In<TSF<BidOnly>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto write_quotes = node<tsf_writer>();
    const auto rejected_graph = [&](wp<TS<i64>> command) {
        return observe_bid_only(reject_projection(write_quotes(command)));
    };

    auto rejected = false;
    try {
        static_cast<void>(testing::test_graph(rejected_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "empty TSF projection accepted a target schema with fields");

    const auto write_empty = node([](In<TS<i64>> command, Out<TSF<EmptySchema>> output) {
        if (*command == 1)
            static_cast<void>(output.add(EmptySchema<Cell>{}));
    });
    const auto project_empty = node([](In<TSF<EmptySchema>> input, Out<TSF<EmptySchema>, view> output) {
        output.bind_projection(input, columns<>());
    });
    const auto observe_empty = node([](In<TSF<EmptySchema>, optional> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto empty_graph = [&](wp<TS<i64>> command) {
        return observe_empty(project_empty(write_empty(command)));
    };
    const auto result = testing::test_graph(empty_graph, std::array<i64, 1>{1});
    require(result.equals<i64>({1}),
            "empty TSF projection did not preserve rows for a zero-field target schema");
}

void test_tsf_projection_selection_change() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool modified{};
        std::optional<double> price;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<tsf_writer>();
    const auto project = node<tsf_switchable_projection>();
    const auto observe = node([&](In<TSF<BidOnly>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid(), .modified = input.modified()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        if (input.valid() && input.size() != 0)
            input.for_each_row([&](row_id_t id, std::size_t) { result.price = input.cell<0>(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> selection, wp<TS<i64>> tick) {
        const auto source = write(command);
        return observe(project(source, selection), tick);
    };
    const auto commands = std::array<i64, 8>{7, 1, 4, 3, 4, 2, 4, 6};
    const auto selections = std::array<i64, 8>{0, 0, 0, 0, 1, 1, 0, 0};
    const auto ticks = std::array<i64, 8>{0, 1, 2, 3, 4, 5, 6, 7};
    static_cast<void>(testing::test_graph(graph, commands, selections, ticks));

    require(observations.size() == commands.size(), "switchable TSF projection missed a cycle");
    require(observations[0].valid && observations[0].modified && observations[0].changed.empty(),
            "switchable TSF projection did not bind an empty source");
    require(observations[1].valid && observations[1].changed.size() == 1 && observations[1].price == 10.5,
            "switchable TSF projection did not bind its first row");
    require(!observations[2].modified,
            "switchable TSF projection published an unchanged source");
    require(!observations[3].modified,
            "switchable TSF projection published a change to the unselected column");
    require(observations[4].modified && observations[4].changed == observations[1].changed &&
                observations[4].removed.empty() && observations[4].price == 12.5,
            "switchable TSF projection did not publish a same-row selection change as a modification");
    require(!observations[5].modified,
            "switchable TSF projection published a change to the previously selected column");
    require(observations[6].modified && observations[6].changed == observations[1].changed &&
                observations[6].price == 11.5,
            "switchable TSF projection did not publish a later selection change");
    require(!observations[7].valid, "switchable TSF projection did not propagate invalidation");
}

void test_tsf_projection_rejects_invalid_mappings_before_invalidation() {
    using namespace bg;
    using namespace bg::frame;

    auto projection_errors = std::array<bool, 2>{};
    auto observations = std::vector<bool>{};
    const auto write = node<tsf_writer>();
    const auto project = node([&](In<TSF<Quotes>, optional> input, In<TS<i64>> selection,
                                  Out<TSF<BidOnly>, view> output) {
        if (*selection == 0) {
            output.bind_projection(input, std::array<std::size_t, 1>{1});
            return;
        }

        const auto source_index = *selection == 1 ? std::size_t{99} : std::size_t{0};
        auto rejected = false;
        try {
            output.bind_projection(input, std::array{source_index});
        } catch (const std::out_of_range &) {
            rejected = *selection == 1;
        } catch (const std::invalid_argument &) {
            rejected = *selection == 2;
        }
        require(rejected, "invalid TSF projection mapping was not rejected");
        projection_errors[static_cast<std::size_t>(*selection - 1)] = rejected;
    });
    const auto observe = node([&](In<TSF<BidOnly>, optional> input, In<TS<i64>> selection, Out<TS<i64>> output) {
        observations.push_back(input.valid());
        output.set(static_cast<i64>(input.valid()));
        static_cast<void>(selection);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> selection) {
        return observe(project(write(command), selection), selection);
    };
    const auto commands = std::array<i64, 3>{1, 6, 6};
    const auto selections = std::array<i64, 3>{0, 1, 2};
    const auto result = testing::test_graph(graph, commands, selections);

    require(projection_errors[0] && projection_errors[1] && observations == std::vector<bool>{true, true, true} &&
                result.equals<i64>({1, 1, 1}),
            "invalid ordinal or cell type mapping invalidated a previously valid TSF projection");
}

void test_tsf_projection_rejects_empty_row_domain_mismatch() {
    using namespace bg;
    using namespace bg::frame;

    auto memory = allocator{};
    const auto time = MIN_DT + EvaluationTime::duration{1};
    auto first = NativeFrame<Quotes>{};
    auto second = NativeFrame<Quotes>{};
    first.clear(time, memory);
    second.clear(time, memory);

    auto mismatched = native_frame_input(second);
    FrameReadBindings bindings{};
    bindings.apply(mismatched);
    bindings.source = native_frame_input(first);
    const auto bid_field = std::array{mismatched.field_index(util::symbol{"bid"})};
    auto rejected = false;
    with_runtime_view_storage<BidOnly>(memory, [&](auto &output, const auto &) {
        try {
            static_cast<void>(output.bind(mismatched, bid_field, std::nullopt, nullptr, nullptr, time, memory));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
    });
    first.release(memory);
    second.release(memory);
    require(rejected, "empty TSF projection accepted a mismatched physical row domain");
}

void test_tsf_projection_rejects_native_columns_from_another_logical_domain() {
    using namespace bg;
    using namespace bg::frame;

    auto memory = allocator{};
    auto value_frame = NativeFrame<Quotes>{};
    auto visible_frame = NativeFrame<Quotes>{};
    auto release = util::scope_exit{[&]() noexcept {
        value_frame.release(memory);
        visible_frame.release(memory);
    }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    const auto value_id = value_frame.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                                       .ask = Cell<double>{11.0}},
                                         time, memory);
    const auto visible_id = visible_frame.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                                          .ask = Cell<double>{21.0}},
                                             time, memory);
    require(value_id == visible_id && value_frame.rows().identity() != visible_frame.rows().identity(),
            "test setup did not create overlapping frame row IDs in distinct logical domains");

    auto input = native_frame_input(value_frame);
    const auto foreign_rows = RowSet::native(visible_frame.rows());
    input.rows = foreign_rows;
    const auto bid_index = input.field_index(util::symbol{"bid"});
    auto bindings = FrameReadBindings{};
    bindings.source = input;
    bindings.overridden_index = bid_index;
    bindings.overridden_binding = input.column(bid_index);
    bindings.overridden_binding.visible_rows = foreign_rows;
    bindings.overridden_binding.coverage = foreign_rows;
    bindings.apply(input);

    auto rejected = false;
    const auto bid_field = std::array{bid_index};
    with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &) {
        try {
            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, time, memory));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
    });
    require(rejected, "TSF projection accepted a native column delta for an overlapping foreign row ID");
}

void test_view_updates_do_not_traverse_source_rows() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto id = source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                            .ask = Cell<double>{11.0}},
                               first_time, memory);
    const auto row_counter = RowTraversalCounter{std::addressof(source.rows())};
    auto bindings = FrameReadBindings{};
    const auto make_input = [&] {
        auto input = native_frame_input(source);
        const auto rows = row_counter.row_set();
        input.rows = rows;
        bindings.visible_rows = rows;
        bindings.override_visible_rows = true;
        bindings.apply(input);
        return input;
    };

    auto input = make_input();
    const auto bid_field = std::array{input.field_index(util::symbol{"bid"})};
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto predicate_index = input.field_index(util::symbol{"size"});
    auto predicate = [&](row_id_t row) { return source.cell<0>(row).value_or(0) > 4; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);

    with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &) {
        with_runtime_view_storage<Quotes>(memory, [&](auto &filter, const auto &) {
            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, first_time, memory));
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          first_time, memory));

            auto size_patch = Quotes<PatchCell>{};
            size_patch.size = 6;
            const auto second_time = first_time + EvaluationTime::duration{1};
            source.set(id, size_patch, second_time, memory);
            input = make_input();
            row_counter.traversals = 0;
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          second_time, memory));
            require(row_counter.traversals == 0,
                    "steady-state predicate-column update traversed all source rows");

            auto bid_patch = Quotes<PatchCell>{};
            bid_patch.bid = 12.5;
            const auto third_time = second_time + EvaluationTime::duration{1};
            source.set(id, bid_patch, third_time, memory);
            input = make_input();
            row_counter.traversals = 0;
            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, third_time, memory));
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          third_time, memory));
            require(row_counter.traversals == 0,
                    "steady-state projection and payload updates traversed all source rows");
        });
    });
}

void test_filter_row_set_reads_cached_source_order() {
    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto release_source = util::scope_exit{[&]() noexcept { source.release(memory); }};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first = source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.0},
                                               .ask = Cell<double>{11.0}},
                                  first_time, memory);
    const auto second = source.add(Quotes<Cell>{.size = Cell<i64>{6}, .bid = Cell<double>{20.0},
                                                .ask = Cell<double>{21.0}},
                                   first_time, memory);
    const auto row_counter = RowTraversalCounter{std::addressof(source.rows())};
    auto bindings = FrameReadBindings{};
    const auto make_input = [&] {
        auto input = native_frame_input(source);
        const auto rows = row_counter.row_set();
        input.rows = rows;
        bindings.visible_rows = rows;
        bindings.override_visible_rows = true;
        bindings.apply(input);
        return input;
    };
    auto input = make_input();
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto predicate_index = input.field_index(util::symbol{"size"});
    auto predicate = [&](row_id_t id) { return source.cell<0>(id).value_or(0) > 4; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);
    with_runtime_view_storage<Quotes>(memory, [&](auto &filter, const auto &runtime) {
        static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                      first_time, memory));
        const auto second_time = first_time + EvaluationTime::duration{1};
        require(source.erase(first, second_time, memory), "filter ordering test could not remove its first row");
        const auto reused_slot = source.add(Quotes<Cell>{.size = Cell<i64>{7}, .bid = Cell<double>{30.0},
                                                         .ask = Cell<double>{31.0}},
                                            second_time, memory);
        require(reused_slot != first && reused_slot == 2 && source.rows().slot_of(reused_slot) == 0 &&
                    source.rows().slot_of(second) == 1,
                "filter ordering test did not add a later row ID into the earlier physical slot");
        input = make_input();
        row_counter.traversals = 0;
        static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                      second_time, memory));
        const auto traversals_after_bind = row_counter.traversals;
        const auto filtered = static_cast<const FrameSeriesRuntimeType &>(runtime).input_storage(&filter);
        auto observed = std::vector<row_id_t>{};
        filtered.rows.each([&](row_id_t id) { observed.push_back(id); });
        require(observed == std::vector<row_id_t>{reused_slot, second} && filtered.rows.at(0) == reused_slot &&
                    filtered.rows.at(1) == second,
                "filter output did not preserve source order after membership changes");
        for (auto iteration = std::size_t{}; iteration != 3; ++iteration) {
            filtered.rows.each([](row_id_t) {});
            static_cast<void>(filtered.rows.at(0));
            static_cast<void>(filtered.rows.at(1));
        }
        require(row_counter.traversals == traversals_after_bind,
                "filter row reads enumerated the source after bind rebuilt its ordered selection");
    });
}

void test_tsf_view_uses_local_physical_rows_incrementally() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};
    auto physical_rows = NativeRowSlots{};
    auto local_bid = NativeColumnData<double>{};
    auto local_release = util::scope_exit{[&]() noexcept {
        local_bid.release(memory);
        physical_rows.release(memory);
    }};

    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto first_id = source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                                   .ask = Cell<double>{11.0}},
                                    first_time, memory);
    const auto second_id = source.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                                    .ask = Cell<double>{21.0}},
                                     first_time, memory);
    physical_rows.insert(second_id, memory);
    local_bid.add(physical_rows, second_id, double{20.5}, memory);
    physical_rows.insert(first_id, memory);
    local_bid.add(physical_rows, first_id, double{10.5}, memory);

    const auto row_counter = RowTraversalCounter{std::addressof(source.rows())};
    auto bindings = FrameReadBindings{};
    const auto bid_index = std::size_t{1};
    auto local_column = ColumnStorage<double>{};
    auto make_input = [&](EvaluationTime time) {
        auto input = native_frame_input(source);
        const auto rows = row_counter.row_set();
        input.rows = rows;
        local_column = ColumnStorage<double>{local_bid, physical_rows, rows, source.rows().added(),
                                             local_bid.modified(), source.rows().removed(), time};
        bindings.source = input;
        bindings.overridden_index = bid_index;
        bindings.overridden_binding = frame::details::frame_column_read_binding<double>(std::addressof(local_column));
        bindings.apply(input);
        return input;
    };

    auto input = make_input(first_time);
    const auto bid_field = std::array{bid_index};
    with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &runtime) {
        require(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, first_time, memory),
                "TSF view did not bind a local physical row mapping");
        auto projected = runtime.input_storage(std::addressof(projection));
        auto column = projected.generic_column(0).template view<double>();
        require(column.cell(0) == 10.5 && column.cell(1) == 20.5,
                "TSF view exposed local physical slot order instead of source row order");

        const auto second_time = first_time + EvaluationTime::duration{1};
        const auto third_id = source.add(Quotes<Cell>{.size = Cell<i64>{9}, .bid = Cell<double>{30.5},
                                                       .ask = Cell<double>{31.0}},
                                        second_time, memory);
        physical_rows.insert(third_id, memory);
        local_bid.add(physical_rows, third_id, double{30.5}, memory);
        input = make_input(second_time);
        row_counter.traversals = 0;
        require(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, second_time, memory) &&
                    row_counter.traversals == 0,
                "TSF view traversed existing local rows instead of validating the added row delta");
        projected = runtime.input_storage(std::addressof(projection));
        column = projected.generic_column(0).template view<double>();
        require(column.cell(2) == 30.5, "TSF view did not read a newly added local physical slot");
    });
}

void test_filtered_column_coverage_rejects_a_different_row_set() {
    using namespace bg;
    using namespace bg::frame;

    auto resource = std::pmr::monotonic_buffer_resource{};
    const auto memory = allocator{resource};
    auto source = NativeFrame<Quotes>{};
    auto source_release = util::scope_exit{[&]() noexcept { source.release(memory); }};

    const auto time = MIN_DT + EvaluationTime::duration{1};
    static_cast<void>(source.add(Quotes<Cell>{.size = Cell<i64>{5}, .bid = Cell<double>{10.5},
                                              .ask = Cell<double>{11.0}},
                                time, memory));
    static_cast<void>(source.add(Quotes<Cell>{.size = Cell<i64>{8}, .bid = Cell<double>{20.5},
                                              .ask = Cell<double>{21.0}},
                                time, memory));
    const auto input = native_frame_input(source);
    const auto all_fields = std::array<std::size_t, 3>{0, 1, 2};
    const auto bid_index = input.field_index(util::symbol{"bid"});
    const auto bid_field = std::array{bid_index};
    const auto predicate_index = input.field_index(util::symbol{"size"});
    auto predicate = [&](row_id_t id) { return source.cell<0>(id).value_or(0) > 6; };
    const auto [predicate_context, predicate_function] = runtime_predicate(predicate);
    with_runtime_view_storage<Quotes>(memory, [&](auto &filter, const auto &filter_runtime) {
        with_runtime_view_storage<BidOnly>(memory, [&](auto &projection, const auto &projection_runtime) {
            static_cast<void>(filter.bind(input, all_fields, predicate_index, predicate_context, predicate_function,
                                          time, memory));

            auto mismatched = input;
            auto bindings = FrameReadBindings{};
            bindings.source = input;
            bindings.overridden_index = bid_index;
            bindings.overridden_binding = filter_runtime.input_storage(std::addressof(filter)).column(bid_index);
            bindings.apply(mismatched);
            auto rejected = false;
            try {
                static_cast<void>(projection.bind(mismatched, bid_field, std::nullopt, nullptr, nullptr, time,
                                                  memory));
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "a filtered column accepted a frame row set outside its visible coverage");

            auto operation_switch_rejected = false;
            try {
                static_cast<void>(filter.bind(input, all_fields, std::nullopt, nullptr, nullptr,
                                              time + EvaluationTime::duration{1}, memory));
            } catch (const std::logic_error &) {
                operation_switch_rejected = true;
            }
            require(operation_switch_rejected, "a TSF view output silently switched from filtering to projection");

            static_cast<void>(projection.bind(input, bid_field, std::nullopt, nullptr, nullptr, time, memory));
            auto projected = projection_runtime.input_storage(std::addressof(projection));
            auto always_true = [](row_id_t) { return true; };
            const auto [always_true_context, always_true_function] = runtime_predicate(always_true);
            operation_switch_rejected = false;
            try {
                static_cast<void>(projection.bind(projected, std::array<std::size_t, 1>{0}, 0,
                                                  always_true_context, always_true_function,
                                                  time + EvaluationTime::duration{1}, memory));
            } catch (const std::logic_error &) {
                operation_switch_rejected = true;
            }
            require(operation_switch_rejected, "a TSF view output silently switched from projection to filtering");
        });
    });
}

void test_incremental_numeric_filter() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        bool became_valid{};
        bool became_invalid{};
        bool modified{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<double> bids;
    };
    auto observations = std::vector<observation>{};
    auto predicate_visits = std::vector<std::size_t>{};
    const auto write = node<filter_writer>();
    const auto filter = node([&](In<TSF<Quotes>, optional> input, Out<TSF<Quotes>, view> output) {
        auto visits = std::size_t{};
        output.template bind_filter<"size">(input, [&](row_id_t id) {
            ++visits;
            return input.cell<0>(id).value_or(0) > 4;
        });
        predicate_visits.push_back(visits);
    });
    const auto observe = node([&](In<TSF<Quotes>, optional> input, In<TS<i64>> tick,
                                  Out<TS<Frame<Quotes>>> snapshot) {
        auto result = observation{.valid = input.valid(),
                                  .became_valid = input.became_valid(),
                                  .became_invalid = input.became_invalid(),
                                  .modified = input.modified()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.bids.push_back(input.cell<1>(id).value_or(-1.0));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        if (input.valid())
            snapshot.set_value(input.value_view());
        static_cast<void>(tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        return observe(filter(write(command)), tick);
    };
    const auto commands = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto ticks = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto snapshots = testing::test_graph(graph, commands, ticks);

    require(observations.size() == commands.size(), "numeric filter observer missed a cycle");
    require(predicate_visits == std::vector<std::size_t>{0, 1, 1, 0, 1, 1, 0, 1, 0, 0},
            "numeric filter evaluated rows outside additions and predicate-column modifications");
    require(observations[0].valid && observations[0].became_valid && observations[0].modified &&
                observations[0].rows.empty(),
            "numeric filter did not bind its first empty source");
    require(observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].changed == std::vector<row_id_t>{0} && observations[1].bids ==
                    std::vector<double>{10.5},
            "numeric filter did not include a qualifying added row");
    require(observations[2].rows == std::vector<row_id_t>{0, 1} &&
                observations[2].changed == std::vector<row_id_t>{1},
            "numeric filter did not include a second qualifying row");
    require(observations[3].modified && observations[3].changed == std::vector<row_id_t>{0} &&
                observations[3].bids == std::vector<double>{11.5, 20.5},
            "numeric filter did not forward selected payload changes");
    require(observations[4].modified && observations[4].rows == std::vector<row_id_t>{1} &&
                observations[4].removed == std::vector<row_id_t>{0},
            "numeric filter did not remove a row when its predicate became false");
    require(observations[5].modified && observations[5].rows == std::vector<row_id_t>{0, 1} &&
                observations[5].changed == std::vector<row_id_t>{0},
            "numeric filter did not reinclude a row when its predicate became true");
    require(observations[6].modified && observations[6].rows == std::vector<row_id_t>{1} &&
                observations[6].removed == std::vector<row_id_t>{0},
            "numeric filter did not remove a deleted selected row");
    const auto bid_for = [](const observation &entry, row_id_t id) {
        const auto row = std::ranges::find(entry.rows, id);
        require(row != entry.rows.end(), "numeric filter observation lost a row ID");
        return entry.bids[static_cast<std::size_t>(row - entry.rows.begin())];
    };
    require(observations[7].rows.size() == 2 && observations[7].changed == std::vector<row_id_t>{0} &&
                bid_for(observations[7], 0) == 12.5 && bid_for(observations[7], 1) == 20.5,
            "numeric filter lost row/value alignment or reused row identity incorrectly");
    require(!observations[8].valid && observations[8].became_invalid &&
                observations[8].changed.empty() && observations[8].removed.empty(),
            "numeric filter did not propagate source invalidation");
    require(observations[9].valid && observations[9].became_valid && observations[9].rows.empty(),
            "numeric filter did not rebuild after an empty source revalidation");
    require(snapshots.size() == commands.size() && snapshots[2] && snapshots[2]->view().value().size() == 2 &&
                snapshots[2]->view().value().columns().bid.cell(0) == 10.5 &&
                snapshots[3]->view().value().columns().bid.cell(0) == 11.5,
            "numeric filter snapshots did not retain stable row data");
}

void test_filter_greater_than_composes_with_column_projection() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool valid{};
        std::vector<row_id_t> rows;
        std::vector<double> bids;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<filter_writer>();
    const auto observe = node([&](In<TSC<double>, optional> input, In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.valid = input.valid()};
        input.for_each_row([&](row_id_t id, std::size_t) {
            result.rows.push_back(id);
            result.bids.push_back(input.cell(id).value_or(-1.0));
        });
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto filtered = filter_greater_than<"size">(write(command), 6);
        const auto projected = project<BidOnly, "bid">(filtered);
        const auto price_filtered = filter_greater_than<"price">(projected, 15.0);
        return observe(select_column<"price">(price_filtered), tick);
    };
    const auto commands = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 10, 11};
    const auto ticks = std::array<i64, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static_cast<void>(testing::test_graph(graph, commands, ticks));

    require(observations.size() == commands.size() && observations[1].rows.empty() &&
                observations[2].rows == std::vector<row_id_t>{1} &&
                observations[2].bids == std::vector<double>{20.5} &&
                observations[5].rows == std::vector<row_id_t>{1} &&
                observations[8].rows.empty() && observations[8].removed == std::vector<row_id_t>{1} &&
                observations[9].rows == std::vector<row_id_t>{1} &&
                observations[9].changed == std::vector<row_id_t>{1},
            "nested filter and projection views did not propagate selection membership changes");
}

void test_computed_tsf_outputs_and_downstream_views() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        bool source_valid{};
        bool aggregate_valid{};
        bool mid_valid{};
        bool aggregate_modified{};
        bool mid_modified{};
        EvaluationTime mid_last_updated{MIN_DT};
        EvaluationTime mid_storage_change_time{MIN_DT};
        const void *source_row_domain{};
        const void *aggregate_row_domain{};
        const void *source_bid{};
        const void *aggregate_bid{};
        const void *aggregate_mid{};
        const void *copied_bid{};
        const void *copied_mid{};
        std::vector<row_id_t> rows;
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<row_id_t> mid_modified_rows;
        std::vector<row_id_t> computed_slots;
        std::vector<std::optional<double>> mids;
        std::vector<row_id_t> filtered_rows;
        std::vector<std::optional<double>> filtered_mids;
    };
    auto observations = std::vector<observation>{};
    const auto write = node<computed_frame_writer>();
    const auto compute = node<computed_quote_node>();
    const auto copy = node([](In<TSF<ComputedQuote>> input,
                              Out<TSF<ComputedQuote>, frame::computed<"mid">> output) {
        output.bind(input);
        if (!input.valid())
            return;
        input.for_each_row([&](row_id_t id, std::size_t) { output.template set<"mid">(id, input.cell<0>(id)); });
    });
    const auto observe = node([&](In<TSF<Quotes>, optional> source, In<TSF<ComputedQuote>, optional> aggregate,
                                  In<TSF<ComputedQuote>, optional> copied, In<TSC<double>, optional> filtered,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.source_valid = source.valid(),
                                  .aggregate_valid = aggregate.valid(),
                                  .aggregate_modified = aggregate.modified()};
        const auto source_storage = source.valid() ? source.extension_descriptor() : FrameReadStorage{};
        const auto aggregate_storage = aggregate.valid() ? aggregate.extension_descriptor() : FrameReadStorage{};
        result.source_row_domain = source_storage.rows.identity;
        result.aggregate_row_domain = aggregate_storage.rows.identity;
        if (source.valid() && aggregate.valid()) {
            result.source_bid = source_storage.column(source_storage.field_index(util::symbol{"bid"})).native;
            const auto bid_index = aggregate_storage.field_index(util::symbol{"bid"});
            const auto mid_index = aggregate_storage.field_index(util::symbol{"mid"});
            const auto bid_binding = aggregate_storage.column(bid_index);
            const auto mid_binding = aggregate_storage.column(mid_index);
            result.aggregate_bid = bid_binding.native;
            result.aggregate_mid = mid_binding.native;
            result.mid_storage_change_time = static_cast<const ColumnStorage<double> *>(mid_binding.payload)->change_time;
            const auto mid_column = aggregate.column("mid");
            result.mid_valid = mid_column.valid();
            result.mid_modified = mid_column.modified();
            result.mid_last_updated = mid_column.last_updated();
            aggregate.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.mids.push_back(aggregate.cell<0>(id));
                if (mid_binding.physical_rows != nullptr && mid_binding.physical_rows->contains(id))
                    result.computed_slots.push_back(mid_binding.physical_rows->slot_of(id));
            });
            aggregate.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
            aggregate.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
            aggregate.for_each_modified<0>([&](row_id_t id) { result.mid_modified_rows.push_back(id); });
        }
        if (copied.valid()) {
            const auto copied_storage = copied.extension_descriptor();
            result.copied_bid = copied_storage.column(copied_storage.field_index(util::symbol{"bid"})).native;
            result.copied_mid = copied_storage.column(copied_storage.field_index(util::symbol{"mid"})).native;
        }
        if (filtered.valid()) {
            filtered.for_each_row([&](row_id_t id, std::size_t) {
                result.filtered_rows.push_back(id);
                result.filtered_mids.push_back(filtered.cell(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> command, wp<TS<i64>> tick) {
        const auto source = write(command);
        const auto aggregate = compute(source, command);
        const auto copied = copy(aggregate);
        const auto filtered = filter_greater_than<"mid">(copied, 20.0);
        return observe(source, aggregate, copied, select_column<"mid">(filtered), tick);
    };
    const auto commands = std::array<i64, 12>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const auto ticks = commands;
    const auto result = testing::test_graph(graph, commands, ticks);

    require(result.size() == commands.size() && observations.size() == commands.size(),
            "computed TSF graph missed a command cycle");
    require(observations[0].source_valid && observations[0].aggregate_valid && observations[0].mid_valid &&
                observations[0].rows.empty() && observations[1].source_valid &&
                observations[1].aggregate_valid && observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].mids == std::vector<std::optional<double>>{11.0},
            "computed TSF aggregate output did not initialize its rows and computed cells");
    require(observations[1].source_row_domain == observations[1].aggregate_row_domain &&
                observations[1].source_bid == observations[1].aggregate_bid &&
                observations[1].aggregate_mid != observations[1].source_bid &&
                observations[1].copied_bid == observations[1].aggregate_bid &&
                observations[1].copied_mid != observations[1].aggregate_mid &&
                observations[1].computed_slots == std::vector<row_id_t>{0},
            "computed TSF did not borrow source columns and own computed columns");
    require(observations[2].mids == std::vector<std::optional<double>>{12.0} &&
                observations[2].mid_modified_rows == std::vector<row_id_t>{0},
            "computed TSF did not publish a computed cell modification");
    require(observations[3].rows == std::vector<row_id_t>{0, 1} &&
                observations[3].mids == std::vector<std::optional<double>>{std::nullopt, std::nullopt} &&
                observations[3].changed == std::vector<row_id_t>{0, 1} &&
                observations[3].mid_modified_rows == std::vector<row_id_t>{0},
            "computed TSF all-null batch did not preserve nullable row semantics");
    require(observations[4].mids == std::vector<std::optional<double>>{12.0, std::nullopt},
            "computed TSF did not restore its computed cells after an all-null batch");
    require(observations[4].mid_modified_rows == std::vector<row_id_t>{0, 1},
            "computed TSF did not publish the restored computed row delta");
    require(observations[4].filtered_rows.empty(),
            "downstream computed-column filter selected a row below its threshold");
    require(observations[5].aggregate_modified && observations[5].mid_modified &&
                observations[5].mid_last_updated > observations[4].mid_last_updated &&
                observations[5].mid_storage_change_time > observations[4].mid_storage_change_time,
            "set-only computed output did not publish its row delta and advance column timestamps");
    require(observations[6].rows == std::vector<row_id_t>{1} &&
                observations[6].removed == std::vector<row_id_t>{0},
            "computed TSF did not remove computed storage with its source row");
    require(observations[7].rows == std::vector<row_id_t>{0, 1} &&
                observations[7].mids == std::vector<std::optional<double>>{31.0, std::nullopt} &&
                observations[7].computed_slots == std::vector<row_id_t>{0, 1} &&
                observations[7].computed_slots.front() == observations[1].computed_slots.front() &&
                observations[7].filtered_rows == std::vector<row_id_t>{0} &&
                observations[7].filtered_mids == std::vector<std::optional<double>>{31.0},
            "computed TSF did not reuse a removed row ID or support downstream filter and projection");
    require(!observations[8].source_valid && !observations[8].aggregate_valid &&
                observations[8].aggregate_row_domain == nullptr && observations[9].source_valid &&
                observations[9].aggregate_valid && observations[9].mid_valid && observations[9].rows.empty() &&
                observations[10].mids == std::vector<std::optional<double>>{41.0} &&
                observations[11].mids == std::vector<std::optional<double>>{41.0} &&
                observations[11].mid_modified_rows.empty() && !observations[11].mid_modified &&
                observations[11].mid_last_updated == observations[10].mid_last_updated,
            "computed TSF did not invalidate and revalidate with its source");
}

void test_all_computed_tsf_publishes_empty_domain_rebinding() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        const void *selected_row_domain{};
        const void *row_domain{};
        const void *bid_row_domain{};
        bool selected_modified{};
        bool valid{};
        bool modified{};
        bool bid_valid{};
        bool bid_modified{};
        std::vector<row_id_t> changed;
        std::vector<row_id_t> removed;
        std::vector<row_id_t> bid_changed;
        std::vector<row_id_t> bid_removed;
    };
    auto observations = std::vector<observation>{};
    const auto first_empty_source = node([](In<TS<i64>> tick, Out<TSF<Quotes>> output) {
        output.clear();
        static_cast<void>(tick);
    });
    const auto second_empty_source = node([](In<TS<i64>> tick, Out<TSF<Quotes>> output) {
        output.clear();
        static_cast<void>(tick);
    });
    const auto select = node([](In<TSF<Quotes>, optional> first, In<TSF<Quotes>, optional> second,
                                In<TS<i64>> selection, Out<TSF<Quotes>, view> output) {
        if (*selection == 0)
            output.bind_projection(first, columns<"size", "bid", "ask">());
        else
            output.bind_projection(second, columns<"size", "bid", "ask">());
    });
    const auto compute = node([](In<TSF<Quotes>, optional> input, In<TS<i64>> tick,
                                 Out<TSF<Quotes>, computed<"size", "bid", "ask">> output) {
        static_cast<void>(output.bind(input));
        static_cast<void>(tick);
    });
    const auto observe = node([&](In<TSF<Quotes>, optional> selected, In<TSF<Quotes>, optional> input,
                                  In<TSC<double>, optional> bid,
                                  In<TS<i64>> tick, Out<TS<i64>> output) {
        auto result = observation{.selected_row_domain = selected.row_domain(),
                                  .row_domain = input.row_domain(),
                                  .bid_row_domain = bid.row_domain(),
                                  .selected_modified = selected.modified(),
                                  .valid = input.valid(),
                                  .modified = input.modified(),
                                  .bid_valid = bid.valid(),
                                  .bid_modified = bid.modified()};
        input.for_each_changed([&](row_id_t id) { result.changed.push_back(id); });
        input.for_each_removed([&](row_id_t id) { result.removed.push_back(id); });
        bid.for_each_changed([&](row_id_t id) { result.bid_changed.push_back(id); });
        bid.for_each_removed([&](row_id_t id) { result.bid_removed.push_back(id); });
        observations.push_back(std::move(result));
        output.set(*tick);
    });
    const auto graph = [&](wp<TS<i64>> tick, wp<TS<i64>> selection) {
        const auto first = first_empty_source(tick);
        const auto second = second_empty_source(tick);
        const auto selected = select(first, second, selection);
        const auto computed = compute(selected, tick);
        return observe(selected, computed, select_column<"bid">(computed), tick);
    };
    const auto ticks = std::array<i64, 4>{0, 1, 2, 3};
    const auto selections = std::array<i64, 4>{0, 0, 1, 1};
    static_cast<void>(testing::test_graph(graph, ticks, selections));

    require(observations.size() == ticks.size() && observations[0].valid && observations[0].bid_valid &&
                observations[0].row_domain != nullptr && observations[0].bid_row_domain == observations[0].row_domain &&
                observations[0].selected_row_domain == observations[0].row_domain &&
                observations[1].selected_row_domain == observations[0].selected_row_domain &&
                observations[1].row_domain == observations[0].row_domain && !observations[1].selected_modified &&
                !observations[1].modified && !observations[1].bid_modified &&
                observations[2].selected_row_domain != observations[0].selected_row_domain &&
                observations[2].selected_modified && observations[2].row_domain != observations[0].row_domain &&
                observations[2].bid_row_domain == observations[2].row_domain && observations[2].modified &&
                observations[2].bid_modified && observations[2].changed.empty() &&
                observations[2].removed.empty() && observations[2].bid_changed.empty() &&
                observations[2].bid_removed.empty() &&
                observations[3].selected_row_domain == observations[2].selected_row_domain &&
                observations[3].row_domain == observations[2].row_domain && !observations[3].selected_modified &&
                !observations[3].modified && !observations[3].bid_modified,
            "all-computed TSF or its selected child missed an empty row-domain change");
}

void test_computed_tsf_callable_and_aggregate_runtime_hooks() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        const TimeSeriesRuntimeType *mid_runtime{};
        const TimeSeriesRuntimeType *bid_runtime{};
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> mids;
        std::vector<std::optional<double>> bids;
    };
    auto observations = std::vector<observation>{};
    auto callable_runtime_selections = std::size_t{};
    computed_bid_aggregate::runtime_selections = 0;

    const auto source = node([](In<TS<i64>> command, Out<TSF<ComputedQuote>> output) {
        if (*command == 0) {
            output.clear();
            return;
        }
        static_cast<void>(output.add(
            ComputedQuote<Cell>{.mid = Cell<double>{0.0}, .ask = Cell<double>{12.0}, .bid = Cell<double>{10.0}}));
    });
    const auto compute_mid = node(
        eval = [](In<TSF<ComputedQuote>> input, Out<TSF<ComputedQuote>, frame::computed<"mid">> output) {
            output.bind(input);
            if (!input.valid())
                return;
            input.for_each_row([&](row_id_t id, std::size_t) {
                const auto bid = input.cell<2>(id);
                const auto ask = input.cell<1>(id);
                if (bid && ask)
                    output.template set<"mid">(id, (*bid + *ask) / 2.0);
            });
        },
        output_runtime_type = [&](const WiringArguments &arguments) -> const TimeSeriesRuntimeType & {
            ++callable_runtime_selections;
            return frame_computed_time_series_runtime_type<ComputedQuote>(
                *arguments.resolved_output_type(), computed_option<"mid">{});
        });
    const auto compute_bid = node<computed_bid_aggregate>();
    const auto observe = node([&](In<TSF<ComputedQuote>, optional> input,
                                  In<TSF<ComputedQuote>, optional> mid,
                                  In<TSF<ComputedQuote>, optional> bid,
                                  In<TS<i64>> command, Out<TS<i64>> output) {
        auto result = observation{};
        if (input.valid() && mid.valid() && bid.valid()) {
            result.mid_runtime = std::addressof(mid.runtime_type());
            result.bid_runtime = std::addressof(bid.runtime_type());
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.mids.push_back(mid.cell<0>(id));
                result.bids.push_back(bid.cell<2>(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(*command);
    });
    const auto graph = [&](wp<TS<i64>> command) {
        const auto frame = source(command);
        return observe(frame, compute_mid(frame), compute_bid(frame), command);
    };
    const auto commands = std::array<i64, 2>{0, 1};
    static_cast<void>(testing::test_graph(graph, commands));

    const auto &series_type = TSF<ComputedQuote>::type();
    const auto &mid_runtime = frame_computed_time_series_runtime_type<ComputedQuote>(
        series_type, computed_option<"mid">{});
    const auto &bid_runtime = frame_computed_time_series_runtime_type<ComputedQuote>(
        series_type, computed_option<"bid">{});
    require(observations.size() == commands.size() && callable_runtime_selections == 1 &&
                computed_bid_aggregate::runtime_selections == 1 &&
                observations[1].mid_runtime == std::addressof(mid_runtime) &&
                observations[1].bid_runtime == std::addressof(bid_runtime) &&
                observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].mids == std::vector<std::optional<double>>{11.0} &&
                observations[1].bids == std::vector<std::optional<double>>{20.0},
            "callable and aggregate output hooks selected incorrect computed TSF layouts");
}

void test_computed_tsf_runtime_cache_and_source_rebinding() {
    using namespace bg;
    using namespace bg::frame;

    const auto &series_type = frame_time_series_type<ComputedQuote>(TimeSeriesGuarantee::ordinary);
    const auto &runtime = frame_computed_time_series_runtime_type<ComputedQuote>(
        series_type, computed_option<"mid", "bid">{});
    const auto &same_runtime = frame_computed_time_series_runtime_type<ComputedQuote>(
        series_type, computed_option<"bid", "mid">{});
    require(std::addressof(runtime) == std::addressof(same_runtime),
            "computed TSF runtime cache depended on computed field name order");

    auto missing_rejected = false;
    try {
        static_cast<void>(frame_computed_time_series_runtime_type<ComputedQuote>(
            series_type, computed_option<"missing">{}));
    } catch (const std::invalid_argument &) {
        missing_rejected = true;
    }
    auto duplicate_rejected = false;
    try {
        static_cast<void>(frame_computed_time_series_runtime_type<ComputedQuote>(
            series_type, computed_option<"mid", "mid">{}));
    } catch (const std::invalid_argument &) {
        duplicate_rejected = true;
    }
    require(missing_rejected && duplicate_rejected,
            "computed TSF runtime accepted a missing or duplicate computed field name");

    auto memory = allocator{};
    auto first = NativeFrame<Quotes>{};
    auto second = NativeFrame<Quotes>{};
    auto release_frames = util::scope_exit{[&]() noexcept {
        first.release(memory);
        second.release(memory);
    }};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    const auto second_time = first_time + EvaluationTime::duration{1};
    const auto first_id = first.add(
        Quotes<Cell>{.size = Cell<i64>{1}, .bid = Cell<double>{10.0}, .ask = Cell<double>{12.0}}, first_time,
        memory);
    const auto second_id = second.add(
        Quotes<Cell>{.size = Cell<i64>{2}, .bid = Cell<double>{20.0}, .ask = Cell<double>{22.0}}, second_time,
        memory);
    require(first_id == second_id && first.rows().identity() != second.rows().identity(),
            "computed TSF rebind test did not create equal row IDs in distinct source domains");

    auto *allocation = ::operator new(runtime.value_size(), std::align_val_t{runtime.value_alignment()});
    runtime.construct_initial_value(allocation);
    auto release_output = util::scope_exit{[&]() noexcept {
        runtime.destroy_value(allocation, memory);
        ::operator delete(allocation, std::align_val_t{runtime.value_alignment()});
    }};
    auto &output = *static_cast<RuntimeFrameViewOutputStorage *>(allocation);
    auto first_input = native_frame_input(first);
    require(output.bind(first_input, {}, std::nullopt, nullptr, nullptr, first_time, memory),
            "computed TSF output did not bind its first source domain");
    auto first_mid = std::optional<double>{11.0};
    require(output.set_computed(0, first_id, std::addressof(first_mid), first_time, memory),
            "computed TSF output did not write its first computed cell");
    auto first_bid = std::optional<double>{10.5};
    require(output.set_computed(2, first_id, std::addressof(first_bid), first_time, memory),
            "computed TSF output did not write its second computed column");
    const auto first_output_storage = static_cast<const FrameSeriesRuntimeType &>(runtime).input_storage(allocation);
    const auto first_mid_binding = first_output_storage.column(0);
    const auto first_bid_binding = first_output_storage.column(2);
    require(first_mid_binding.physical_rows == first_bid_binding.physical_rows &&
                first_mid_binding.physical_rows->slot_of(first_id) == 0 &&
                first_output_storage.generic_column(0).view<double>().cell(0) == 11.0 &&
                first_output_storage.generic_column(2).view<double>().cell(0) == 10.5,
            "computed TSF columns did not share their row-to-slot mapping");

    auto second_input = native_frame_input(second);
    require(output.bind(second_input, {}, std::nullopt, nullptr, nullptr, second_time, memory) &&
                output.added_rows.contains(second_id) && output.removed_rows.contains(first_id),
            "computed TSF output did not report a source row-domain rebind");
    const auto output_storage = static_cast<const FrameSeriesRuntimeType &>(runtime).input_storage(allocation);
    require(output_storage.rows.count() == 1 && output_storage.rows.has(second_id) &&
                !output_storage.generic_column(0).view<double>().cell(0),
            "computed TSF source-domain rebind retained a prior source cell");
    auto second_mid = std::optional<double>{21.0};
    require(output.set_computed(0, second_id, std::addressof(second_mid), second_time, memory) &&
                output_storage.generic_column(0).view<double>().cell(0) == 21.0 &&
                output_storage.column(0).physical_rows == output_storage.column(2).physical_rows,
            "computed TSF output did not accept a computed cell after source-domain rebind");
}

void test_runtime_schema_computed_tsf_output() {
    using namespace bg;
    using namespace bg::frame;

    struct observation {
        std::vector<row_id_t> rows;
        std::vector<std::optional<double>> bids;
        std::vector<std::optional<double>> asks;
        std::vector<std::optional<double>> mids;
        bool computed_columns_share_slots{};
    };
    auto observations = std::vector<observation>{};
    const auto schema_fields = std::array{
        SchemaField{util::symbol{"bid"}, &cpp_scalar_type<double>()},
        SchemaField{util::symbol{"ask"}, &cpp_scalar_type<double>()},
        SchemaField{util::symbol{"mid"}, &cpp_scalar_type<double>()},
    };
    const auto &schema = anonymous_schema(schema_fields);
    auto empty_bid = Column<double>{std::vector<std::optional<double>>{}};
    auto empty_ask = Column<double>{std::vector<std::optional<double>>{}};
    auto empty_mid = Column<double>{std::vector<std::optional<double>>{}};
    const auto seed_columns = std::array{ColumnView<>{empty_bid.view()}, ColumnView<>{empty_ask.view()},
                                         ColumnView<>{empty_mid.view()}};
    auto memory = allocator{};
    auto seed = make_frame_value(memory, schema, 0, seed_columns);
    auto erased_seed = ScalarValue<>{seed, memory};
    const auto commands = std::array<i64, 2>{0, 1};
    auto seed_ticks = std::vector<std::optional<ScalarValue<>>>{};
    auto command_ticks = std::vector<std::optional<ScalarValue<>>>{};
    for (const auto command : commands) {
        seed_ticks.emplace_back(erased_seed.view());
        command_ticks.emplace_back(ScalarValue<i64>{command});
    }
    const auto inputs = std::array{
        testing::TestGraphInput{erased_seed.runtime_type(), seed_ticks},
        testing::TestGraphInput{cpp_scalar_runtime_type<i64>(), command_ticks},
    };

    const auto source = node([](In<TS<Frame<SCHEMA>>> schema_seed, In<TS<i64>> command, Out<TSF<SCHEMA>> output) {
        static_cast<void>(schema_seed);
        if (*command == 0) {
            output.clear();
            return;
        }
        const auto bid_value = 10.0;
        const auto ask_value = 12.0;
        const auto bid = Scalar<double>::from(bid_value);
        const auto ask = Scalar<double>::from(ask_value);
        const auto null_mid = std::optional<Scalar<>>{};
        const auto row = std::array<std::optional<Scalar<>>, 3>{Scalar<>::from(bid), Scalar<>::from(ask), null_mid};
        static_cast<void>(output.add(row));
    });
    const auto compute = node([](In<TSF<SCHEMA>> input,
                                 Out<TSF<SCHEMA>, frame::computed<"mid", "bid">> output) {
        output.bind(input);
        if (!input.valid())
            return;

        const auto bids = input.column("bid").view<double>();
        const auto asks = input.column("ask").view<double>();
        auto ids = std::vector<row_id_t>{};
        auto bids_out = std::vector<double>{};
        auto mids_out = std::vector<double>{};
        input.for_each_row([&](row_id_t id, std::size_t) {
            ids.push_back(id);
            const auto bid = bids.cell(id);
            const auto ask = asks.cell(id);
            bids_out.push_back(bid.value_or(0.0) + 1.0);
            mids_out.push_back((bid.value_or(0.0) + ask.value_or(0.0)) / 2.0);
        });
        output.template set_batch<"bid">(
            ids, ColumnBatch<double>{0, ids.size(), bids_out.data(), ColumnBatch<double>::Validity::all_valid()});
        output.template set_batch<"mid">(
            ids, ColumnBatch<double>{0, ids.size(), mids_out.data(), ColumnBatch<double>::Validity::all_valid()});
    });
    const auto observe = node([&](In<TSF<SCHEMA>> input, In<TS<i64>> command, Out<TS<i64>> output) {
        auto result = observation{};
        if (input.valid()) {
            const auto bids = input.column("bid").view<double>();
            const auto asks = input.column("ask").view<double>();
            const auto mids = input.column("mid").view<double>();
            const auto frame = input.extension_descriptor();
            const auto bid_binding = frame.column(frame.field_index(util::symbol{"bid"}));
            const auto mid_binding = frame.column(frame.field_index(util::symbol{"mid"}));
            result.computed_columns_share_slots = bid_binding.physical_rows == mid_binding.physical_rows;
            input.for_each_row([&](row_id_t id, std::size_t) {
                result.rows.push_back(id);
                result.bids.push_back(bids.cell(id));
                result.asks.push_back(asks.cell(id));
                result.mids.push_back(mids.cell(id));
            });
        }
        observations.push_back(std::move(result));
        output.set(*command);
    });
    using namespace bg::literals;
    const auto graph = bg::graph("name"_n = "runtime_schema_computed_tsf_output", [&](wp<TS<Frame<SCHEMA>>> seed_input,
                                                                                           wp<TS<i64>> command) {
        return observe(compute(source(seed_input, command)), command);
    });
    const auto result = testing::test_graph(graph, std::span<const testing::TestGraphInput>{inputs});
    require(result.size() == commands.size() && observations.size() == commands.size() &&
                observations[1].rows == std::vector<row_id_t>{0} &&
                observations[1].bids == std::vector<std::optional<double>>{11.0} &&
                observations[1].asks == std::vector<std::optional<double>>{12.0} &&
                observations[1].mids == std::vector<std::optional<double>>{11.0} &&
                observations[1].computed_columns_share_slots,
            "runtime-schema computed TSF output did not materialize its fields on shared row slots");
}

} // namespace

int main() {
    try {
        test_scalar_literals_and_content_equality();
        test_runtime_schema_and_malformed_frame_literals();
        test_frame_literals_as_ordinary_time_series_inputs();
        test_base_and_derived_frame_equality();
        test_frame_key_schema_validation();
        test_frame_scalar_key_validation();
        test_native_frame_composite_keys();
        test_native_frame_key_insert_rolls_back();
        test_native_frame_key_removal_retention_rolls_back();
        test_runtime_native_storage_mixed_and_zero_column_schemas();
        test_native_storage_validates_row_and_patch_shape_before_mutation();
        test_native_storage_changed_columns_beyond_inline_capacity();
        test_shared_rows_and_column_changes();
        test_failed_multicolumn_insert_cleanup();
        test_recycled_rows_reuse_physical_slots();
        test_failed_value_insert_rolls_back_reused_slot();
        test_frame_scalar_views_and_snapshots();
        test_runtime_frame_value_factory();
        test_inherited_frame_fields_and_views();
        test_scalar_frame_time_series_graph();
        test_compiled_graph_runtime_storage_is_independent();
        test_tsf_graph_behavior();
        test_tsf_literal_updates();
        test_tsf_literal_uses_actual_derived_schema();
        test_tsf_literal_unkeyed_append_and_typed_output();
        test_tsf_literal_anonymous_composite_key();
        test_tsf_literal_rejections();
        test_keyed_tsf_delta_capture();
        test_keyed_tsf_capture_treats_omitted_added_cells_as_null();
        test_keyed_tsf_capture_uses_actual_derived_schema();
        test_keyed_tsf_capture_uses_keyed_runtime_schema_for_unkeyed_base_output();
        test_keyed_tsf_capture_uses_runtime_schema();
        test_keyed_tsf_capture_supports_null_composite_keys();
        test_keyed_tsf_capture_invalidates_and_revalidates_without_old_deltas();
        test_keyed_tsf_capture_rejects_missing_actual_patch_fields();
        test_borrowed_tsf_input_with_native_and_view_runtimes();
        test_generic_tsf_input_schema_and_columns();
        test_generic_declared_tsf_rows_follow_resolved_cell_type();
        test_repeated_and_constrained_generic_tsf_fields_match_runtime_schema();
        test_typed_patch_rows_insert_nullable_rows();
        test_runtime_schema_tsf_writes_and_named_projection();
        test_inherited_tsf_graph_behavior();
        test_base_frame_output_retains_derived_schema();
        test_tsf_projection_view();
        test_tsf_projection_empty_columns_match_target_schema();
        test_tsf_projection_selection_change();
        test_tsf_projection_rejects_invalid_mappings_before_invalidation();
        test_tsf_projection_rejects_empty_row_domain_mismatch();
        test_tsf_projection_rejects_native_columns_from_another_logical_domain();
        test_view_updates_do_not_traverse_source_rows();
        test_filter_row_set_reads_cached_source_order();
        test_tsf_view_uses_local_physical_rows_incrementally();
        test_filtered_column_coverage_rejects_a_different_row_set();
        test_incremental_numeric_filter();
        test_double_column_addition_updates_and_nulls();
        test_double_column_addition_crosses_scratch_batch_boundaries();
        test_double_column_addition_accepts_projected_columns_with_shared_selection();
        test_double_column_addition_rejects_different_domains_and_membership();
        test_double_column_addition_rejects_mixed_integer_columns();
        test_double_column_addition_leaves_scalar_addition_to_bg();
        test_double_column_addition_broadcasts_owned_scalar_values();
        test_double_column_addition_parameter_updates_recompute_rows();
        test_double_column_addition_parameter_invalidation_and_revalidation();
        test_double_column_addition_deduplicates_repeated_parameter_inputs();
        test_mixed_column_expression_fuses_and_collects_parameters();
        test_bound_column_expression_owns_captures_and_binds_parameters();
        test_double_column_addition_reuses_expression_and_sparse_rows();
        test_double_column_addition_reuses_forced_materialisation_form();
        test_double_column_arithmetic_fusion_and_forced_materialization();
        test_double_column_addition_preserves_null_cells();
        test_filter_greater_than_composes_with_column_projection();
        test_computed_tsf_outputs_and_downstream_views();
        test_all_computed_tsf_publishes_empty_domain_rebinding();
        test_computed_tsf_callable_and_aggregate_runtime_hooks();
        test_computed_tsf_runtime_cache_and_source_rebinding();
        test_runtime_schema_computed_tsf_output();
        test_scalar_variable_frame_input();
    } catch (const std::exception &error) {
        std::cerr << "bg_frame_frame_test: " << error.what() << '\n';
        return 1;
    }
}

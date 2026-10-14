#include <literal_input/scalar_literal.hpp>
#include <literal_input/construct.hpp>

#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/value.hpp>
#include "test_scalar_provider.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>


#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {

struct ReferenceAggregate {
    bg::i64 quantity;
    double price;
};

static_assert(bg::native_struct<ReferenceAggregate>);
static_assert(std::same_as<typename bg::scalar_storage_traits<ReferenceAggregate>::value_type,
                           const ReferenceAggregate &>);

std::atomic_size_t allocation_count{};
std::atomic_bool count_allocations{};
std::size_t free_function_visits{};

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

template <typename T> T read_value(const bg::ScalarData &source) {
    alignas(T) std::byte storage[sizeof(T)];
    source.runtime_type().read_value(source.mode(), source.data_word(), storage);
    auto *value = std::launder(reinterpret_cast<T *>(storage));
    auto result = std::move(*value);
    std::destroy_at(value);
    return result;
}

void free_function_visitor(const bg::Scalar<std::int64_t> *source) {
    require(source->value() == 41, "function pointer visitor received the wrong value");
    ++free_function_visits;
}

void visit_tree(const literal_input::ScalarLiteral &element, bg::ScalarVisitTypes &scalar_types,
                std::int64_t &sum, std::size_t &leaves) {
    element.visit(
        [&](const literal_input::ScalarLiteralItem &source) {
            source.accept(scalar_types);
        },
        [&](std::span<const literal_input::ScalarLiteral> children) {
            for (const auto &child : children) {
                visit_tree(child, scalar_types, sum, leaves);
            }
        });
}

template <template <typename, auto...> typename Field> struct ProbeBundle {};

struct NoncopyableLeafVisitor {
    int *calls;

    explicit NoncopyableLeafVisitor(int *calls) noexcept : calls{calls} {}
    NoncopyableLeafVisitor(const NoncopyableLeafVisitor &) = delete;
    NoncopyableLeafVisitor(NoncopyableLeafVisitor &&) noexcept = default;

    void operator()(const literal_input::ScalarLiteralItem &) const { ++*calls; }
};

struct ShapeStats {
    std::size_t sequences{};
    std::size_t empty_sequences{};
    std::size_t leaves{};
};

class CountingMemoryResource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t outstanding{};

  private:
    void *do_allocate(std::size_t size, std::size_t alignment) override {
        auto *result = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++allocations;
        ++outstanding;
        return result;
    }

    void do_deallocate(void *value, std::size_t size, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, size, alignment);
        ++deallocations;
        --outstanding;
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

struct CopyTracked {
    explicit CopyTracked(int value) noexcept : value{value} {}
    CopyTracked(const CopyTracked &other) noexcept : value{other.value} { ++copies; }
    CopyTracked(CopyTracked &&) noexcept = default;
    ~CopyTracked() { ++destructions; }

    int value;
    static inline std::size_t copies{};
    static inline std::size_t destructions{};
};

struct ListItemConstruction {
    const bg::ScalarData *source;
    std::size_t calls{};
    std::size_t fail_on{};

    static void callback(void *context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                         bg::allocator &memory) {
        auto &construction = *static_cast<ListItemConstruction *>(context);
        ++construction.calls;
        if (construction.calls == construction.fail_on) {
            throw std::runtime_error{"list item construction failed"};
        }
        runtime_type.construct_from_input(destination, *construction.source, memory);
    }
};

void inspect_shape(const literal_input::ScalarLiteral &element, ShapeStats &stats) {
    element.visit(
        [&](const literal_input::ScalarLiteralItem &) { ++stats.leaves; },
        [&](std::span<const literal_input::ScalarLiteral> children) {
            ++stats.sequences;
            if (children.empty()) {
                ++stats.empty_sequences;
            }
            for (const auto &child : children) {
                inspect_shape(child, stats);
            }
        });
}

ShapeStats inspect_shape(const literal_input::ScalarLiteral &element) {
    auto stats = ShapeStats{};
    inspect_shape(element, stats);
    return stats;
}

void require_same_descriptor(const literal_input::ScalarLiteralItem &source, const bg::ScalarData &expected) {
    source.visit([&](auto *view) {
        const auto &actual = bg::details::scalar_data(*view);
        require(&actual.runtime_type() == &expected.runtime_type(), "runtime type identity was not preserved");
        require(actual.data_word() == expected.data_word(), "scalar data pointer was not aliased");
        require(actual.mode() == expected.mode(), "scalar data mode was not preserved");
    });
}

void verify_scalar_dispatch() {
    using namespace test_scalar_provider;

    auto builtin_called = false;
    auto byte_called = false;
    auto boolean_called = false;
    auto double_called = false;
    auto date_time_called = false;
    auto timedelta_called = false;
    auto string_called = false;
    auto symbol_called = false;
    auto list_called = false;
    auto set_called = false;
    auto dict_called = false;
    auto struct_called = false;
    auto hierarchical_called = false;
    auto extension_called = false;
    auto visitor = ddv::serial{
        [&](const bg::Scalar<bg::byte> *view) { byte_called = view->value() == std::byte{0x2a}; },
        [&](const bg::Scalar<bool> *view) { boolean_called = view->value(); },
        [&](const bg::Scalar<bg::i64> *view) {
            builtin_called = view->value() == 41;
        },
        [&](const bg::Scalar<double> *view) { double_called = view->value() == 2.5; },
        [&](const bg::Scalar<bg::date_time_t> *view) {
            date_time_called = view->value() == bg::date_time_t{std::chrono::microseconds{8}};
        },
        [&](const bg::Scalar<bg::timedelta_t> *view) {
            timedelta_called = view->value() == bg::timedelta_t{9};
        },
        [&](const bg::Scalar<bg::string> *view) { string_called = view->value() == "literal"; },
        [&](const bg::Scalar<bg::symbol> *view) { symbol_called = view->value().text() == "name"; },
        [&](const bg::Scalar<bg::List<bg::SCALAR>> *view) {
            list_called = view->size() == 1;
        },
        [&](const bg::Scalar<bg::Set<bg::SCALAR>> *view) {
            set_called = view->size() == 1;
        },
        [&](const bg::Scalar<bg::Dict<bg::SCALAR, bg::SCALAR>> *view) {
            dict_called = view->size() == 1;
        },
        [&](const bg::Scalar<bg::STRUCT> *) { struct_called = true; },
        [&](const bg::Scalar<bg::HIERARCHICAL> *view) {
            hierarchical_called = view->field_count() == 0;
        },
        [&](const bg::Scalar<> *view) {
            const auto read = bg::scalar_storage_traits<value>::read_value(bg::details::scalar_data(*view));
            extension_called = read.get() == 42;
        },
        ddv::noop,
    };

    const auto byte = bg::ScalarValue<bg::byte>{std::byte{0x2a}};
    const auto boolean = bg::ScalarValue<bool>{true};
    const auto builtin = bg::ScalarValue<bg::i64>{41};
    const auto real = bg::ScalarValue<double>{2.5};
    const auto date_time = bg::ScalarValue<bg::date_time_t>{bg::date_time_t{std::chrono::microseconds{8}}};
    const auto timedelta = bg::ScalarValue<bg::timedelta_t>{bg::timedelta_t{9}};
    const auto string = bg::ScalarValue<bg::string>{"literal"};
    const auto symbol = bg::ScalarValue<bg::symbol>{bg::symbol{"name"}};
    const auto list = bg::ScalarValue<bg::List<bg::i64>>{std::vector<bg::i64>{1}};
    const auto set = bg::ScalarValue<bg::Set<bg::i64>>{std::vector<bg::i64>{2}};
    const auto dict = bg::ScalarValue<bg::Dict<bg::i64, bg::i64>>{
        std::vector<std::pair<bg::i64, bg::i64>>{{3, 4}}};
    const auto structure = bg::ScalarValue<compound_value>{compound_value{.quantity = 5, .price = 6.0}};
    auto memory = bg::allocator{};
    const auto hierarchical = bg::bundle<ProbeBundle>(memory);
    const auto provider_value = value{42};

    literal_input::ScalarLiteralItem{byte}.visit(visitor);
    literal_input::ScalarLiteralItem{boolean}.visit(visitor);
    literal_input::ScalarLiteralItem{builtin}.visit(visitor);
    literal_input::ScalarLiteralItem{real}.visit(visitor);
    literal_input::ScalarLiteralItem{date_time}.visit(visitor);
    literal_input::ScalarLiteralItem{timedelta}.visit(visitor);
    literal_input::ScalarLiteralItem{string}.visit(visitor);
    literal_input::ScalarLiteralItem{symbol}.visit(visitor);
    literal_input::ScalarLiteralItem{list}.visit(visitor);
    literal_input::ScalarLiteralItem{set}.visit(visitor);
    literal_input::ScalarLiteralItem{dict}.visit(visitor);
    literal_input::ScalarLiteralItem{structure}.visit(visitor);
    literal_input::ScalarLiteralItem{hierarchical}.visit(visitor);
    literal_input::ScalarLiteralItem{provider_value}.visit(visitor);

    require(byte_called, "byte scalar view was not dispatched");
    require(boolean_called, "boolean scalar view was not dispatched");
    require(builtin_called, "builtin scalar view was not dispatched");
    require(double_called, "double scalar view was not dispatched");
    require(date_time_called, "date-time scalar view was not dispatched");
    require(timedelta_called, "time-delta scalar view was not dispatched");
    require(string_called, "string scalar view was not dispatched");
    require(symbol_called, "symbol scalar view was not dispatched");
    require(list_called, "list scalar view was not dispatched");
    require(set_called, "set scalar view was not dispatched");
    require(dict_called, "dictionary scalar view was not dispatched");
    require(struct_called, "struct scalar view was not dispatched");
    require(hierarchical_called, "hierarchical scalar view was not dispatched");
    require(extension_called, "extension scalar fallback was not dispatched through borrow_value");

    const auto native_structure = ReferenceAggregate{.quantity = 11, .price = 12.5};
    literal_input::ScalarLiteralItem{native_structure}.visit(
        [&](const bg::Scalar<bg::STRUCT> *view) {
            const auto &descriptor = bg::details::scalar_data(*view);
            require(descriptor.data_word() == reinterpret_cast<std::uintptr_t>(&native_structure),
                    "reference-valued native struct source was copied instead of borrowed");
            require(descriptor.mode() == bg::ScalarDataMode::source_view,
                    "reference-valued native struct source used an unexpected data mode");
        },
        ddv::noop);

    const auto same_type_provider = copy_only_value{73};
    literal_input::ScalarLiteralItem{same_type_provider}.visit(
        [&](const bg::Scalar<> *view) {
            const auto &descriptor = bg::details::scalar_data(*view);
            require(descriptor.data_word() == reinterpret_cast<std::uintptr_t>(&same_type_provider),
                    "same-type provider source was copied instead of borrowed");
            require(descriptor.mode() == bg::ScalarDataMode::source_view,
                    "same-type provider source used an unexpected data mode");
        },
        ddv::noop);
}

void verify_literal_construction() {
    using literal_input::ScalarLiteral;
    using Inner = bg::List<bg::i64>;
    using Nested = bg::List<Inner>;

    const auto &item_runtime = bg::cpp_scalar_runtime_type<bg::i64>();
    const auto &inner_type = bg::list_scalar_type(item_runtime.type());
    const auto inner_available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
    const auto &composed_inner = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(inner_type, inner_available));
    const auto &outer_type = bg::list_scalar_type(inner_type);
    const auto outer_available = std::array<const bg::ScalarRuntimeType *, 1>{&composed_inner};
    const auto &composed_outer = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(outer_type, outer_available));

    auto resource = CountingMemoryResource{};
    auto memory = bg::allocator{resource};
    {
        const auto runtime_only = literal_input::construct(composed_outer, {{1, 2}, {}, {3, 4}}, memory);
        const auto typed = literal_input::construct<Nested>({{1, 2}, {}, {3, 4}}, memory);
        const auto runtime_view = bg::unchecked_scalar_cast<Nested>(runtime_only.view());
        require(runtime_view.size() == 3 && runtime_view[0].size() == 2 &&
                    runtime_view[0][1].value() == 2 && runtime_view[1].empty() &&
                    runtime_view[2][1].value() == 4,
                "runtime-only nested list construction produced the wrong values");
        require(typed.view().size() == 3, "nested literal produced the wrong outer list size");
        require(typed.view()[0].size() == 2 && typed.view()[0][1].value() == 2,
                "nested literal produced the wrong first list");
        require(typed.view()[1].empty(), "empty nested literal was not preserved");
        require(typed.view()[2][1].value() == 4, "nested literal produced the wrong final value");

        const auto empty = literal_input::construct<bg::List<bg::i64>>(ScalarLiteral{}, memory);
        require(empty.view().empty(), "empty literal did not construct an empty list");
        const auto nested_empty = literal_input::construct<Nested>(ScalarLiteral{{}, {}}, memory);
        require(nested_empty.view().size() == 2 && nested_empty.view()[0].empty() &&
                    nested_empty.view()[1].empty(),
                "nested empty lists were not preserved");

        const auto deeper = literal_input::construct<bg::List<bg::List<Inner>>>(
            ScalarLiteral{{{1, 2}, {}}, {}, {{3}}}, memory);
        require(deeper.view().size() == 3 && deeper.view()[0].size() == 2 &&
                    deeper.view()[0][0].size() == 2 && deeper.view()[0][1].empty() &&
                    deeper.view()[1].empty() && deeper.view()[2][0][0].value() == 3,
                "three-level list literal did not preserve its nested shape");

        const auto text = literal_input::construct<bg::List<bg::string>>(
            ScalarLiteral{std::string{"temporary text"}, std::string{"another temporary"}}, memory);
        require(text.view().size() == 2 && text.view()[0].value() == "temporary text" &&
                    text.view()[1].value() == "another temporary",
                "temporary strings were not copied into owned storage");

        auto shape_mismatch_threw = false;
        try {
            (void)literal_input::construct<bg::i64>(ScalarLiteral{1, 2}, memory);
        } catch (const std::invalid_argument &) {
            shape_mismatch_threw = true;
        }
        require(shape_mismatch_threw, "a sequence accepted a scalar destination");

        auto child_failure_threw = false;
        try {
            (void)literal_input::construct<bg::List<bg::i64>>(
                ScalarLiteral{std::string{"not an integer"}}, memory);
        } catch (const std::invalid_argument &) {
            child_failure_threw = true;
        }
        require(child_failure_threw, "an incompatible list child was accepted");
        require(resource.outstanding != 0, "live values lost their allocator-owned storage");
    }
    require(resource.outstanding == 0, "nested scalar values leaked allocator-owned storage");

    using ProviderValue = test_scalar_provider::value;
    const auto &provider_runtime = test_scalar_provider::preserving_runtime_type();
    const auto &provider_type = bg::list_scalar_type(provider_runtime.type());
    const auto provider_available = std::array<const bg::ScalarRuntimeType *, 1>{&provider_runtime};
    const auto &composed_provider = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(provider_type, provider_available));
    auto provider_storage = test_scalar_provider::storage{.number = 47};
    const auto provider_view = bg::Scalar<ProviderValue>::from(provider_runtime, &provider_storage);
    const auto provider_result = literal_input::construct(composed_provider, ScalarLiteral{provider_view});
    const auto provider_list = bg::unchecked_scalar_cast<bg::List<ProviderValue>>(provider_result.view());
    require(&provider_list[0].runtime_type() == &provider_runtime,
            "runtime-only list construction lost the provider runtime identity");
    require(provider_list[0].value().get() == 47, "runtime-only provider list changed the leaf value");

    const auto raw_provider_result = literal_input::construct(
        test_scalar_provider::immutable_snapshot_runtime_type(),
        ScalarLiteral(ProviderValue{.number = 51}));
    require(&raw_provider_result.runtime_type() == &test_scalar_provider::immutable_snapshot_runtime_type(),
            "runtime-only construction lost the requested provider representation");
    require(bg::unchecked_scalar_cast<ProviderValue>(raw_provider_result.view()).value().get() == 51,
            "raw provider value was not constructed into the requested runtime representation");

    auto alternative_storage = test_scalar_provider::storage{.number = 53};
    const auto alternative_source = bg::Scalar<ProviderValue>::from(
        test_scalar_provider::canonicalizing_runtime_type(), &alternative_storage);
    const auto alternative_result = literal_input::construct(
        test_scalar_provider::immutable_snapshot_runtime_type(), ScalarLiteral(alternative_source));
    require(bg::details::scalar_data(alternative_source).mode() == bg::ScalarDataMode::stored_view &&
                &bg::details::scalar_data(alternative_source).runtime_type() ==
                    &test_scalar_provider::canonicalizing_runtime_type(),
            "alternative provider source did not retain its runtime representation");
    require(&alternative_result.runtime_type() == &test_scalar_provider::immutable_snapshot_runtime_type() &&
                bg::unchecked_scalar_cast<ProviderValue>(alternative_result.view()).value().get() == 53,
            "alternative provider source was not applied to the requested destination runtime");

    using Item = bg::Opaque<CopyTracked>;
    using ItemList = bg::List<Item>;
    auto sources = std::array{
        bg::ScalarValue<Item>{CopyTracked{0}}, bg::ScalarValue<Item>{CopyTracked{1}},
        bg::ScalarValue<Item>{CopyTracked{2}}, bg::ScalarValue<Item>{CopyTracked{3}},
        bg::ScalarValue<Item>{CopyTracked{4}}, bg::ScalarValue<Item>{CopyTracked{5}},
        bg::ScalarValue<Item>{CopyTracked{6}}, bg::ScalarValue<Item>{CopyTracked{7}},
        bg::ScalarValue<Item>{CopyTracked{8}},
    };
    CopyTracked::copies = 0;
    auto tracked_resource = CountingMemoryResource{};
    auto tracked_memory = bg::allocator{tracked_resource};
    {
        const auto tracked = literal_input::construct<ItemList>(
            ScalarLiteral{sources[0], sources[1], sources[2], sources[3], sources[4],
                          sources[5], sources[6], sources[7], sources[8]},
            tracked_memory);
        require(CopyTracked::copies == 9, "literal construction made an intermediate or duplicate leaf copy");
        require(tracked.view().size() == 9 && tracked.view()[8].value().value == 8,
                "literal construction lost a leaf beyond the first bunch block");
        require(tracked_resource.allocations == 4,
                "nine scalar leaves did not use one owner, two data blocks, and descriptor metadata");
    }
    require(tracked_resource.outstanding == 0 && tracked_resource.deallocations == 4,
            "tracked list construction did not release every allocation");

    auto failure_resource = CountingMemoryResource{};
    auto failure_memory = bg::allocator{failure_resource};
    const auto copies_before_failure = CopyTracked::copies;
    const auto destructions_before_failure = CopyTracked::destructions;
    auto partial_failure_threw = false;
    try {
        (void)literal_input::construct<ItemList>(
            ScalarLiteral{sources[0], sources[1], std::string{"incompatible child"}}, failure_memory);
    } catch (const std::invalid_argument &) {
        partial_failure_threw = true;
    }
    require(partial_failure_threw, "a later incompatible list child was accepted");
    require(CopyTracked::copies - copies_before_failure == 2,
            "partial list construction made an unexpected number of child copies");
    require(CopyTracked::destructions - destructions_before_failure == 2,
            "partial list construction did not destroy its two committed children");
    require(failure_resource.outstanding == 0,
            "partial list construction leaked its owner or bunch allocation");

    using NestedItemList = bg::List<ItemList>;
    auto nested_failure_resource = CountingMemoryResource{};
    auto nested_failure_memory = bg::allocator{nested_failure_resource};
    const auto nested_copies_before = CopyTracked::copies;
    const auto nested_destructions_before = CopyTracked::destructions;
    auto nested_failure_threw = false;
    try {
        (void)literal_input::construct<NestedItemList>(
            ScalarLiteral{{sources[0]}, {sources[1], std::string{"incompatible nested child"}}},
            nested_failure_memory);
    } catch (const std::invalid_argument &) {
        nested_failure_threw = true;
    }
    require(nested_failure_threw, "an incompatible nested-list child was accepted");
    require(CopyTracked::copies - nested_copies_before == 2 &&
                CopyTracked::destructions - nested_destructions_before == 2,
            "nested rollback did not destroy both the incomplete and completed inner lists");
    require(nested_failure_resource.outstanding == 0,
            "nested partial construction leaked owner or list bunch allocations");
}

void verify_late_list_emplacement_failure(const bg::ScalarListRuntimeType &runtime_type,
                                          const bg::ScalarData &source) {
    auto resource = CountingMemoryResource{};
    auto memory = bg::allocator{resource};
    {
        auto *storage = memory.allocate_bytes(runtime_type.size(), runtime_type.alignment());
        runtime_type.construct_runtime_value(storage, memory);
        auto release = bg::util::scope_exit{[&]() noexcept {
            runtime_type.destroy(storage, memory);
            memory.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
        }};

        auto construction = ListItemConstruction{&source, 0, 9};
        const auto callback = bg::ScalarConstruction{&construction, &ListItemConstruction::callback};
        for (auto index = std::size_t{}; index < 8; ++index) {
            runtime_type.emplace_back_with(storage, memory, callback);
        }
        const auto outstanding_before_growth = resource.outstanding;
        auto threw = false;
        try {
            runtime_type.emplace_back_with(storage, memory, callback);
        } catch (const std::runtime_error &) {
            threw = true;
        }

        require(threw && construction.calls == 9, "later-block construction did not fail at the requested item");
        require(resource.outstanding == outstanding_before_growth + 1,
                "failed later-block insertion did not release its data block while retaining descriptor metadata");
        require(runtime_type.item_count(storage) == 8, "failed emplacement committed an unconstructed item slot");
    }
    require(resource.outstanding == 0 && resource.allocations == resource.deallocations,
            "destroying the partially populated list did not release its storage and descriptor metadata");
}

void verify_list_emplacement_failure_cleanup() {
    using literal_input::ScalarLiteral;
    const auto integer = bg::ScalarValue<bg::i64>{37};
    verify_late_list_emplacement_failure(
        static_cast<const bg::ScalarListRuntimeType &>(bg::cpp_scalar_runtime_type<bg::List<bg::i64>>()),
        bg::details::scalar_data(integer));

    struct ComposedItem {
        int value;
    };
    using OpaqueItem = bg::Opaque<ComposedItem>;
    const auto item_value = bg::ScalarValue<OpaqueItem>{ComposedItem{42}};
    const auto &item_runtime = bg::cpp_scalar_runtime_type<OpaqueItem>();
    const auto &list_type = bg::list_scalar_type(item_runtime.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
    const auto &composed = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(list_type, available));
    verify_late_list_emplacement_failure(composed, bg::details::scalar_data(item_value));
}

} // namespace

namespace {

void deallocate_aligned(void *memory) noexcept {
#ifdef _MSC_VER
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

void *allocate_aligned(std::size_t size, std::size_t alignment) {
#ifdef _MSC_VER
    if (auto *memory = _aligned_malloc(size == 0 ? alignment : size, alignment); memory != nullptr) {
#else
    const auto requested_size = size == 0 ? alignment : size;
    const auto rounded_size = (requested_size + alignment - 1) / alignment * alignment;
    if (auto *memory = std::aligned_alloc(alignment, rounded_size); memory != nullptr) {
#endif
        if (count_allocations.load(std::memory_order_relaxed)) {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
        }
        return memory;
    }
    throw std::bad_alloc{};
}

} // namespace

void *operator new(std::size_t size) {
    if (auto *memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
        if (count_allocations.load(std::memory_order_relaxed)) {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
        }
        return memory;
    }
    throw std::bad_alloc{};
}

void *operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    ::operator delete(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void *operator new(std::size_t size, std::align_val_t alignment) {
    const auto alignment_size = static_cast<std::size_t>(alignment);
    return allocate_aligned(size, alignment_size);
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void *memory, std::align_val_t) noexcept {
    deallocate_aligned(memory);
}

void operator delete[](void *memory, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete(void *memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete[](void *memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

int main() {
    using literal_input::ScalarLiteral;
    using literal_input::ScalarLiteralItem;

    const auto input = std::int64_t{41};
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    const ScalarLiteralItem source{input};
    count_allocations.store(false, std::memory_order_relaxed);
    const auto source_construction_allocations = allocation_count.load(std::memory_order_relaxed);

    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    source.visit([](auto *) {});
    count_allocations.store(false, std::memory_order_relaxed);
    const auto first_visit_allocations = allocation_count.load(std::memory_order_relaxed);

    auto observed = std::int64_t{};
    const auto const_lvalue_visitor = [&](auto *view) {
        const auto &data = bg::details::scalar_data(*view);
        observed = read_value<std::int64_t>(data);
    };
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    source.visit(const_lvalue_visitor);
    count_allocations.store(false, std::memory_order_relaxed);
    const auto warm_visit_allocations = allocation_count.load(std::memory_order_relaxed);
    require(source_construction_allocations == 0, "ScalarLiteralItem construction called global operator new");
    require(warm_visit_allocations == 0, "warmed scalar visit called global operator new");
    require(observed == input, "const lvalue visitor received the wrong value");

    auto *function_pointer = &free_function_visitor;
    source.visit(function_pointer, ddv::noop);
    require(free_function_visits == 1, "function pointer visitor was not called");
    source.visit(free_function_visitor, ddv::noop);
    require(free_function_visits == 2, "plain function visitor was not called");

    auto increment = std::int64_t{1};
    auto argument_visitor = ddv::serial{
        [](const bg::Scalar<std::int64_t> *view, std::int64_t &increment) {
            return view->value() + increment;
        },
        ddv::noop,
    };
    const auto outcome = source.visit(argument_visitor, increment);
    require(outcome == 42 && increment == 1, "scalar visitor arguments or nonvoid result were not forwarded");

    std::string lvalue_string{"an lvalue string"};
    auto string_view_data = std::string_view{};
    ScalarLiteralItem{lvalue_string}.visit([&](auto *view) {
        const auto &data = bg::details::scalar_data(*view);
        string_view_data = read_value<std::string_view>(data);
    });
    require(string_view_data == lvalue_string, "lvalue string contents were not preserved");
    require(string_view_data.data() == lvalue_string.data(), "lvalue string storage was copied");

    std::string mutable_string{"before"};
    const ScalarLiteralItem mutable_string_source{mutable_string};
    mutable_string.replace(0, mutable_string.size(), "after!");
    mutable_string_source.visit([&](auto *view) {
        const auto &data = bg::details::scalar_data(*view);
        const auto string_value = read_value<std::string_view>(data);
        require(string_value == mutable_string, "source did not observe a later lvalue mutation");
        require(string_value.data() == mutable_string.data(), "mutable lvalue string storage was copied");
    });

    auto temporary_string_seen = false;
    ScalarLiteralItem{std::string{"a temporary string"}}.visit([&](auto *view) {
        const auto &data = bg::details::scalar_data(*view);
        require(read_value<std::string_view>(data) == "a temporary string",
                "temporary string was not alive during its callback");
        temporary_string_seen = true;
    });
    require(temporary_string_seen, "temporary string callback did not run");

    const auto lvalue_view = std::string_view{"an lvalue string view"};
    auto observed_view = std::string_view{};
    ScalarLiteralItem{lvalue_view}.visit([&](auto *view) {
        const auto &data = bg::details::scalar_data(*view);
        observed_view = read_value<std::string_view>(data);
    });
    require(observed_view.data() == lvalue_view.data(), "lvalue string_view storage was copied");

    ScalarLiteralItem{std::string_view{"a temporary string_view"}}.visit([&](auto *view) {
        const auto &data = bg::details::scalar_data(*view);
        require(read_value<std::string_view>(data) == "a temporary string_view",
                "temporary string_view contents were not preserved");
    });

    bg::ScalarValue<std::int64_t> scalar_value{input};
    const auto &scalar_descriptor = bg::details::scalar_data(scalar_value);
    require_same_descriptor(ScalarLiteralItem{scalar_value}, scalar_descriptor);
    const auto scalar = scalar_value.view();
    require_same_descriptor(ScalarLiteralItem{scalar}, bg::details::scalar_data(scalar));
    require_same_descriptor(ScalarLiteralItem{scalar_descriptor}, scalar_descriptor);

    auto sum = std::int64_t{};
    auto leaves = std::size_t{};
    auto scalar_dispatch = ddv::make_visitable_visitor<bg::ScalarVisitTypes>(
        std::tuple<>{}, [&](auto *view) {
            const auto &scalar = bg::details::scalar_data(*view);
            sum += read_value<std::int64_t>(scalar);
            ++leaves;
        });
    auto &scalar_types = static_cast<bg::ScalarVisitTypes &>(scalar_dispatch);
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    visit_tree(ScalarLiteral{{1, {{2, 3}, 4}, {{5}}}}, scalar_types, sum, leaves);
    count_allocations.store(false, std::memory_order_relaxed);
    const auto recursive_allocations = allocation_count.load(std::memory_order_relaxed);
    require(recursive_allocations == 0, "recursive literal construction/traversal called global operator new");
    require(sum == 15, "nested brace values produced the wrong sum");
    require(leaves == 5, "nested brace values produced the wrong leaf count");

    const auto empty_shape = inspect_shape(ScalarLiteral{});
    const auto nested_empty_shape = inspect_shape(ScalarLiteral{{}});
    const auto one_element_sequence_shape = inspect_shape(ScalarLiteral{7});
    const auto scalar_leaf_shape = inspect_shape(ScalarLiteral(7));
    require(empty_shape.sequences == 1 && empty_shape.empty_sequences == 1 && empty_shape.leaves == 0,
            "empty shape was not preserved");
    require(nested_empty_shape.sequences == 2 && nested_empty_shape.empty_sequences == 1 &&
                nested_empty_shape.leaves == 0,
            "nested-empty shape was not preserved");
    require(one_element_sequence_shape.sequences == 1 && one_element_sequence_shape.leaves == 1,
            "one-element sequence shape was not preserved");
    require(scalar_leaf_shape.sequences == 0 && scalar_leaf_shape.leaves == 1,
            "scalar leaf shape was not preserved");

    auto leaf_calls = 0;
    auto noncopyable_visitor = ddv::serial{NoncopyableLeafVisitor{&leaf_calls}, ddv::noop};
    ScalarLiteral(8).visit(noncopyable_visitor);
    ScalarLiteral(9).visit(noncopyable_visitor);
    require(leaf_calls == 2, "a reusable noncopyable serial visitor was copied or skipped");

    auto result_visitor = ddv::serial{
        [](const ScalarLiteralItem &, int offset) { return offset + 1; },
        [](std::span<const ScalarLiteral> children, int offset) { return offset + static_cast<int>(children.size()); },
    };
    const auto leaf_result = ScalarLiteral(8).visit(result_visitor, 4);
    const auto sequence_result = ScalarLiteral{8, 9}.visit(result_visitor, 4);
    require(leaf_result == 5 && sequence_result == 6, "literal variant visitor lost forwarded arguments or results");

    const auto callable_list_result = ScalarLiteral(8).visit(
        [](const ScalarLiteralItem &) { return 10; },
        [](std::span<const ScalarLiteral>) { return 0; });
    require(callable_list_result == 10, "literal callable-list visitor lost its result");

    verify_scalar_dispatch();
    verify_literal_construction();
    verify_list_emplacement_failure_cleanup();

    std::cout << "ScalarLiteralItem size/alignment: " << sizeof(ScalarLiteralItem) << '/' << alignof(ScalarLiteralItem)
              << '\n';
    std::cout << "ScalarLiteral size/alignment: " << sizeof(ScalarLiteral) << '/' << alignof(ScalarLiteral) << '\n';
    std::cout << "operator new calls: first visit=" << first_visit_allocations
              << ", source construction=" << source_construction_allocations
              << ", warmed visit=" << warm_visit_allocations
              << ", recursive construction/traversal=" << recursive_allocations << '\n';
    std::cout << "nested shape: leaves=" << leaves << ", sum=" << sum << '\n';
}

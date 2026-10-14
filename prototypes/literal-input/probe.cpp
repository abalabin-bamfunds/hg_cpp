#include <literal_input/scalar_literal.hpp>
#include <literal_input/construct.hpp>

#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/hash_dictionary.hpp>
#include "test_scalar_provider.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
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

struct NestedLiteralAggregate {
    bg::i64 quantity;
    double price;
};

struct ConstLiteralAggregate {
    const bg::i64 identifier;
    const NestedLiteralAggregate nested;
};

struct alignas(128) OveralignedLiteralAggregate {
    bg::i64 identifier;
    double quantity;
};

struct OveralignedNativeAggregate {
    OveralignedLiteralAggregate nested;
    bg::i64 tail;
};

template <template <typename, auto...> typename Field> struct LiteralBundle {
    Field<bg::i64> quantity;
    Field<bg::List<bg::i64>> values;
    Field<bg::i64, bg::optional> note;
};

template <template <typename, auto...> typename Field> struct NestedLiteralBundle {
    Field<bg::Bundle<LiteralBundle>> inner;
};

template <template <typename, auto...> typename Field> struct OptionalLiteralBundle {
    Field<bg::i64, bg::optional> value;
};

template <template <typename, auto...> typename Field> struct ProviderLiteralBundle {
    Field<test_scalar_provider::value> value;
};

static_assert(bg::native_struct<ReferenceAggregate>);
static_assert(std::same_as<typename bg::scalar_storage_traits<ReferenceAggregate>::value_type,
                           const ReferenceAggregate &>);
static_assert(bg::native_struct<ConstLiteralAggregate>);
static_assert(bg::native_struct<OveralignedLiteralAggregate>);
static_assert(bg::native_struct<OveralignedNativeAggregate>);

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

void visit_tree(const literal::ScalarLiteral &element, bg::ScalarVisitTypes &scalar_types,
                std::int64_t &sum, std::size_t &leaves) {
    element.visit(
        [&](const literal::ScalarLiteralItem &source) {
            source.accept(scalar_types);
        },
        [&](std::span<const literal::ScalarLiteral> children) {
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

    void operator()(const literal::ScalarLiteralItem &) const { ++*calls; }
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

class FailNextAllocationResource final : public std::pmr::memory_resource {
  public:
    bool fail_next{};
    std::size_t outstanding{};

  private:
    void *do_allocate(std::size_t size, std::size_t alignment) override {
        if (std::exchange(fail_next, false)) {
            throw std::bad_alloc{};
        }
        auto *result = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++outstanding;
        return result;
    }

    void do_deallocate(void *value, std::size_t size, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, size, alignment);
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

struct ScalarConstructionProbe {
    const bg::ScalarData *source;
    std::size_t calls{};
    bool fail{};
    void *destination{};

    static void callback(void *context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                         bg::allocator &memory) {
        auto &construction = *static_cast<ScalarConstructionProbe *>(context);
        ++construction.calls;
        construction.destination = destination;
        if (construction.fail) {
            throw std::runtime_error{"scalar construction failed"};
        }
        runtime_type.construct_from_input(destination, *construction.source, memory);
    }
};

struct IndexedNativeFieldConstruction {
    const bg::i64 *identifier;
    const NestedLiteralAggregate *nested;
    std::size_t fail_on{std::numeric_limits<std::size_t>::max()};
    bool omit_on_failure{};
    std::array<void *, 2> destinations{};
    std::size_t calls{};

    static bool callback(void *context, std::size_t index, const bg::ScalarRuntimeType &runtime_type,
                         void *destination, bg::allocator &memory) {
        auto &construction = *static_cast<IndexedNativeFieldConstruction *>(context);
        ++construction.calls;
        construction.destinations[index] = destination;
        if (index == construction.fail_on) {
            if (construction.omit_on_failure) {
                return false;
            }
            throw std::runtime_error{"indexed native field construction failed"};
        }

        if (index == 0) {
            runtime_type.construct_from_value(destination, construction.identifier, memory);
        } else {
            runtime_type.construct_from_value(destination, construction.nested, memory);
        }
        return true;
    }
};

struct OveralignedNativeFieldConstruction {
    const OveralignedLiteralAggregate *nested;
    const bg::i64 *tail;
    std::array<void *, 2> destinations{};

    static bool callback(void *context, std::size_t index, const bg::ScalarRuntimeType &runtime_type,
                         void *destination, bg::allocator &memory) {
        auto &construction = *static_cast<OveralignedNativeFieldConstruction *>(context);
        construction.destinations[index] = destination;
        runtime_type.construct_from_value(destination, index == 0 ? static_cast<const void *>(construction.nested)
                                                                  : static_cast<const void *>(construction.tail),
                                          memory);
        return true;
    }
};

struct IndexedHierarchicalFieldConstruction {
    std::array<const bg::ScalarData *, 3> fields{};
    std::size_t fail_on{std::numeric_limits<std::size_t>::max()};
    std::size_t calls{};

    static bool callback(void *context, std::size_t index, const bg::ScalarRuntimeType &runtime_type,
                         void *destination, bg::allocator &memory) {
        auto &construction = *static_cast<IndexedHierarchicalFieldConstruction *>(context);
        ++construction.calls;
        if (index == construction.fail_on) {
            throw std::runtime_error{"indexed hierarchical field construction failed"};
        }
        if (construction.fields[index] == nullptr) {
            return false;
        }
        runtime_type.construct_from_input(destination, *construction.fields[index], memory);
        return true;
    }
};

struct HierarchicalScalarConstruction {
    bg::ScalarFieldConstruction fields;

    static void callback(void *context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                         bg::allocator &memory) {
        static_cast<const bg::HierarchicalScalarRuntimeType &>(runtime_type).construct_fields_with(
            destination, memory, static_cast<const HierarchicalScalarConstruction *>(context)->fields);
    }
};

struct DistinctNestedBundleAllocatorConstruction {
    bg::allocator *nested_memory;
    bg::i64 quantity{77};

    static void callback(void *context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                         bg::allocator &memory) {
        const auto construction = bg::ScalarFieldConstruction{context, &outer_field};
        static_cast<const bg::HierarchicalScalarRuntimeType &>(runtime_type).construct_fields_with(
            destination, memory, construction);
    }

    static bool outer_field(void *context, std::size_t index, const bg::ScalarRuntimeType &runtime_type,
                            void *destination, bg::allocator &) {
        auto &construction = *static_cast<DistinctNestedBundleAllocatorConstruction *>(context);
        require(index == 0 && bg::details::current_scalar_allocation_owner() != nullptr,
                "outer Bundle field callback did not run in its scalar allocation context");
        const auto fields = bg::ScalarFieldConstruction{&construction, &inner_field};
        static_cast<const bg::HierarchicalScalarRuntimeType &>(runtime_type).construct_fields_with(
            destination, *construction.nested_memory, fields);
        return true;
    }

    static bool inner_field(void *context, std::size_t index, const bg::ScalarRuntimeType &runtime_type,
                            void *destination, bg::allocator &memory) {
        auto &construction = *static_cast<DistinctNestedBundleAllocatorConstruction *>(context);
        if (index == 0) {
            runtime_type.construct_from_value(destination, &construction.quantity, memory);
            return true;
        }
        if (index == 1) {
            static_cast<const bg::ScalarListRuntimeType &>(runtime_type).construct_runtime_value(destination, memory);
            return true;
        }
        return false;
    }
};

class ThrowingIntegerRuntime final : public bg::ScalarRuntimeType {
  public:
    ThrowingIntegerRuntime()
        : ScalarRuntimeType{bg::cpp_scalar_type<bg::i64>(), sizeof(bg::i64), alignof(bg::i64), false, false, true} {}

    bool throw_hash{};
    bool throw_equals{};
    mutable std::size_t destructions{};

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::i64 *>(destination), bg::i64{});
    }

    void copy_construct(void *destination, const void *source, bg::allocator &) const override {
        std::construct_at(static_cast<bg::i64 *>(destination), *static_cast<const bg::i64 *>(source));
    }

    std::size_t hash(const void *value) const override {
        if (throw_hash) {
            throw std::runtime_error{"integer hash failed"};
        }
        return std::hash<bg::i64>{}(*static_cast<const bg::i64 *>(value));
    }

    void destroy(void *value, bg::allocator &) const noexcept override {
        ++destructions;
        std::destroy_at(static_cast<bg::i64 *>(value));
    }

  private:
    void do_construct_from_input(void *destination, const bg::ScalarData &source,
                                 bg::allocator &) const override {
        auto value = bg::i64{};
        source.runtime_type().read_value(source.mode(), source.data_word(), &value);
        std::construct_at(static_cast<bg::i64 *>(destination), value);
    }

    void do_assign_from_input(void *destination, const bg::ScalarData &source) const override {
        source.runtime_type().read_value(source.mode(), source.data_word(), destination);
    }

    bool equals_same_type(const void *value, const bg::ScalarData &other) const override {
        if (throw_equals) {
            throw std::runtime_error{"integer equality failed"};
        }
        auto other_value = bg::i64{};
        other.runtime_type().read_value(other.mode(), other.data_word(), &other_value);
        return *static_cast<const bg::i64 *>(value) == other_value;
    }
};

template <typename Value> class AlternateNameRuntime final : public bg::ScalarRuntimeType {
    static_assert(std::is_trivially_destructible_v<Value>);

  public:
    explicit AlternateNameRuntime(const bg::ScalarType &type)
        : ScalarRuntimeType{type, sizeof(Value), alignof(Value), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Value *>(destination));
    }

    void read_value(bg::ScalarDataMode, std::uintptr_t data, void *destination) const override {
        std::construct_at(static_cast<Value *>(destination), *reinterpret_cast<const Value *>(data));
    }

    void copy_construct(void *destination, const void *source, bg::allocator &) const override {
        std::construct_at(static_cast<Value *>(destination), *static_cast<const Value *>(source));
    }

    std::size_t hash(const void *value) const override {
        if constexpr (std::same_as<Value, bg::symbol>) {
            return bg::scalar_storage_traits<Value>::hash(*static_cast<const Value *>(value));
        } else {
            return std::hash<Value>{}(*static_cast<const Value *>(value));
        }
    }

    void destroy(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<Value *>(value));
    }

  private:
    void do_construct_from_input(void *destination, const bg::ScalarData &source, bg::allocator &) const override {
        source.runtime_type().read_value(source.mode(), source.data_word(), destination);
    }

    void do_assign_from_input(void *destination, const bg::ScalarData &source) const override {
        alignas(Value) std::byte storage[sizeof(Value)];
        source.runtime_type().read_value(source.mode(), source.data_word(), storage);
        *static_cast<Value *>(destination) = *std::launder(reinterpret_cast<const Value *>(storage));
    }

    bool equals_same_type(const void *value, const bg::ScalarData &other) const override {
        alignas(Value) std::byte storage[sizeof(Value)];
        other.runtime_type().read_value(other.mode(), other.data_word(), storage);
        return *static_cast<const Value *>(value) == *std::launder(reinterpret_cast<const Value *>(storage));
    }
};

void inspect_shape(const literal::ScalarLiteral &element, ShapeStats &stats) {
    element.visit(
        [&](const literal::ScalarLiteralItem &) { ++stats.leaves; },
        [&](std::span<const literal::ScalarLiteral> children) {
            ++stats.sequences;
            if (children.empty()) {
                ++stats.empty_sequences;
            }
            for (const auto &child : children) {
                inspect_shape(child, stats);
            }
        });
}

ShapeStats inspect_shape(const literal::ScalarLiteral &element) {
    auto stats = ShapeStats{};
    inspect_shape(element, stats);
    return stats;
}

void require_same_descriptor(const literal::ScalarLiteralItem &source, const bg::ScalarData &expected) {
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

    literal::ScalarLiteralItem{byte}.visit(visitor);
    literal::ScalarLiteralItem{boolean}.visit(visitor);
    literal::ScalarLiteralItem{builtin}.visit(visitor);
    literal::ScalarLiteralItem{real}.visit(visitor);
    literal::ScalarLiteralItem{date_time}.visit(visitor);
    literal::ScalarLiteralItem{timedelta}.visit(visitor);
    literal::ScalarLiteralItem{string}.visit(visitor);
    literal::ScalarLiteralItem{symbol}.visit(visitor);
    literal::ScalarLiteralItem{list}.visit(visitor);
    literal::ScalarLiteralItem{set}.visit(visitor);
    literal::ScalarLiteralItem{dict}.visit(visitor);
    literal::ScalarLiteralItem{structure}.visit(visitor);
    literal::ScalarLiteralItem{hierarchical}.visit(visitor);
    literal::ScalarLiteralItem{provider_value}.visit(visitor);

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
    literal::ScalarLiteralItem{native_structure}.visit(
        [&](const bg::Scalar<bg::STRUCT> *view) {
            const auto &descriptor = bg::details::scalar_data(*view);
            require(descriptor.data_word() == reinterpret_cast<std::uintptr_t>(&native_structure),
                    "reference-valued native struct source was copied instead of borrowed");
            require(descriptor.mode() == bg::ScalarDataMode::source_view,
                    "reference-valued native struct source used an unexpected data mode");
        },
        ddv::noop);

    const auto same_type_provider = copy_only_value{73};
    literal::ScalarLiteralItem{same_type_provider}.visit(
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
    using literal::ScalarLiteral;
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
        const auto const_record = literal::construct<ConstLiteralAggregate>(
            {{"nested", {{"price", 2.5}, {"quantity", 18}}}, {"identifier", 7}}, memory);
        require(const_record.view().value().identifier == 7 && const_record.view().value().nested.quantity == 18 &&
                    const_record.view().value().nested.price == 2.5,
                "named native struct literal did not construct nested const fields");
        const auto runtime_const_record = literal::construct(
            bg::cpp_scalar_runtime_type<ConstLiteralAggregate>(),
            {{"identifier", 8}, {"nested", {{"quantity", 19}, {"price", 3.5}}}}, memory);
        require(bg::unchecked_scalar_cast<ConstLiteralAggregate>(runtime_const_record.view()).value().identifier == 8,
                "runtime-only named native struct literal produced the wrong value");
        const auto overaligned_record = literal::construct<OveralignedNativeAggregate>(
            {{"nested", {{"identifier", 12}, {"quantity", 4.5}}}, {"tail", 15}}, memory);
        const auto &overaligned_value = overaligned_record.view().value();
        require(reinterpret_cast<std::uintptr_t>(std::addressof(overaligned_value)) % alignof(OveralignedNativeAggregate) == 0 &&
                    overaligned_value.nested.identifier == 12 && overaligned_value.nested.quantity == 4.5 &&
                    overaligned_value.tail == 15,
                "named literal did not construct an over-aligned native aggregate");

        using BundleValue = bg::Bundle<LiteralBundle>;
        using OuterBundleValue = bg::Bundle<NestedLiteralBundle>;
        using OptionalBundleValue = bg::Bundle<OptionalLiteralBundle>;
        auto root_bundle_allocator_required = false;
        try {
            (void)literal::construct<BundleValue>({{"quantity", 3}, {"values", {4, 5}}});
        } catch (const std::logic_error &) {
            root_bundle_allocator_required = true;
        }
        require(root_bundle_allocator_required,
                "a root Bundle literal did not require an explicit allocator");

        const auto bundle = literal::construct<BundleValue>(
            {{bg::symbol{"quantity"}, 9}, {"values", {2, 4, 6}}}, memory);
        require(bg::scalar_cast<bg::i64>(bundle.view()[0]).value() == 9 &&
                    bg::scalar_cast<bg::List<bg::i64>>(bundle.view()[1]).size() == 3 &&
                    !bundle.view().present(2),
                "named Bundle literal did not construct required fields or omit an optional field");

        const auto bundle_list = literal::construct<bg::List<BundleValue>>(
            ScalarLiteral{{{"quantity", 1}, {"values", {2, 3}}},
                          {{"quantity", 4}, {"values", {5}}}},
            memory);
        const auto first_bundle = bg::scalar_cast<BundleValue>(bundle_list.view()[0]);
        const auto second_bundle = bg::scalar_cast<BundleValue>(bundle_list.view()[1]);
        require(bundle_list.view().size() == 2 &&
                    bg::scalar_cast<bg::i64>(first_bundle[0]).value() == 1 &&
                    bg::scalar_cast<bg::List<bg::i64>>(first_bundle[1])[1].value() == 3 &&
                    bg::scalar_cast<bg::i64>(second_bundle[0]).value() == 4 &&
                    bg::scalar_cast<bg::List<bg::i64>>(second_bundle[1])[0].value() == 5,
                "list literal did not construct nested Bundles with an explicit allocator");

        using ProviderBundle = bg::Bundle<ProviderLiteralBundle>;
        using ProviderValue = test_scalar_provider::value;
        const auto &provider_runtime = test_scalar_provider::preserving_runtime_type();
        auto provider_storage = test_scalar_provider::storage{.number = 59};
        const auto provider_view = bg::Scalar<ProviderValue>::from(provider_runtime, &provider_storage);
        const auto provider_bundle = literal::construct<ProviderBundle>({{"value", provider_view}}, memory);
        const auto provider_field = bg::unchecked_scalar_cast<ProviderValue>(provider_bundle.view()[0]);
        require(&provider_field.runtime_type() == &provider_runtime && provider_field.value().get() == 59,
                "named Bundle literal did not construct a raw provider field");

        const auto runtime_bundle = literal::construct(bg::cpp_scalar_runtime_type<BundleValue>(),
                                                       {{"quantity", 10}, {"values", {}}}, memory);
        require(bg::scalar_cast<BundleValue>(runtime_bundle.view()).field_count() == 3,
                "runtime-only named Bundle literal produced the wrong type");
        const auto empty_optional_bundle = literal::construct<OptionalBundleValue>(literal::ScalarLiteral{}, memory);
        require(empty_optional_bundle.view().field_count() == 1 && !empty_optional_bundle.view().present(0),
                "empty Bundle literal did not preserve an omitted optional field");

        auto missing_required_field_threw = false;
        try {
            (void)literal::construct<BundleValue>({{"values", {2, 4}}}, memory);
        } catch (const std::invalid_argument &) {
            missing_required_field_threw = true;
        }
        require(missing_required_field_threw, "named Bundle literal accepted a missing required field");

        auto unknown_field_threw = false;
        try {
            (void)literal::construct<BundleValue>({{"quantity", 3}, {"values", {}}, {"extra", 5}}, memory);
        } catch (const std::invalid_argument &) {
            unknown_field_threw = true;
        }
        require(unknown_field_threw, "named Bundle literal accepted an unknown field");

        auto duplicate_field_threw = false;
        try {
            (void)literal::construct<BundleValue>({{"quantity", 3}, {"quantity", 5}, {"values", {}}}, memory);
        } catch (const std::invalid_argument &) {
            duplicate_field_threw = true;
        }
        require(duplicate_field_threw, "named Bundle literal accepted a duplicate field");

        auto numeric_field_name_threw = false;
        try {
            (void)literal::construct<BundleValue>({{3, 3}, {"values", {}}}, memory);
        } catch (const std::invalid_argument &) {
            numeric_field_name_threw = true;
        }
        require(numeric_field_name_threw, "named Bundle literal accepted a numeric field name");

        auto alternate_string_runtime = AlternateNameRuntime<std::string_view>{bg::cpp_scalar_type<bg::string>()};
        auto alternate_symbol_runtime = AlternateNameRuntime<bg::symbol>{bg::cpp_scalar_type<bg::symbol>()};
        const auto alternate_string_name = std::string_view{"quantity"};
        const auto alternate_symbol_name = bg::symbol{"values"};
        const auto string_name_view =
            bg::Scalar<bg::string>::from(alternate_string_runtime, &alternate_string_name);
        const auto symbol_name_view = bg::Scalar<bg::symbol>::from(alternate_symbol_runtime, &alternate_symbol_name);
        const auto alternate_name_bundle = literal::construct<BundleValue>(
            {{string_name_view, 13}, {symbol_name_view, {}}}, memory);
        require(bg::scalar_cast<bg::i64>(alternate_name_bundle.view()[0]).value() == 13 &&
                    bg::scalar_cast<bg::List<bg::i64>>(alternate_name_bundle.view()[1]).empty(),
                "named Bundle literal did not read provider-backed string and symbol field names");

        auto mixed_name_duplicate_threw = false;
        try {
            (void)literal::construct<BundleValue>(
                {{string_name_view, 13}, {"quantity", 14}, {"values", {}}}, memory);
        } catch (const std::invalid_argument &) {
            mixed_name_duplicate_threw = true;
        }
        require(mixed_name_duplicate_threw, "string and symbol field names did not compare by text");

        auto malformed_field_threw = false;
        try {
            (void)literal::construct<BundleValue>({{"quantity", 3}, {"values"}}, memory);
        } catch (const std::invalid_argument &) {
            malformed_field_threw = true;
        }
        require(malformed_field_threw, "named Bundle literal accepted a malformed field entry");

        const auto nested_bundle = literal::construct<OuterBundleValue>(
            {{"inner", {{"quantity", 11}, {"values", {1, 3}}}}}, memory);
        const auto nested_inner = bg::scalar_cast<BundleValue>(nested_bundle.view()[0]);
        require(bg::scalar_cast<bg::i64>(nested_inner[0]).value() == 11 &&
                    bg::scalar_cast<bg::List<bg::i64>>(nested_inner[1])[1].value() == 3 &&
                    bg::details::current_scalar_allocation_owner() == nullptr,
                "nested Bundle literal failed to construct or restore allocation context");

        auto nested_missing_field_threw = false;
        try {
            (void)literal::construct<OuterBundleValue>({{"inner", {{"quantity", 11}}}}, memory);
        } catch (const std::invalid_argument &) {
            nested_missing_field_threw = true;
        }
        require(nested_missing_field_threw && bg::details::current_scalar_allocation_owner() == nullptr,
                "nested Bundle failure did not restore allocation context");

        const auto runtime_only = literal::construct(composed_outer, {{1, 2}, {}, {3, 4}}, memory);
        const auto typed = literal::construct<Nested>({{1, 2}, {}, {3, 4}}, memory);
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

        const auto empty = literal::construct<bg::List<bg::i64>>(ScalarLiteral{}, memory);
        require(empty.view().empty(), "empty literal did not construct an empty list");
        const auto nested_empty = literal::construct<Nested>(ScalarLiteral{{}, {}}, memory);
        require(nested_empty.view().size() == 2 && nested_empty.view()[0].empty() &&
                    nested_empty.view()[1].empty(),
                "nested empty lists were not preserved");

        const auto deeper = literal::construct<bg::List<bg::List<Inner>>>(
            ScalarLiteral{{{1, 2}, {}}, {}, {{3}}}, memory);
        require(deeper.view().size() == 3 && deeper.view()[0].size() == 2 &&
                    deeper.view()[0][0].size() == 2 && deeper.view()[0][1].empty() &&
                    deeper.view()[1].empty() && deeper.view()[2][0][0].value() == 3,
                "three-level list literal did not preserve its nested shape");

        const auto integers = literal::construct<bg::Set<bg::i64>>({1, 2, 2, 3}, memory);
        require(integers.view().size() == 3 && integers.view().contains(1) && integers.view().contains(2) &&
                    integers.view().contains(3),
                "set literal did not deduplicate repeated values");

        using ListSet = bg::Set<Inner>;
        const auto lists = literal::construct<ListSet>({{1, 2}, {1, 2}, {3}}, memory);
        require(lists.view().size() == 2, "set literal did not deduplicate nested list values");
        auto saw_first_list = false;
        auto saw_second_list = false;
        lists.view().for_each([&](const bg::Scalar<Inner> &item) {
            if (item.size() == 2 && item[0].value() == 1 && item[1].value() == 2) {
                saw_first_list = true;
            }
            if (item.size() == 1 && item[0].value() == 3) {
                saw_second_list = true;
            }
        });
        require(saw_first_list && saw_second_list, "set literal changed nested list values");

        using IntDict = bg::Dict<bg::i64, bg::i64>;
        using DictSet = bg::Set<IntDict>;
        const auto dictionaries = literal::construct<DictSet>(ScalarLiteral{{{1, 2}}, {{1, 2}}, {{3, 4}}}, memory);
        require(dictionaries.view().size() == 2, "set literal did not deduplicate nested dictionary values");
        auto saw_first_dictionary = false;
        auto saw_second_dictionary = false;
        dictionaries.view().for_each([&](const bg::Scalar<IntDict> &item) {
            if (item.size() == 1 && item.contains(1) && item.at(1).value() == 2) {
                saw_first_dictionary = true;
            }
            if (item.size() == 1 && item.contains(3) && item.at(3).value() == 4) {
                saw_second_dictionary = true;
            }
        });
        require(saw_first_dictionary && saw_second_dictionary, "set literal changed nested dictionary values");

        const auto &integer_set_type = bg::set_scalar_type(item_runtime.type());
        const auto integer_set_available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
        const auto &composed_integer_set = static_cast<const bg::ScalarSetRuntimeType &>(
            bg::resolve_scalar_runtime_type(integer_set_type, integer_set_available));
        const auto runtime_integers = literal::construct(composed_integer_set, {4, 5, 5}, memory);
        const auto runtime_integer_view = bg::unchecked_scalar_cast<bg::Set<bg::i64>>(runtime_integers.view());
        require(runtime_integer_view.size() == 2 && runtime_integer_view.contains(4) &&
                    runtime_integer_view.contains(5),
                "runtime-only set literal did not deduplicate values");

        const auto &list_set_type = bg::set_scalar_type(inner_type);
        const auto list_set_available = std::array<const bg::ScalarRuntimeType *, 1>{&composed_inner};
        const auto &composed_list_set = static_cast<const bg::ScalarSetRuntimeType &>(
            bg::resolve_scalar_runtime_type(list_set_type, list_set_available));
        const auto runtime_lists = literal::construct(composed_list_set, {{6, 7}, {6, 7}, {8}}, memory);
        const auto runtime_list_view = bg::unchecked_scalar_cast<ListSet>(runtime_lists.view());
        require(runtime_list_view.size() == 2,
                "runtime-only set literal did not deduplicate nested list values");

        const auto &integer_dict_type = bg::dict_scalar_type(item_runtime.type(), item_runtime.type());
        const auto integer_available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
        const auto &composed_integer_dict = static_cast<const bg::ScalarDictRuntimeType &>(
            bg::resolve_scalar_runtime_type(integer_dict_type, integer_available));
        const auto &integer_dict_set_type = bg::set_scalar_type(integer_dict_type);
        const auto integer_dict_set_available =
            std::array<const bg::ScalarRuntimeType *, 1>{&composed_integer_dict};
        const auto &composed_integer_dict_set = static_cast<const bg::ScalarSetRuntimeType &>(
            bg::resolve_scalar_runtime_type(integer_dict_set_type, integer_dict_set_available));
        const auto runtime_dictionaries = literal::construct(
            composed_integer_dict_set, ScalarLiteral{{{5, 6}}, {{5, 6}}, {{7, 8}}}, memory);
        const auto runtime_dictionary_view = bg::unchecked_scalar_cast<DictSet>(runtime_dictionaries.view());
        require(runtime_dictionary_view.size() == 2,
                "runtime-only set literal did not deduplicate nested dictionary values");

        using IntList = bg::List<bg::i64>;
        using StringListDict = bg::Dict<bg::string, IntList>;
        const auto string_list_dict = literal::construct<StringListDict>(
            {{"a", {1, 2}}, {"b", {3, 4}}}, memory);
        require(string_list_dict.view().size() == 2 && string_list_dict.view().at("a").size() == 2 &&
                    string_list_dict.view().at("a")[1].value() == 2 &&
                    string_list_dict.view().at("b")[0].value() == 3,
                "typed dictionary literal did not construct nested list values");

        using StringDict = bg::Dict<bg::string, bg::string>;
        const auto string_dict = literal::construct<StringDict>({{"key", "value"}}, memory);
        require(string_dict.view().at("key").value() == "value",
                "typed dictionary literal did not adapt character-array keys and mapped values");

        const auto &string_runtime = bg::cpp_scalar_runtime_type<bg::string>();
        const auto &string_list_dict_type = bg::dict_scalar_type(string_runtime.type(), inner_type);
        const auto dict_available = std::array<const bg::ScalarRuntimeType *, 2>{&string_runtime, &composed_inner};
        const auto &composed_string_list_dict = static_cast<const bg::ScalarDictRuntimeType &>(
            bg::resolve_scalar_runtime_type(string_list_dict_type, dict_available));
        const auto runtime_string_list_dict = literal::construct(
            composed_string_list_dict, ScalarLiteral{{"x", {6, 7}}, {"y", {8}}}, memory);
        const auto runtime_dict_view = bg::unchecked_scalar_cast<StringListDict>(runtime_string_list_dict.view());
        require(runtime_dict_view.size() == 2 && runtime_dict_view.at("x").size() == 2 &&
                    runtime_dict_view.at("x")[1].value() == 7 && runtime_dict_view.at("y")[0].value() == 8,
                "runtime-only composed dictionary literal produced the wrong values");

        const auto &string_dict_type = bg::dict_scalar_type(string_runtime.type(), string_runtime.type());
        const auto string_available = std::array<const bg::ScalarRuntimeType *, 1>{&string_runtime};
        const auto &composed_string_dict = static_cast<const bg::ScalarDictRuntimeType &>(
            bg::resolve_scalar_runtime_type(string_dict_type, string_available));
        const auto runtime_string_dict = literal::construct(composed_string_dict, {{"runtime key", "runtime value"}},
                                                            memory);
        require(bg::unchecked_scalar_cast<StringDict>(runtime_string_dict.view())
                        .at("runtime key")
                        .value() == "runtime value",
                "runtime-only dictionary did not adapt character-array keys and mapped values");

        using ProviderValue = test_scalar_provider::value;
        using ProviderDict = bg::Dict<bg::i64, ProviderValue>;
        const auto &provider_dict_runtime = test_scalar_provider::preserving_runtime_type();
        const auto &provider_dict_type = bg::dict_scalar_type(item_runtime.type(), provider_dict_runtime.type());
        const auto provider_dict_available =
            std::array<const bg::ScalarRuntimeType *, 2>{&item_runtime, &provider_dict_runtime};
        const auto &composed_provider_dict = static_cast<const bg::ScalarDictRuntimeType &>(
            bg::resolve_scalar_runtime_type(provider_dict_type, provider_dict_available));
        auto provider_dict_storage = test_scalar_provider::storage{.number = 83};
        const auto provider_dict_view_source =
            bg::Scalar<ProviderValue>::from(provider_dict_runtime, &provider_dict_storage);
        const auto provider_dict_value =
            literal::construct(composed_provider_dict, ScalarLiteral{{9, provider_dict_view_source}}, memory);
        const auto provider_dict_view = bg::unchecked_scalar_cast<ProviderDict>(provider_dict_value.view());
        require(&provider_dict_view.at(9).runtime_type() == &provider_dict_runtime &&
                    provider_dict_view.at(9).value().get() == 83,
                "runtime-only dictionary construction lost a provider mapped runtime");

        using IntListDict = bg::Dict<bg::i64, IntList>;
        using ListOfDicts = bg::List<IntListDict>;
        const auto list_of_dicts = literal::construct<ListOfDicts>(
            ScalarLiteral{{{1, {2, 3}}, {4, {5}}}, {}, {{6, {7}}}}, memory);
        require(list_of_dicts.view().size() == 3 && list_of_dicts.view()[0].size() == 2 &&
                    list_of_dicts.view()[0].at(1)[1].value() == 3 && list_of_dicts.view()[1].empty() &&
                    list_of_dicts.view()[2].at(6)[0].value() == 7,
                "list of dictionaries did not preserve its nested shape and values");

        using InnerDict = bg::Dict<bg::i64, bg::i64>;
        using DictOfDicts = bg::Dict<bg::i64, InnerDict>;
        const auto dict_of_dicts = literal::construct<DictOfDicts>(ScalarLiteral{{1, {{2, 3}, {4, 5}}}, {6, {}}},
                                                                  memory);
        require(dict_of_dicts.view().size() == 2 && dict_of_dicts.view().at(1).size() == 2 &&
                    dict_of_dicts.view().at(1).at(4).value() == 5 && dict_of_dicts.view().at(6).empty(),
                "nested dictionary literal produced the wrong values");

        const auto empty_dict = literal::construct<InnerDict>(ScalarLiteral{}, memory);
        require(empty_dict.view().empty(), "empty dictionary literal did not construct an empty dictionary");

        auto malformed_leaf_threw = false;
        try {
            (void)literal::construct<InnerDict>(ScalarLiteral{1}, memory);
        } catch (const std::invalid_argument &) {
            malformed_leaf_threw = true;
        }
        require(malformed_leaf_threw, "dictionary literal accepted a leaf in place of an entry");

        auto malformed_arity_threw = false;
        try {
            (void)literal::construct<InnerDict>(ScalarLiteral{{}}, memory);
        } catch (const std::invalid_argument &) {
            malformed_arity_threw = true;
        }
        require(malformed_arity_threw, "dictionary literal accepted a zero-field entry");

        auto excess_fields_threw = false;
        try {
            (void)literal::construct<InnerDict>(ScalarLiteral{{2, 3, 4}}, memory);
        } catch (const std::invalid_argument &) {
            excess_fields_threw = true;
        }
        require(excess_fields_threw, "dictionary literal accepted an entry with more than two fields");

        auto malformed_resource = CountingMemoryResource{};
        auto malformed_memory = bg::allocator{malformed_resource};
        auto malformed_after_value_threw = false;
        try {
            (void)literal::construct<InnerDict>(ScalarLiteral{{1, 2}, {3}}, malformed_memory);
        } catch (const std::invalid_argument &) {
            malformed_after_value_threw = true;
        }
        require(malformed_after_value_threw && malformed_resource.outstanding == 0,
                "malformed later entry did not roll back the dictionary and its allocator storage");

        auto duplicate_key_threw = false;
        try {
            (void)literal::construct<InnerDict>(ScalarLiteral{{1, 2}, {1, 3}}, memory);
        } catch (const std::invalid_argument &) {
            duplicate_key_threw = true;
        }
        require(duplicate_key_threw, "dictionary literal accepted duplicate keys");

        using ListKeyDict = bg::Dict<IntList, bg::i64>;
        auto duplicate_nested_key_threw = false;
        try {
            (void)literal::construct<ListKeyDict>(ScalarLiteral{{{1, 2}, 3}, {{1, 2}, 4}}, memory);
        } catch (const std::invalid_argument &) {
            duplicate_nested_key_threw = true;
        }
        require(duplicate_nested_key_threw, "dictionary literal did not detect duplicate nested keys");

        using TrackedValue = bg::Opaque<CopyTracked>;
        using TrackedDict = bg::Dict<bg::i64, TrackedValue>;
        const auto mapped_source = bg::ScalarValue<TrackedValue>{CopyTracked{42}};
        const auto mapped_view = mapped_source.view();
        const auto copies_before_duplicate = CopyTracked::copies;
        const auto destructions_before_duplicate = CopyTracked::destructions;
        auto duplicate_value_resource = CountingMemoryResource{};
        auto duplicate_value_memory = bg::allocator{duplicate_value_resource};
        auto duplicate_tracked_key_threw = false;
        try {
            (void)literal::construct<TrackedDict>(
                {{1, mapped_view}, {1, mapped_view}}, duplicate_value_memory);
        } catch (const std::invalid_argument &) {
            duplicate_tracked_key_threw = true;
        }
        require(duplicate_tracked_key_threw && CopyTracked::copies - copies_before_duplicate == 1 &&
                    CopyTracked::destructions - destructions_before_duplicate == 1 &&
                    duplicate_value_resource.outstanding == 0,
                "duplicate key constructed its mapped value or failed to clean the committed value");

        const auto text = literal::construct<bg::List<bg::string>>(
            ScalarLiteral{std::string{"temporary text"}, std::string{"another temporary"}}, memory);
        require(text.view().size() == 2 && text.view()[0].value() == "temporary text" &&
                    text.view()[1].value() == "another temporary",
                "temporary strings were not copied into owned storage");

        auto shape_mismatch_threw = false;
        try {
            (void)literal::construct<bg::i64>(ScalarLiteral{1, 2}, memory);
        } catch (const std::invalid_argument &) {
            shape_mismatch_threw = true;
        }
        require(shape_mismatch_threw, "a sequence accepted a scalar destination");

        auto child_failure_threw = false;
        try {
            (void)literal::construct<bg::List<bg::i64>>(
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
    const auto provider_result = literal::construct(composed_provider, ScalarLiteral{provider_view});
    const auto provider_list = bg::unchecked_scalar_cast<bg::List<ProviderValue>>(provider_result.view());
    require(&provider_list[0].runtime_type() == &provider_runtime,
            "runtime-only list construction lost the provider runtime identity");
    require(provider_list[0].value().get() == 47, "runtime-only provider list changed the leaf value");

    const auto raw_provider_result = literal::construct(
        test_scalar_provider::immutable_snapshot_runtime_type(),
        ScalarLiteral(ProviderValue{.number = 51}));
    require(&raw_provider_result.runtime_type() == &test_scalar_provider::immutable_snapshot_runtime_type(),
            "runtime-only construction lost the requested provider representation");
    require(bg::unchecked_scalar_cast<ProviderValue>(raw_provider_result.view()).value().get() == 51,
            "raw provider value was not constructed into the requested runtime representation");

    auto alternative_storage = test_scalar_provider::storage{.number = 53};
    const auto alternative_source = bg::Scalar<ProviderValue>::from(
        test_scalar_provider::canonicalizing_runtime_type(), &alternative_storage);
    const auto alternative_result = literal::construct(
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
        const auto tracked = literal::construct<ItemList>(
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
        (void)literal::construct<ItemList>(
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
        (void)literal::construct<NestedItemList>(
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

void verify_indexed_field_construction() {
    auto memory = bg::allocator{};
    const auto &native_runtime = static_cast<const bg::StructScalarRuntimeType &>(
        bg::cpp_scalar_runtime_type<ConstLiteralAggregate>());
    const auto identifier = bg::i64{17};
    const auto nested = NestedLiteralAggregate{.quantity = 23, .price = 2.75};
    auto native_construction = IndexedNativeFieldConstruction{&identifier, &nested};
    alignas(ConstLiteralAggregate) std::array<std::byte, sizeof(ConstLiteralAggregate)> native_storage{};
    native_runtime.construct_fields_with(
        native_storage.data(), memory,
        {&native_construction, &IndexedNativeFieldConstruction::callback});
    const auto *native_value = std::launder(reinterpret_cast<const ConstLiteralAggregate *>(native_storage.data()));
    require(native_value->identifier == identifier && native_value->nested.quantity == nested.quantity &&
                native_value->nested.price == nested.price &&
                native_construction.destinations[0] != std::addressof(native_value->identifier) &&
                native_construction.destinations[1] != std::addressof(native_value->nested),
            "native struct fields were not constructed in staged indexed destinations");
    native_runtime.destroy(native_storage.data(), memory);

    auto verify_native_failure = [&](bool omitted) {
        native_storage.fill(std::byte{0xa5});
        native_construction = IndexedNativeFieldConstruction{&identifier, &nested, 1, omitted};
        auto threw = false;
        try {
            native_runtime.construct_fields_with(
                native_storage.data(), memory,
                {&native_construction, &IndexedNativeFieldConstruction::callback});
        } catch (const std::invalid_argument &) {
            threw = true;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        require(threw && native_construction.calls == 2 &&
                    std::all_of(native_storage.begin(), native_storage.end(),
                                [](std::byte value) { return value == std::byte{0xa5}; }),
                "native struct field failure changed destination storage or skipped rollback");
    };
    verify_native_failure(true);
    verify_native_failure(false);

    const auto &overaligned_runtime = static_cast<const bg::StructScalarRuntimeType &>(
        bg::cpp_scalar_runtime_type<OveralignedNativeAggregate>());
    const auto nested_value = OveralignedLiteralAggregate{.identifier = 41, .quantity = 6.25};
    const auto tail = bg::i64{43};
    auto overaligned_construction = OveralignedNativeFieldConstruction{&nested_value, &tail};
    alignas(OveralignedNativeAggregate)
        std::array<std::byte, sizeof(OveralignedNativeAggregate)> overaligned_storage{};
    overaligned_runtime.construct_fields_with(
        overaligned_storage.data(), memory,
        {&overaligned_construction, &OveralignedNativeFieldConstruction::callback});
    const auto *overaligned_result =
        std::launder(reinterpret_cast<const OveralignedNativeAggregate *>(overaligned_storage.data()));
    require(reinterpret_cast<std::uintptr_t>(overaligned_construction.destinations[0]) %
                    alignof(OveralignedLiteralAggregate) ==
                0 &&
                reinterpret_cast<std::uintptr_t>(overaligned_result) % alignof(OveralignedNativeAggregate) == 0 &&
                overaligned_result->nested.identifier == nested_value.identifier &&
                overaligned_result->nested.quantity == nested_value.quantity && overaligned_result->tail == tail,
            "native compound construction did not align its staged field or aggregate");
    overaligned_runtime.destroy(overaligned_storage.data(), memory);

    using BundleValue = bg::Bundle<LiteralBundle>;
    const auto &bundle_runtime = bg::cpp_scalar_runtime_type<BundleValue>();
    auto resource = CountingMemoryResource{};
    auto bundle_memory = bg::allocator{resource};
    const auto quantity = bg::ScalarValue<bg::i64>{31, bundle_memory};
    const auto values = literal::construct<bg::List<bg::i64>>({5, 7}, bundle_memory);
    auto fields = IndexedHierarchicalFieldConstruction{
        {&bg::details::scalar_data(quantity), &bg::details::scalar_data(values), nullptr}};
    const auto field_construction = bg::ScalarFieldConstruction{
        &fields, &IndexedHierarchicalFieldConstruction::callback};
    auto outer_construction = HierarchicalScalarConstruction{field_construction};
    const auto baseline = resource.outstanding;
    {
        const auto value = bg::ScalarValue<>::from_construction(
            bundle_runtime, {&outer_construction, &HierarchicalScalarConstruction::callback}, bundle_memory);
        const auto view = bg::unchecked_scalar_cast<BundleValue>(value.view());
        require(bg::scalar_cast<bg::i64>(view[0]).value() == 31 &&
                    bg::scalar_cast<bg::List<bg::i64>>(view[1])[1].value() == 7 && !view.present(2),
                "indexed Bundle field callback did not allow an absent optional field");
    }
    require(resource.outstanding == baseline && bg::details::current_scalar_allocation_owner() == nullptr,
            "successful indexed Bundle construction did not release its allocation context");

    fields.fields[1] = nullptr;
    auto missing_required_threw = false;
    try {
        (void)bg::ScalarValue<>::from_construction(
            bundle_runtime, {&outer_construction, &HierarchicalScalarConstruction::callback}, bundle_memory);
    } catch (const std::invalid_argument &) {
        missing_required_threw = true;
    }
    require(missing_required_threw && resource.outstanding == baseline &&
                bg::details::current_scalar_allocation_owner() == nullptr,
            "missing indexed Bundle field leaked storage or failed to restore allocation context");

    fields.fields[1] = &bg::details::scalar_data(values);
    fields.fail_on = 1;
    fields.calls = 0;
    auto construction_threw = false;
    try {
        (void)bg::ScalarValue<>::from_construction(
            bundle_runtime, {&outer_construction, &HierarchicalScalarConstruction::callback}, bundle_memory);
    } catch (const std::runtime_error &) {
        construction_threw = true;
    }
    require(construction_threw && fields.calls == 2 && resource.outstanding == baseline &&
                bg::details::current_scalar_allocation_owner() == nullptr,
            "throwing indexed Bundle callback leaked storage or failed to restore allocation context");
}

void verify_nested_bundle_retained_child() {
    using Inner = bg::Bundle<LiteralBundle>;
    using Outer = bg::Bundle<NestedLiteralBundle>;
    auto resource = CountingMemoryResource{};
    auto memory = bg::allocator{resource};
    auto retained = std::optional<bg::ScalarValue<Inner>>{};
    {
        const auto outer = literal::construct<Outer>(
            {{"inner", {{"quantity", 37}, {"values", {8, 9}}}}}, memory);
        const auto child = bg::scalar_cast<Inner>(outer.view()[0]);
        retained.emplace(child);
    }

    require(retained.has_value() && resource.outstanding != 0 &&
                bg::scalar_cast<bg::i64>(retained->view()[0]).value() == 37 &&
                bg::scalar_cast<bg::List<bg::i64>>(retained->view()[1])[1].value() == 9,
            "retained nested Bundle child did not outlive its parent");
    retained.reset();
    require(resource.outstanding == 0 && bg::details::current_scalar_allocation_owner() == nullptr,
            "retained nested Bundle child did not release its shared allocation owner");
}

void verify_nested_bundle_distinct_allocator() {
    using Outer = bg::Bundle<NestedLiteralBundle>;
    auto outer_resource = CountingMemoryResource{};
    auto nested_resource = CountingMemoryResource{};
    auto outer_memory = bg::allocator{outer_resource};
    auto nested_memory = bg::allocator{nested_resource};
    auto construction = DistinctNestedBundleAllocatorConstruction{&nested_memory};
    const auto &runtime_type = bg::cpp_scalar_runtime_type<Outer>();
    {
        const auto value = bg::ScalarValue<>::from_construction(
            runtime_type, {&construction, &DistinctNestedBundleAllocatorConstruction::callback}, outer_memory);
        const auto outer = bg::unchecked_scalar_cast<Outer>(value.view());
        const auto inner = bg::scalar_cast<bg::Bundle<LiteralBundle>>(outer[0]);
        require(bg::scalar_cast<bg::i64>(inner[0]).value() == 77 &&
                    bg::scalar_cast<bg::List<bg::i64>>(inner[1]).empty(),
                "nested Bundle construction with a distinct allocator produced incorrect fields");
        require(outer_resource.outstanding != 0 && nested_resource.outstanding != 0,
                "nested Bundle construction did not allocate through both allocator resources");
    }
    require(outer_resource.outstanding == 0 && nested_resource.outstanding == 0 &&
                bg::details::current_scalar_allocation_owner() == nullptr,
            "nested Bundle construction with a distinct allocator leaked storage or context");
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
    using literal::ScalarLiteral;
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

void verify_dictionary_emplacement_failure_cleanup(const bg::ScalarDictRuntimeType &runtime_type) {
    const auto key_one = bg::ScalarValue<bg::i64>{1};
    const auto value_ten = bg::ScalarValue<bg::i64>{10};
    const auto value_twenty = bg::ScalarValue<bg::i64>{20};
    const auto key_two = bg::ScalarValue<bg::i64>{2};
    const auto key_three = bg::ScalarValue<bg::i64>{3};

    auto resource = CountingMemoryResource{};
    auto memory = bg::allocator{resource};
    {
        auto *storage = memory.allocate_bytes(runtime_type.size(), runtime_type.alignment());
        runtime_type.construct_runtime_value(storage, memory);
        auto release = bg::util::scope_exit{[&]() noexcept {
            runtime_type.destroy(storage, memory);
            memory.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
        }};

        auto key_construction = ScalarConstructionProbe{&bg::details::scalar_data(key_one)};
        auto mapped_construction = ScalarConstructionProbe{&bg::details::scalar_data(value_ten)};
        const auto key_callback = bg::ScalarConstruction{&key_construction, &ScalarConstructionProbe::callback};
        const auto mapped_callback =
            bg::ScalarConstruction{&mapped_construction, &ScalarConstructionProbe::callback};
        require(runtime_type.try_emplace_with(storage, memory, key_callback, mapped_callback),
                "first dictionary emplacement was rejected");

        struct StoredAddresses {
            const void *key{};
            const void *mapped{};
        } addresses;
        runtime_type.for_each(storage, &addresses, [](void *context, bg::ScalarDictRuntimeType::Entry entry) {
            auto &stored = *static_cast<StoredAddresses *>(context);
            stored.key = entry.key.data;
            stored.mapped = entry.value.data;
        });
        require(addresses.key == key_construction.destination && addresses.mapped == mapped_construction.destination,
                "dictionary emplacement copied constructed keys or mapped values into their final slots");

        auto duplicate_value_construction = ScalarConstructionProbe{&bg::details::scalar_data(value_twenty)};
        const auto duplicate_value_callback =
            bg::ScalarConstruction{&duplicate_value_construction, &ScalarConstructionProbe::callback};
        require(!runtime_type.try_emplace_with(storage, memory, key_callback, duplicate_value_callback),
                "duplicate dictionary emplacement was inserted");
        require(key_construction.calls == 2 && duplicate_value_construction.calls == 0 &&
                    runtime_type.item_count(storage) == 1,
                "duplicate emplacement constructed its mapped value or changed the dictionary");

        key_construction.source = &bg::details::scalar_data(key_two);
        mapped_construction.source = &bg::details::scalar_data(value_twenty);
        mapped_construction.fail = true;
        auto mapped_failure_threw = false;
        try {
            (void)runtime_type.try_emplace_with(storage, memory, key_callback, mapped_callback);
        } catch (const std::runtime_error &) {
            mapped_failure_threw = true;
        }
        require(mapped_failure_threw && runtime_type.item_count(storage) == 1 &&
                    runtime_type.at(storage, bg::details::scalar_data(key_one)).data != nullptr,
                "mapped construction failure did not preserve the existing dictionary entry");

        key_construction.source = &bg::details::scalar_data(key_three);
        key_construction.fail = true;
        mapped_construction.fail = false;
        const auto mapped_calls_before_key_failure = mapped_construction.calls;
        auto key_failure_threw = false;
        try {
            (void)runtime_type.try_emplace_with(storage, memory, key_callback, mapped_callback);
        } catch (const std::runtime_error &) {
            key_failure_threw = true;
        }
        require(key_failure_threw && mapped_construction.calls == mapped_calls_before_key_failure &&
                    runtime_type.item_count(storage) == 1,
                "key construction failure changed the dictionary or invoked mapped construction");

        key_construction.fail = false;
        require(runtime_type.try_emplace_with(storage, memory, key_callback, mapped_callback) &&
                    runtime_type.item_count(storage) == 2,
                "dictionary could not accept a new entry after construction failures");
    }
    require(resource.outstanding == 0 && resource.allocations == resource.deallocations,
            "dictionary emplacement failures leaked storage or allocator metadata");
}

void verify_set_emplacement(const bg::ScalarSetRuntimeType &runtime_type, const bg::ScalarData &source) {
    auto resource = CountingMemoryResource{};
    auto memory = bg::allocator{resource};
    {
        auto *storage = memory.allocate_bytes(runtime_type.size(), runtime_type.alignment());
        runtime_type.construct_runtime_value(storage, memory);
        auto release = bg::util::scope_exit{[&]() noexcept {
            runtime_type.destroy(storage, memory);
            memory.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
        }};

        auto construction = ScalarConstructionProbe{&source};
        const auto callback = bg::ScalarConstruction{&construction, &ScalarConstructionProbe::callback};
        require(runtime_type.insert_with(storage, memory, callback), "set rejected a new constructed item");
        require(runtime_type.item_count(storage) == 1, "set did not commit its constructed item");

        auto stored_address = static_cast<const void *>(nullptr);
        runtime_type.for_each(storage, &stored_address, [](void *context, bg::ScalarSetRuntimeType::Item item) {
            *static_cast<const void **>(context) = item.data;
        });
        require(stored_address == construction.destination,
                "set copied the constructed item instead of using its final slot");

        const auto allocations_before_duplicate = resource.outstanding;
        auto duplicate_construction = ScalarConstructionProbe{&source};
        const auto duplicate_callback =
            bg::ScalarConstruction{&duplicate_construction, &ScalarConstructionProbe::callback};
        require(!runtime_type.insert_with(storage, memory, duplicate_callback),
                "set inserted a duplicate constructed item");
        require(runtime_type.item_count(storage) == 1 && duplicate_construction.calls == 1 &&
                    resource.outstanding == allocations_before_duplicate,
                "set did not destroy its duplicate candidate or changed existing storage");

        auto failed_construction = ScalarConstructionProbe{.source = &source, .fail = true};
        const auto failed_callback =
            bg::ScalarConstruction{&failed_construction, &ScalarConstructionProbe::callback};
        auto construction_threw = false;
        try {
            (void)runtime_type.insert_with(storage, memory, failed_callback);
        } catch (const std::runtime_error &) {
            construction_threw = true;
        }
        require(construction_threw && runtime_type.item_count(storage) == 1 &&
                    resource.outstanding == allocations_before_duplicate,
                "failed set item construction changed existing storage or leaked allocations");
    }
    require(resource.outstanding == 0 && resource.allocations == resource.deallocations,
            "set emplacement leaked allocator-owned storage");
}

void verify_set_allocation_failure_cleanup(const bg::ScalarSetRuntimeType &runtime_type,
                                           const bg::ScalarData &source) {
    auto resource = FailNextAllocationResource{};
    auto memory = bg::allocator{resource};
    {
        auto *storage = memory.allocate_bytes(runtime_type.size(), runtime_type.alignment());
        runtime_type.construct_runtime_value(storage, memory);
        auto release = bg::util::scope_exit{[&]() noexcept {
            runtime_type.destroy(storage, memory);
            memory.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
        }};

        auto construction = ScalarConstructionProbe{&source};
        const auto callback = bg::ScalarConstruction{&construction, &ScalarConstructionProbe::callback};
        resource.fail_next = true;
        auto allocation_threw = false;
        try {
            (void)runtime_type.insert_with(storage, memory, callback);
        } catch (const std::bad_alloc &) {
            allocation_threw = true;
        }
        require(allocation_threw && construction.calls == 0 && runtime_type.item_count(storage) == 0,
                "set allocation failure invoked construction or committed an item");
    }
    require(resource.outstanding == 0, "set allocation failure leaked allocator-owned storage");
}

void verify_dictionary_allocation_failure_cleanup(const bg::ScalarDictRuntimeType &runtime_type) {
    auto resource = FailNextAllocationResource{};
    auto memory = bg::allocator{resource};
    {
        auto *storage = memory.allocate_bytes(runtime_type.size(), runtime_type.alignment());
        runtime_type.construct_runtime_value(storage, memory);
        auto release = bg::util::scope_exit{[&]() noexcept {
            runtime_type.destroy(storage, memory);
            memory.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
        }};

        auto failed = false;
        auto failed_index = std::size_t{};
        auto seed_key = bg::ScalarValue<bg::i64>{0};
        auto seed_value = bg::ScalarValue<bg::i64>{100};
        auto seed_key_construction = ScalarConstructionProbe{&bg::details::scalar_data(seed_key)};
        auto seed_value_construction = ScalarConstructionProbe{&bg::details::scalar_data(seed_value)};
        require(runtime_type.try_emplace_with(
                    storage, memory,
                    {&seed_key_construction, &ScalarConstructionProbe::callback},
                    {&seed_value_construction, &ScalarConstructionProbe::callback}),
                "dictionary rejected its seed entry before allocation-failure probing");

        for (auto index = std::size_t{1}; index < 1024; ++index) {
            const auto key = bg::ScalarValue<bg::i64>{static_cast<std::int64_t>(index)};
            const auto mapped = bg::ScalarValue<bg::i64>{static_cast<std::int64_t>(index + 100)};
            auto key_construction = ScalarConstructionProbe{&bg::details::scalar_data(key)};
            auto mapped_construction = ScalarConstructionProbe{&bg::details::scalar_data(mapped)};
            const auto key_callback =
                bg::ScalarConstruction{&key_construction, &ScalarConstructionProbe::callback};
            const auto mapped_callback =
                bg::ScalarConstruction{&mapped_construction, &ScalarConstructionProbe::callback};

            resource.fail_next = true;
            try {
                require(runtime_type.try_emplace_with(storage, memory, key_callback, mapped_callback),
                        "unique key was rejected while probing allocation failure");
            } catch (const std::bad_alloc &) {
                failed = true;
                failed_index = index;
            }
            if (failed) {
                break;
            }
        }

        require(failed && failed_index > 0 && runtime_type.item_count(storage) == failed_index,
                "dictionary insertion did not reach an allocation failure at a bunch growth boundary");

        const auto first_key = bg::ScalarValue<bg::i64>{0};
        const auto first_value = runtime_type.at(storage, bg::details::scalar_data(first_key));
        require(bg::Scalar<bg::i64>::from(first_value.runtime_type, first_value.data).value() == 100,
                "allocation failure changed an existing dictionary value");

        const auto retry_key = bg::ScalarValue<bg::i64>{static_cast<std::int64_t>(failed_index)};
        const auto retry_mapped = bg::ScalarValue<bg::i64>{static_cast<std::int64_t>(failed_index + 100)};
        auto retry_key_construction = ScalarConstructionProbe{&bg::details::scalar_data(retry_key)};
        auto retry_mapped_construction = ScalarConstructionProbe{&bg::details::scalar_data(retry_mapped)};
        require(runtime_type.try_emplace_with(
                    storage, memory,
                    {&retry_key_construction, &ScalarConstructionProbe::callback},
                    {&retry_mapped_construction, &ScalarConstructionProbe::callback}),
                "dictionary could not accept the entry after allocation failure");
        require(runtime_type.item_count(storage) == failed_index + 1,
                "successful retry after allocation failure produced the wrong item count");
    }
    require(resource.outstanding == 0, "dictionary allocation failure leaked allocator-owned storage");
}

void verify_constructed_hash_dictionary_failure_cleanup() {
    auto resource = FailNextAllocationResource{};
    auto items = bg::util::hash_dictionary<int, int>{};
    auto candidate_key = 1;
    auto candidate_value = 10;
    auto fail_hash = false;
    auto fail_equal = false;
    auto fail_value = false;
    auto destroyed_keys = std::size_t{};

    const auto hash = [&](const int &key) {
        if (fail_hash) {
            throw std::runtime_error{"key hash failed"};
        }
        return std::hash<int>{}(key);
    };
    const auto equal = [&](const int &stored, const int &candidate) {
        if (fail_equal) {
            throw std::runtime_error{"key equality failed"};
        }
        return stored == candidate;
    };
    const auto construct_key = [&](int *destination) { std::construct_at(destination, candidate_key); };
    const auto destroy_key = [&](int *value) noexcept {
        ++destroyed_keys;
        std::destroy_at(value);
    };
    const auto construct_value = [&](int *destination) {
        if (fail_value) {
            throw std::runtime_error{"mapped construction failed"};
        }
        std::construct_at(destination, candidate_value);
    };
    const auto insert = [&] {
        return items.try_emplace_constructed_with(resource, hash, equal, construct_key, destroy_key,
                                                  construct_value);
    };

    {
        const auto [stored, inserted] = insert();
        require(inserted && *stored == 10, "constructed hash dictionary rejected its first entry");

        candidate_key = 2;
        fail_hash = true;
        auto hash_threw = false;
        try {
            (void)insert();
        } catch (const std::runtime_error &) {
            hash_threw = true;
        }
        require(hash_threw && items.size() == 1 && destroyed_keys == 1,
                "hash failure did not destroy the candidate key and preserve the dictionary");

        candidate_key = 1;
        fail_hash = false;
        fail_equal = true;
        auto equality_threw = false;
        try {
            (void)insert();
        } catch (const std::runtime_error &) {
            equality_threw = true;
        }
        require(equality_threw && items.size() == 1 && destroyed_keys == 2,
                "equality failure did not destroy the candidate key and preserve the dictionary");

        candidate_key = 2;
        fail_equal = false;
        fail_value = true;
        auto value_threw = false;
        try {
            (void)insert();
        } catch (const std::runtime_error &) {
            value_threw = true;
        }
        require(value_threw && items.size() == 1 && destroyed_keys == 3,
                "mapped failure did not roll back its key and preserve the dictionary");

        fail_value = false;
        auto allocation_failed = false;
        auto failed_key = 2;
        for (; failed_key < 1024; ++failed_key) {
            candidate_key = failed_key;
            candidate_value = failed_key + 100;
            resource.fail_next = true;
            try {
                (void)insert();
            } catch (const std::bad_alloc &) {
                allocation_failed = true;
            }
            if (allocation_failed) {
                break;
            }
        }
        require(allocation_failed && items.size() == static_cast<std::size_t>(failed_key - 1),
                "constructed hash dictionary did not clean up after an allocation failure");
        require(items.at(1) == 10, "allocation failure changed an existing hash dictionary value");

        candidate_key = failed_key;
        candidate_value = failed_key + 100;
        const auto [retried, retry_inserted] = insert();
        require(retry_inserted && *retried == candidate_value,
                "constructed hash dictionary did not accept an entry after allocation failure");
    }

    items.release_with(resource, destroy_key, [](int *value) noexcept { std::destroy_at(value); });
    require(resource.outstanding == 0, "constructed hash dictionary failure test leaked storage");
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
    using literal::ScalarLiteral;
    using literal::ScalarLiteralItem;

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

    auto generic_sequence_visitor_seen_span = false;
    ScalarLiteral{8, 9}.visit([&](const auto &value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Value, std::span<const ScalarLiteral>>) {
            generic_sequence_visitor_seen_span = value.size() == 2;
        }
    });
    require(generic_sequence_visitor_seen_span,
            "generic Literal visitor did not receive a span for a sequence");

    verify_scalar_dispatch();
    verify_literal_construction();
    verify_indexed_field_construction();
    verify_nested_bundle_retained_child();
    verify_nested_bundle_distinct_allocator();
    verify_list_emplacement_failure_cleanup();
    const auto &integer_runtime = bg::cpp_scalar_runtime_type<bg::i64>();
    const auto &integer_dict_type = bg::dict_scalar_type(integer_runtime.type(), integer_runtime.type());
    const auto integer_available = std::array<const bg::ScalarRuntimeType *, 1>{&integer_runtime};
    const auto &composed_integer_dict = static_cast<const bg::ScalarDictRuntimeType &>(
        bg::resolve_scalar_runtime_type(integer_dict_type, integer_available));
    verify_dictionary_emplacement_failure_cleanup(
        static_cast<const bg::ScalarDictRuntimeType &>(bg::cpp_scalar_runtime_type<bg::Dict<bg::i64, bg::i64>>()));
    verify_dictionary_emplacement_failure_cleanup(composed_integer_dict);
    verify_dictionary_allocation_failure_cleanup(
        static_cast<const bg::ScalarDictRuntimeType &>(bg::cpp_scalar_runtime_type<bg::Dict<bg::i64, bg::i64>>()));
    verify_dictionary_allocation_failure_cleanup(composed_integer_dict);

    using ListItem = bg::List<bg::i64>;
    using ListSet = bg::Set<ListItem>;
    const auto list_item = literal::construct<ListItem>({1, 2});
    const auto &list_runtime = bg::cpp_scalar_runtime_type<ListItem>();
    const auto &list_set = static_cast<const bg::ScalarSetRuntimeType &>(
        bg::cpp_scalar_runtime_type<ListSet>());
    const auto &list_set_type = bg::set_scalar_type(list_runtime.type());
    const auto list_set_available = std::array<const bg::ScalarRuntimeType *, 1>{&list_runtime};
    const auto &composed_list_set = static_cast<const bg::ScalarSetRuntimeType &>(
        bg::resolve_scalar_runtime_type(list_set_type, list_set_available));
    const auto &list_data = bg::details::scalar_data(list_item);
    verify_set_emplacement(list_set, list_data);
    verify_set_emplacement(composed_list_set, list_data);
    verify_set_allocation_failure_cleanup(composed_list_set, list_data);

    verify_constructed_hash_dictionary_failure_cleanup();

    std::cout << "ScalarLiteralItem size/alignment: " << sizeof(ScalarLiteralItem) << '/' << alignof(ScalarLiteralItem)
              << '\n';
    std::cout << "ScalarLiteral size/alignment: " << sizeof(ScalarLiteral) << '/' << alignof(ScalarLiteral) << '\n';
    std::cout << "operator new calls: first visit=" << first_visit_allocations
              << ", source construction=" << source_construction_allocations
              << ", warmed visit=" << warm_visit_allocations
              << ", recursive construction/traversal=" << recursive_allocations << '\n';
    std::cout << "nested shape: leaves=" << leaves << ", sum=" << sum << '\n';
}

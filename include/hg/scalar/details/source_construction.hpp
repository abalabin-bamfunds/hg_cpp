#pragma once

#include <bg/scalar/data.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/tuple/runtime_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T> struct is_scalar_tuple_source : std::false_type {};

template <typename... Items> struct is_scalar_tuple_source<std::tuple<Items...>> : std::true_type {};

template <typename First, typename Second>
struct is_scalar_tuple_source<std::pair<First, Second>> : std::true_type {};

template <typename Source>
inline constexpr bool scalar_source_construction_supported = [] {
    using Value = std::remove_cvref_t<Source>;
    if constexpr (ScalarHandle<Value> || scalar_source<Value> || is_scalar_tuple_source<Value>::value) {
        return true;
    } else if constexpr (std::ranges::contiguous_range<const Value> && std::ranges::sized_range<const Value>) {
        using Item = std::ranges::range_reference_t<const Value>;
        return ScalarHandle<Item> || scalar_source<Item> || is_scalar_tuple_source<std::remove_cvref_t<Item>>::value;
    } else {
        return false;
    }
}();

template <typename Source>
    requires scalar_source_construction_supported<Source>
void construct_scalar_source(const ScalarRuntimeType &runtime_type, void *destination, const Source &source,
                             allocator &memory);

template <typename Source>
    requires scalar_source_construction_supported<Source>
class ScalarSourceConstructionContext {
  public:
    explicit ScalarSourceConstructionContext(const Source &source) noexcept : source_{&source} {}

    static void callback(void *context, const ScalarRuntimeType &runtime_type, void *destination, allocator &memory) {
        const auto &source = *static_cast<const ScalarSourceConstructionContext *>(context)->source_;
        construct_scalar_source(runtime_type, destination, source, memory);
    }

  private:
    const Source *source_;
};

template <typename Source, std::size_t Index = 0>
bool construct_scalar_tuple_field(const Source &source, std::size_t index, const ScalarRuntimeType &runtime_type,
                                  void *destination, allocator &memory) {
    if constexpr (Index == std::tuple_size_v<std::remove_cvref_t<Source>>) {
        return false;
    } else {
        if (index == Index) {
            construct_scalar_source(runtime_type, destination, std::get<Index>(source), memory);
            return true;
        }
        return construct_scalar_tuple_field<Source, Index + 1>(source, index, runtime_type, destination, memory);
    }
}

template <typename Source> struct ScalarTupleFieldConstructionContext {
    const Source *source;

    static bool callback(void *context, std::size_t index, const ScalarRuntimeType &runtime_type, void *destination,
                         allocator &memory) {
        const auto &source = *static_cast<const ScalarTupleFieldConstructionContext *>(context)->source;
        return construct_scalar_tuple_field(source, index, runtime_type, destination, memory);
    }
};

template <typename Source>
    requires scalar_source_construction_supported<Source>
void construct_scalar_source(const ScalarRuntimeType &runtime_type, void *destination, const Source &source,
                             allocator &memory) {
    using Value = std::remove_cvref_t<Source>;

    if constexpr (ScalarHandle<Value>) {
        runtime_type.construct_from_input(destination, scalar_data(source), memory);
    } else if constexpr (scalar_source<Value>) {
        using Canonical = canonical_scalar_t<Value>;
        decltype(auto) value = bg::canonical_scalar(source);
        runtime_type.construct_from_input(destination, make_scalar_source<Canonical>(value), memory);
    } else if constexpr (is_scalar_tuple_source<Value>::value) {
        const auto *tuple_type = type_cast<TupleScalarType>(&runtime_type.type());
        if (tuple_type == nullptr || tuple_type->size() != std::tuple_size_v<Value>) {
            throw std::invalid_argument{"tuple source does not match the target scalar type"};
        }

        auto context = ScalarTupleFieldConstructionContext<Value>{std::addressof(source)};
        const auto construction = ScalarFieldConstruction{&context,
                                                          &ScalarTupleFieldConstructionContext<Value>::callback};
        static_cast<const ScalarTupleRuntimeType &>(runtime_type).construct_fields_with(destination, memory,
                                                                                         construction);
    } else {
        const auto *list_type = type_cast<ListScalarType>(&runtime_type.type());
        if (list_type == nullptr) {
            throw std::invalid_argument{"range source does not match a list scalar type"};
        }

        const auto &list_runtime_type = static_cast<const ScalarListRuntimeType &>(runtime_type);
        list_runtime_type.construct_runtime_value(destination, memory);
        auto rollback = util::scope_exit{[&]() noexcept { list_runtime_type.destroy(destination, memory); }};
        for (const auto &item : source) {
            using Item = std::remove_cvref_t<decltype(item)>;
            auto item_context = ScalarSourceConstructionContext<Item>{item};
            list_runtime_type.emplace_back_with(
                destination, memory, {&item_context, &ScalarSourceConstructionContext<Item>::callback});
        }
        rollback.release();
    }
}

} // namespace bg::details

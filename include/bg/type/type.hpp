#pragma once

#include <bg/type/details/static_registry_entry.hpp>

#include <ddv/visitable.h>

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>
#include <typeinfo>

namespace bg {

class DictScalarType;
class BuiltinScalarType;
class GraphArgumentType;
class HierarchicalScalarType;
class ListScalarType;
class ScalarType;
class SetScalarType;
class StructScalarType;
class TimeSeriesBundleType;
class TimeSeriesDictionaryType;
class TimeSeriesListType;
class TimeSeriesReferenceType;
class TimeSeriesSetType;
class TimeSeriesType;
class TimeSeriesValueType;
class Type;
class TypeMap;
enum class TimeSeriesGuarantee;

using TypeVisitor = ddv::mux<Type, ScalarType, BuiltinScalarType, ListScalarType, SetScalarType, DictScalarType,
                             StructScalarType, HierarchicalScalarType, GraphArgumentType, TimeSeriesType,
                             TimeSeriesValueType, TimeSeriesReferenceType, TimeSeriesListType,
                             TimeSeriesBundleType, TimeSeriesSetType, TimeSeriesDictionaryType>;

template <typename Target, typename Visitor> struct is_type_cast_target;

template <typename Target, typename... Categories>
struct is_type_cast_target<Target, ddv::mux<Categories...>>
    : std::bool_constant<(std::same_as<Target, Categories> || ...)> {};

template <typename Target>
concept type_cast_target = is_type_cast_target<Target, TypeVisitor>::value;

class Type : public ddv::visitable<Type, TypeVisitor> {
  public:
    Type() noexcept : registry_entry_{*this} {}
    Type(const Type &) = delete;
    Type &operator=(const Type &) = delete;
    Type(Type &&) = delete;
    Type &operator=(Type &&) = delete;
    virtual ~Type();

    VISITOR_SUPPORT()

    virtual bool is_resolved() const noexcept = 0;
    virtual bool is_graph() const noexcept { return false; }
    virtual bool is_scalar() const noexcept { return false; }
    virtual bool is_time_series() const noexcept { return false; }
    virtual void format_name(std::string &result) const { result += "type"; }
    // Engine registration hook. Static registration requires this Type to have process-static storage.
    details::StaticRegistryEntry<Type> &registry_entry() const noexcept { return registry_entry_; }
    [[nodiscard]] bool accepts(const Type &supplied) const;
    [[nodiscard]] bool match(const Type &supplied, TypeMap &type_map) const;
    [[nodiscard]] virtual bool match(const Type &supplied, TypeMap &type_map,
                                     std::optional<TimeSeriesGuarantee> reference_limit) const;
    [[nodiscard]] virtual const Type &resolve(const TypeMap &type_map) const = 0;

    // Engine and extension matching hooks. match() handles reference compatibility first.
    [[nodiscard]] virtual bool do_match(const Type &supplied, TypeMap &) const { return this == &supplied; }

    // The default limits guarantees before calling the two-argument hook. Collections pass original child types and
    // the limit to match().
    [[nodiscard]] virtual bool do_match(const Type &supplied, TypeMap &type_map,
                                        std::optional<TimeSeriesGuarantee> reference_limit) const;

  private:
    mutable details::StaticRegistryEntry<Type> registry_entry_;
};

template <type_cast_target Target> Target *type_cast(Type *type) noexcept {
    if constexpr (std::same_as<Target, Type>) {
        return type;
    } else if constexpr (std::same_as<Target, ScalarType>) {
        return type != nullptr && type->is_scalar() ? static_cast<Target *>(type) : nullptr;
    } else if constexpr (std::same_as<Target, TimeSeriesType>) {
        return type != nullptr && type->is_time_series() ? static_cast<Target *>(type) : nullptr;
    } else if constexpr (std::same_as<Target, GraphArgumentType>) {
        return type != nullptr && type->is_graph() ? static_cast<Target *>(type) : nullptr;
    } else {
        auto *result = static_cast<Target *>(nullptr);
        if (type != nullptr) {
            type->visit([&](Target *value) { result = value; }, ddv::noop);
        }
        return result;
    }
}

template <type_cast_target Target> const Target *type_cast(const Type *type) noexcept {
    if constexpr (std::same_as<Target, Type>) {
        return type;
    } else if constexpr (std::same_as<Target, ScalarType>) {
        return type != nullptr && type->is_scalar() ? static_cast<const Target *>(type) : nullptr;
    } else if constexpr (std::same_as<Target, TimeSeriesType>) {
        return type != nullptr && type->is_time_series() ? static_cast<const Target *>(type) : nullptr;
    } else if constexpr (std::same_as<Target, GraphArgumentType>) {
        return type != nullptr && type->is_graph() ? static_cast<const Target *>(type) : nullptr;
    } else {
        const auto *result = static_cast<const Target *>(nullptr);
        if (type != nullptr) {
            type->visit([&](const Target *value) { result = value; }, ddv::noop);
        }
        return result;
    }
}

template <type_cast_target Target> Target &type_cast(Type &type) {
    auto *result = type_cast<Target>(&type);
    if (result == nullptr) {
        throw std::bad_cast{};
    }
    return *result;
}

template <type_cast_target Target> const Target &type_cast(const Type &type) {
    const auto *result = type_cast<Target>(&type);
    if (result == nullptr) {
        throw std::bad_cast{};
    }
    return *result;
}

} // namespace bg

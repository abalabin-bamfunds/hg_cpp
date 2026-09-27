#pragma once

#include <bg/optionality.hpp>
#include <bg/scalar/declaration.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/symbol.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace bg {

template <typename T> class List;
template <typename T> class Set;
template <typename K, typename V> class Dict;

struct HierarchicalScalarField {
    util::symbol name;
    const ScalarType *type;
    bool optional;
};

class HierarchicalScalarType;

namespace details {

const HierarchicalScalarType &canonical_hierarchical_scalar_type(
    const std::type_info &identity, std::string_view name, void (*initialize)(HierarchicalScalarType &));
template <template <template <typename, auto...> typename> typename Schema>
const HierarchicalScalarType &hierarchical_scalar_type();
template <template <template <typename, auto...> typename> typename Schema>
void initialize_hierarchical_scalar_type(HierarchicalScalarType &type);

} // namespace details

class HierarchicalScalarType final : public ScalarType {
  public:
    using generic_scalar_view = Scalar<>;

    VISITOR_SUPPORT()

    util::symbol name() const noexcept { return name_; }
    const HierarchicalScalarType *base() const noexcept { return base_; }
    std::span<const HierarchicalScalarField> direct_fields() const noexcept { return direct_fields_; }
    std::span<const HierarchicalScalarField> fields() const noexcept { return fields_; }
    std::size_t field_count() const noexcept { return fields_.size(); }
    const HierarchicalScalarField &field(std::size_t index) const { return fields_.at(index); }
    const HierarchicalScalarField *find(util::symbol name) const noexcept;
    bool derives_from(const HierarchicalScalarType &type) const noexcept;

    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override;
    bool supports_hash() const noexcept override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    explicit HierarchicalScalarType(util::symbol name) noexcept;
    void initialize(const HierarchicalScalarType *base, std::span<const HierarchicalScalarField> direct_fields);
    bool finalize_capabilities() noexcept;

    friend const HierarchicalScalarType &details::canonical_hierarchical_scalar_type(
        const std::type_info &, std::string_view, void (*)(HierarchicalScalarType &));
    template <template <template <typename, auto...> typename> typename Schema>
    friend const HierarchicalScalarType &details::hierarchical_scalar_type();
    template <template <template <typename, auto...> typename> typename Schema>
    friend void details::initialize_hierarchical_scalar_type(HierarchicalScalarType &);

    util::symbol name_;
    const HierarchicalScalarType *base_;
    std::vector<HierarchicalScalarField> direct_fields_;
    std::vector<HierarchicalScalarField> fields_;
    bool supports_equality_{true};
    bool supports_hash_{true};
};

namespace details {

template <typename Scalar, auto... Properties> struct hierarchical_scalar_schema_field {
    using scalar_type = Scalar;

    static constexpr bool valid_properties =
        (std::same_as<std::remove_cv_t<decltype(Properties)>, Optionality> && ...);
    static constexpr bool valid = valid_properties && sizeof...(Properties) <= 1;

    static constexpr bool optional = [] {
        if constexpr (valid_properties) {
            return ((Properties == Optionality::optional) || ...);
        } else {
            return false;
        }
    }();
};

template <>
inline constexpr bool is_schema_reflection_field<hierarchical_scalar_schema_field> = true;

template <template <template <typename, auto...> typename> typename Schema>
using hierarchical_scalar_schema_reflection = schema_reflection<Schema, hierarchical_scalar_schema_field>;

template <typename... Declarations> struct hierarchical_scalar_schema_stack {};

template <typename T, typename Stack> struct hierarchical_scalar_field_supported
    : std::bool_constant<!generic_scalar<T> && scalar_type_mapped<T>> {};

template <typename T, typename... Seen>
struct hierarchical_scalar_field_supported<List<T>, hierarchical_scalar_schema_stack<Seen...>>
    : hierarchical_scalar_field_supported<T, hierarchical_scalar_schema_stack<Seen...>> {};

template <typename T, typename... Seen>
struct hierarchical_scalar_field_supported<Set<T>, hierarchical_scalar_schema_stack<Seen...>>
    : hierarchical_scalar_field_supported<T, hierarchical_scalar_schema_stack<Seen...>> {};

template <typename K, typename V, typename... Seen>
struct hierarchical_scalar_field_supported<Dict<K, V>, hierarchical_scalar_schema_stack<Seen...>>
    : std::bool_constant<
          hierarchical_scalar_field_supported<K, hierarchical_scalar_schema_stack<Seen...>>::value &&
          hierarchical_scalar_field_supported<V, hierarchical_scalar_schema_stack<Seen...>>::value> {};

template <template <template <typename, auto...> typename> typename Schema, typename Stack>
struct hierarchical_scalar_schema_supported_impl;

template <template <template <typename, auto...> typename> typename Schema, typename... Seen>
struct hierarchical_scalar_field_supported<Bundle<Schema>, hierarchical_scalar_schema_stack<Seen...>> {
    using Declaration = Bundle<Schema>;

    static constexpr bool value = [] {
        if constexpr ((std::same_as<Declaration, Seen> || ...)) {
            return true;
        } else {
            return hierarchical_scalar_schema_supported_impl<
                Schema, hierarchical_scalar_schema_stack<Seen..., Declaration>>::value;
        }
    }();
};

template <template <template <typename, auto...> typename> typename Schema, typename... Seen>
struct hierarchical_scalar_schema_supported_impl<Schema, hierarchical_scalar_schema_stack<Seen...>> {
  private:
    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    using Stack = hierarchical_scalar_schema_stack<Seen...>;

    template <std::size_t... Index> static consteval bool direct_fields(std::index_sequence<Index...>) {
        return ((Reflection::template direct_field<Index>::valid &&
                 hierarchical_scalar_field_supported<
                     typename Reflection::template direct_field<Index>::scalar_type, Stack>::value) &&
                ...);
    }

    static consteval bool calculate() {
        auto supported = direct_fields(std::make_index_sequence<Reflection::direct_field_count>{});
        if constexpr (Reflection::has_base) {
            using Base = typename Reflection::base_schema;
            supported = supported &&
                        hierarchical_scalar_schema_supported_impl<Base::template apply, Stack>::value;
        }
        return supported;
    }

  public:
    static constexpr bool value = calculate();
};

template <template <template <typename, auto...> typename> typename Schema>
inline constexpr bool hierarchical_scalar_schema_supported =
    hierarchical_scalar_schema_supported_impl<Schema, hierarchical_scalar_schema_stack<Bundle<Schema>>>::value;

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
auto hierarchical_scalar_fields(std::index_sequence<Index...>) {
    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    return std::array<HierarchicalScalarField, sizeof...(Index)>{HierarchicalScalarField{
        util::symbol{Reflection::template direct_field_name<Index>()},
        &cpp_scalar_type<typename Reflection::template direct_field<Index>::scalar_type>(),
        Reflection::template direct_field<Index>::optional}...};
}

template <template <template <typename, auto...> typename> typename Schema>
void initialize_hierarchical_scalar_type(HierarchicalScalarType &type) {
    static_assert(hierarchical_scalar_schema_supported<Schema>,
                  "a hierarchical scalar schema requires supported scalar fields and optional properties");

    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    const auto *base = [] {
        if constexpr (Reflection::has_base) {
            using Base = typename Reflection::base_schema;
            return &hierarchical_scalar_type<Base::template apply>();
        } else {
            return static_cast<const HierarchicalScalarType *>(nullptr);
        }
    }();
    const auto fields = hierarchical_scalar_fields<Schema>(
        std::make_index_sequence<Reflection::direct_field_count>{});

    type.initialize(base, fields);
}

template <template <template <typename, auto...> typename> typename Schema>
const HierarchicalScalarType &hierarchical_scalar_type() {
    return canonical_hierarchical_scalar_type(
        typeid(Bundle<Schema>), util::unqualified_type_name<Schema<hierarchical_scalar_schema_field>>(),
        &initialize_hierarchical_scalar_type<Schema>);
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_type_traits<Bundle<Schema>> {
    static const ScalarType &type() { return details::hierarchical_scalar_type<Schema>(); }
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_declaration_traits<Bundle<Schema>> {
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = details::hierarchical_scalar_schema_supported<Schema>;
};

} // namespace bg

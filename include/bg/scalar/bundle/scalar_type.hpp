#pragma once

#include <bg/optionality.hpp>
#include <bg/scalar/declaration.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/type/schema.hpp>
#include <bg/type/schema_variable_instance.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/symbol.hpp>
#include <bg/util/static_storage.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace bg {

template <typename T> class List;
template <typename T> class Set;
template <typename K, typename V> class Dict;

class HierarchicalScalarType;
class ResolvedHierarchicalScalarType;
class TypeMap;

namespace details {

const ResolvedHierarchicalScalarType &canonical_hierarchical_scalar_type(
    const std::type_info &identity, std::string_view name, void (*initialize)(ResolvedHierarchicalScalarType &));
const ResolvedHierarchicalScalarType &canonical_hierarchical_scalar_type(const Schema &schema);
template <template <template <typename, auto...> typename> typename Schema>
const ResolvedHierarchicalScalarType &hierarchical_scalar_type();
template <template <template <typename, auto...> typename> typename Schema>
void initialize_hierarchical_scalar_type(ResolvedHierarchicalScalarType &type);

} // namespace details

class HierarchicalScalarType : public ScalarType {
  public:
    using generic_scalar_view = Scalar<HIERARCHICAL>;

    VISITOR_SUPPORT()

    void format_name(std::string &result) const override;
    virtual const SchemaType &schema_type() const noexcept = 0;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override = 0;
};

class ResolvedHierarchicalScalarType final : public HierarchicalScalarType {
  public:
    util::symbol name() const noexcept { return schema_->name(); }
    const Schema &schema() const noexcept { return *schema_; }
    const SchemaType &schema_type() const noexcept override { return *schema_; }
    const ResolvedHierarchicalScalarType *base() const noexcept { return base_; }
    std::span<const SchemaField> direct_fields() const noexcept { return schema_->direct_fields(); }
    std::span<const SchemaField> fields() const noexcept { return schema_->fields(); }
    std::size_t field_count() const noexcept { return schema_->field_count(); }
    const SchemaField &field(std::size_t index) const { return schema_->field(index); }
    const SchemaField *find(util::symbol name) const noexcept { return schema_->find(name); }
    bool derives_from(const ResolvedHierarchicalScalarType &type) const noexcept;

    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override { return supports_equality_; }
    bool supports_hash() const noexcept override { return supports_hash_; }

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    ResolvedHierarchicalScalarType() noexcept = default;
    void set_base(const ResolvedHierarchicalScalarType *base) noexcept { base_ = base; }
    void initialize(const Schema &schema);
    bool finalize_capabilities() noexcept;

    friend const ResolvedHierarchicalScalarType &details::canonical_hierarchical_scalar_type(
        const std::type_info &, std::string_view, void (*)(ResolvedHierarchicalScalarType &));
    template <template <template <typename, auto...> typename> typename Schema>
    friend const ResolvedHierarchicalScalarType &details::hierarchical_scalar_type();
    template <template <template <typename, auto...> typename> typename Schema>
    friend void details::initialize_hierarchical_scalar_type(ResolvedHierarchicalScalarType &);
    friend const ResolvedHierarchicalScalarType &details::canonical_hierarchical_scalar_type(const Schema &);

    const Schema *schema_{};
    const ResolvedHierarchicalScalarType *base_{};
    bool supports_equality_{true};
    bool supports_hash_{true};
};

class GenericHierarchicalScalarType final : public HierarchicalScalarType {
  public:
    explicit GenericHierarchicalScalarType(const SchemaType &schema);

    const SchemaType &schema_type() const noexcept override { return *schema_; }
    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    bool supports_equality() const noexcept override { return false; }
    bool supports_hash() const noexcept override { return false; }

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const SchemaType *schema_;
};

// Extension-facing canonical lookup for a resolved, runtime schema.
[[nodiscard]] const ResolvedHierarchicalScalarType &hierarchical_scalar_type(const Schema &schema);

namespace details {

inline bool is_hierarchical_scalar(const ScalarType &type) noexcept {
    return type_cast<HierarchicalScalarType>(&type) != nullptr;
}

template <typename Scalar, auto... Properties> struct hierarchical_scalar_schema_field {
    using scalar_type = Scalar;

    template <template <typename, auto...> typename Field>
    using rebind = Field<Scalar, Properties...>;

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
    : std::bool_constant<!scalar_declaration_traits<T>::generic && scalar_type_mapped<T>> {};

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
    return std::array<SchemaField, sizeof...(Index)>{SchemaField{
        util::symbol{Reflection::template direct_field_name<Index>()},
        &cpp_scalar_type<typename Reflection::template direct_field<Index>::scalar_type>(),
        Reflection::template direct_field<Index>::optional}...};
}

template <template <template <typename, auto...> typename> typename Schema>
const bg::Schema &cpp_hierarchical_scalar_schema() {
    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    const bg::Schema *base = nullptr;
    if constexpr (Reflection::has_base) {
        using Base = typename Reflection::base_schema;
        base = &cpp_hierarchical_scalar_schema<Base::template apply>();
    }

    const auto fields = hierarchical_scalar_fields<Schema>(
        std::make_index_sequence<Reflection::direct_field_count>{});
    const auto name = util::symbol{util::unqualified_type_name<Schema<hierarchical_scalar_schema_field>>()};
    auto metadata = declared_schema_metadata<Schema, hierarchical_scalar_schema_field>();
    return bg::schema(name, std::type_index{typeid(schema_template<Schema>)}, base, fields, std::move(metadata));
}

template <template <template <typename, auto...> typename> typename Schema>
void initialize_hierarchical_scalar_type(ResolvedHierarchicalScalarType &type) {
    static_assert(hierarchical_scalar_schema_supported<Schema>,
                  "a hierarchical scalar schema requires supported scalar fields and optional properties");

    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    const auto *base = [] {
        if constexpr (Reflection::has_base) {
            using Base = typename Reflection::base_schema;
            return &hierarchical_scalar_type<Base::template apply>();
        } else {
            return static_cast<const ResolvedHierarchicalScalarType *>(nullptr);
        }
    }();
    type.set_base(base);
    type.initialize(cpp_hierarchical_scalar_schema<Schema>());
}

template <template <template <typename, auto...> typename> typename Schema>
const ResolvedHierarchicalScalarType &hierarchical_scalar_type() {
    return canonical_hierarchical_scalar_type(
        typeid(Bundle<Schema>), util::unqualified_type_name<Schema<hierarchical_scalar_schema_field>>(),
        &initialize_hierarchical_scalar_type<Schema>);
}

template <template <template <typename, auto...> typename> typename Schema>
const GenericHierarchicalScalarType &generic_hierarchical_scalar_type() {
    static const auto type = util::static_storage<GenericHierarchicalScalarType>{schema_type<Schema>::type()};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_type_traits<Bundle<Schema>> {
    static const ScalarType &type() {
        if constexpr (details::schema_variable_template<Schema>) {
            return details::generic_hierarchical_scalar_type<Schema>();
        } else {
            return details::hierarchical_scalar_type<Schema>();
        }
    }
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_declaration_traits<Bundle<Schema>> {
    static constexpr bool generic = details::schema_variable_template<Schema>;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = details::schema_variable_template<Schema> ||
                                                 details::hierarchical_scalar_schema_supported<Schema>;
};

namespace details {

template <template <template <typename, auto...> typename> typename Schema>
struct declaration_variable_traits<Bundle<Schema>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        if constexpr (schema_variable_template<Schema>) {
            using Declaration = schema_type<Schema>;
            std::forward<Visitor>(visitor).template operator()<Declaration>(schema_variable_instance<Schema>());
        }
    }
};

} // namespace details

} // namespace bg

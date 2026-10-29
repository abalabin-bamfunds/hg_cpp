#pragma once

#include <bg/scalar/declaration.hpp>
#include <bg/scalar/tuple/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

class TupleScalarType : public ScalarType {
  public:
    using generic_scalar_view = Scalar<TUPLE>;

    VISITOR_SUPPORT()

    RuntimeTypeFactory default_runtime_type_factory() const noexcept override;
    void format_name(std::string &result) const override;
    virtual std::span<const ScalarType *const> item_types() const noexcept = 0;
    std::size_t size() const noexcept { return item_types().size(); }
    const ScalarType &item_type(std::size_t index) const { return *item_types()[index]; }

    bool supports_equality() const noexcept final;
    bool supports_hash() const noexcept final;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

namespace details {
inline bool is_tuple_scalar(const ScalarType &type) noexcept { return type_cast<TupleScalarType>(&type) != nullptr; }
}

class GenericTupleScalarType final : public TupleScalarType {
  public:
    explicit GenericTupleScalarType(std::vector<const ScalarType *> item_types) : item_types_{std::move(item_types)} {}

    std::span<const ScalarType *const> item_types() const noexcept override { return item_types_; }
    bool is_resolved() const noexcept override { return false; }
    const Type &resolve(const TypeMap &type_map) const override;

  private:
    std::vector<const ScalarType *> item_types_;
};

[[nodiscard]] const TupleScalarType &tuple_scalar_type(std::span<const ScalarType *const> item_types);

namespace details {

template <typename... Items> const ScalarType &tuple_declaration_type() {
    if constexpr ((scalar_declaration_traits<Items>::generic || ...)) {
        static const auto type = util::static_storage<GenericTupleScalarType>{
            std::vector<const ScalarType *>{&scalar_type_for<Items>()...}};
        [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
        return type.get();
    } else {
        const auto item_types = std::array<const ScalarType *, sizeof...(Items)>{&cpp_scalar_type<Items>()...};
        return tuple_scalar_type(item_types);
    }
}

} // namespace details

template <typename... Items> struct scalar_type_traits<Tuple<Items...>> {
    static const ScalarType &type() { return details::tuple_declaration_type<Items...>(); }
};

template <typename... Items> struct scalar_declaration_traits<Tuple<Items...>> {
    static constexpr bool generic = (scalar_declaration_traits<Items>::generic || ...);
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = (scalar_declaration_traits<Items>::supports_time_series && ...);
};

template <typename... Items> struct declaration_variable_traits<Tuple<Items...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        (::bg::for_each_declaration_variable<Items>(visitor), ...);
    }
};

} // namespace bg

namespace bg::details {

template <typename T> struct scalar_handle_traits;

template <typename... Items> inline constexpr bool generic_scalar<Tuple<Items...>> =
    (generic_scalar<Items> || ...);

template <typename... Items> struct scalar_handle_traits<Tuple<Items...>> {
    static constexpr bool supports_equality = (scalar_handle_traits<Items>::supports_equality && ...);
    static constexpr bool supports_hash = (scalar_handle_traits<Items>::supports_hash && ...);
};

} // namespace bg::details

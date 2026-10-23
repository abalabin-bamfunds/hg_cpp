#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

namespace bg::details {

template <typename T> class OpaqueScalarType final : public ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override { return scalar_storage_traits<Opaque<T>>::supports_equality; }
    bool supports_hash() const noexcept override { return scalar_storage_traits<Opaque<T>>::supports_hash; }
};

template <typename T> const ScalarType &opaque_scalar_type() noexcept {
    static const auto type = util::static_storage<OpaqueScalarType<T>>{};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

} // namespace bg::details

namespace bg {

template <typename T> struct scalar_type_traits<Opaque<T>> {
    static const ScalarType &type() noexcept { return details::opaque_scalar_type<T>(); }
};

template <typename T> struct scalar_declaration_traits<Opaque<T>> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = true;
};

} // namespace bg

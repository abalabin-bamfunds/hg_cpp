#pragma once

#include <hg/scalar/opaque.hpp>
#include <hg/scalar/runtime_type.hpp>
#include <hg/scalar/storage_traits.hpp>
#include <hg/type/scalar_type.hpp>
#include <hg/type/type_map.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/util/static_storage.hpp>

namespace hg::details {

template <typename T> class OpaqueScalarType final : public ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override { return scalar_storage_traits<Opaque<T>>::supports_equality; }
    bool supports_hash() const noexcept override { return scalar_storage_traits<Opaque<T>>::supports_hash; }
};

template <typename T> const ScalarType &opaque_scalar_type() noexcept {
    static const auto type = util::static_storage<OpaqueScalarType<T>>{};
    type.get().set_default_runtime_type_factory(&cpp_scalar_runtime_type_for_type<Opaque<T>>);
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

} // namespace hg::details

namespace hg {

template <typename T> struct scalar_type_traits<Opaque<T>> {
    static const ScalarType &type() noexcept { return details::opaque_scalar_type<T>(); }
};

template <typename T> struct scalar_declaration_traits<Opaque<T>> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = true;
};

} // namespace hg

#include <hg/scalar/cpp_runtime_type.hpp>

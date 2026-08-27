#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>

namespace bg::details {

template <typename T> class OpaqueScalarType final : public ScalarType {
  public:
    [[nodiscard]] bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override { return this == &supplied; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace bg::details

namespace bg {

template <typename T>
    requires is_opaque_v<T>
const ScalarType &cpp_scalar_type() noexcept {
    static const details::OpaqueScalarType<T> type;

    return type;
}

} // namespace bg

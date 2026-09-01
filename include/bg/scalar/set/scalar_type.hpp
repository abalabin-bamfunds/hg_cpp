#pragma once

#include <bg/scalar/set/type.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class SetScalarType : public ScalarType {
  public:
    [[nodiscard]] virtual const ScalarType &item_type() const noexcept = 0;

    [[nodiscard]] bool supports_equality() const noexcept final { return item_type().supports_equality(); }
    [[nodiscard]] bool supports_hash() const noexcept final { return item_type().supports_hash(); }
};

class GenericSetScalarType final : public SetScalarType {
  public:
    explicit GenericSetScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
    [[nodiscard]] const ScalarType &item_type() const noexcept override { return *item_type_; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;

  private:
    const ScalarType *item_type_;
};

[[nodiscard]] const SetScalarType &set_scalar_type(const ScalarType &item_type);

template <typename T>
    requires is_set_v<T>
const ScalarType &cpp_scalar_type() {
    return set_scalar_type(cpp_scalar_type<typename T::item_type>());
}

} // namespace bg

namespace bg::details {

template <typename T> struct scalar_type_traits<Set<T>> : scalar_type_properties {
    static constexpr bool is_collection = true;

    template <typename Scope> [[nodiscard]] static const ScalarType &get() {
        if constexpr (generic_scalar<T>) {
            static const auto type = GenericSetScalarType{scalar_type_for<Scope, T>()};

            return type;
        } else {
            return cpp_scalar_type<Set<T>>();
        }
    }
};

} // namespace bg::details

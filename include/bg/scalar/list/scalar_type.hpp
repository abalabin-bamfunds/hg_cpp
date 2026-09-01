#pragma once

#include <bg/type/scalar_type.hpp>

namespace bg {

class ListScalarType : public ScalarType {
  public:
    [[nodiscard]] virtual const ScalarType &item_type() const noexcept = 0;

    [[nodiscard]] bool supports_equality() const noexcept final { return item_type().supports_equality(); }
    [[nodiscard]] bool supports_hash() const noexcept final { return item_type().supports_hash(); }
};

class GenericListScalarType final : public ListScalarType {
  public:
    explicit GenericListScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
    [[nodiscard]] const ScalarType &item_type() const noexcept override { return *item_type_; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;

  private:
    const ScalarType *item_type_;
};

[[nodiscard]] const ListScalarType &list_scalar_type(const ScalarType &item_type);

template <typename T>
    requires is_list_v<T>
const ScalarType &cpp_scalar_type() {
    return list_scalar_type(cpp_scalar_type<typename T::item_type>());
}

} // namespace bg

namespace bg::details {

template <typename T> struct scalar_type_traits<List<T>> : scalar_type_properties {
    static constexpr bool is_collection = true;

    template <typename Scope> [[nodiscard]] static const ScalarType &get() {
        if constexpr (generic_scalar<T>) {
            static const auto type = GenericListScalarType{scalar_type_for<Scope, T>()};

            return type;
        } else {
            return cpp_scalar_type<List<T>>();
        }
    }
};

} // namespace bg::details

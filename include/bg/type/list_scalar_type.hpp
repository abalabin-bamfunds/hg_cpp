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
    [[nodiscard]] bool match(const Type &supplied, TypeMap &bindings) const override;
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
    [[nodiscard]] const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

[[nodiscard]] const ListScalarType &list_scalar_type(const ScalarType &item_type);

template <typename T>
    requires is_list_v<T>
const ScalarType &cpp_scalar_type() noexcept {
    return list_scalar_type(cpp_scalar_type<typename T::item_type>());
}

} // namespace bg

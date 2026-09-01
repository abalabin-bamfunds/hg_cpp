#pragma once

#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>

namespace bg {

class TypeScalarType : public ScalarType {
  public:
    [[nodiscard]] const Type &type() const noexcept { return *type_; }

  protected:
    explicit TypeScalarType(const Type &type) : type_(&type) {}

  private:
    const Type *type_;
};

[[nodiscard]] const TypeScalarType &type_of(const Type &type);

class GenericTypeScalarType final : public TypeScalarType {
  public:
    explicit GenericTypeScalarType(const Type &type) : TypeScalarType(type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;
};

} // namespace bg

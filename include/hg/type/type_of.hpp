#pragma once

#include <bg/type/details/scalar_type.hpp>
#include <bg/type/type_map.hpp>

namespace bg {

class TypeScalarType : public ScalarType {
  public:
    void format_name(std::string &result) const override;
    const Type &type() const noexcept { return *type_; }

  protected:
    explicit TypeScalarType(const Type &type) : type_(&type) {}

  private:
    const Type *type_;
};

const TypeScalarType &type_of(const Type &type);

class GenericTypeScalarType final : public TypeScalarType {
  public:
    explicit GenericTypeScalarType(const Type &type) : TypeScalarType(type) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

} // namespace bg

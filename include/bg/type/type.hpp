#pragma once

namespace bg {

class TypeMap;

class Type {
  public:
    virtual ~Type() = default;

    [[nodiscard]] virtual bool is_resolved() const noexcept = 0;
    [[nodiscard]] virtual bool is_scalar() const noexcept { return false; }
    [[nodiscard]] virtual bool is_time_series() const noexcept { return false; }
    [[nodiscard]] virtual bool match(const Type &supplied, TypeMap &bindings) const = 0;
    [[nodiscard]] virtual const Type &resolve(const TypeMap &bindings) const = 0;
};

} // namespace bg

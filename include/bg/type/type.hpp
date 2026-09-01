#pragma once

namespace bg {

class TypeMap;

class Type {
  public:
    virtual ~Type() = default;

    [[nodiscard]] virtual bool is_resolved() const noexcept = 0;
    [[nodiscard]] virtual bool is_scalar() const noexcept { return false; }
    [[nodiscard]] virtual bool is_time_series() const noexcept { return false; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &bindings) const;
    [[nodiscard]] virtual const Type &resolve(const TypeMap &bindings) const = 0;

  protected:
    [[nodiscard]] virtual bool do_match(const Type &supplied, TypeMap &) const { return this == &supplied; }
};

} // namespace bg

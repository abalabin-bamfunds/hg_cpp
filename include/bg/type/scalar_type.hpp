#pragma once

#include <bg/type/type.hpp>

namespace bg {

class ScalarType : public Type {
  public:
    [[nodiscard]] bool is_scalar() const noexcept final { return true; }
    [[nodiscard]] bool is_time_series() const noexcept final { return false; }
};

[[nodiscard]] const ScalarType &float_type() noexcept;

} // namespace bg

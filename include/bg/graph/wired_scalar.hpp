#pragma once

#include <bg/scalar/runtime_type.hpp>

namespace bg {

class GraphWiring;

class WiredScalar {
  public:
    [[nodiscard]] const ScalarRuntimeType &runtime_type() const noexcept { return *runtime_type_; }

    [[nodiscard]] const void *data() const noexcept { return data_; }

    [[nodiscard]] bool has_value() const noexcept { return data_ != nullptr; }

  private:
    friend GraphWiring;

    WiredScalar(const ScalarRuntimeType &runtime_type, void *data) noexcept
        : runtime_type_(&runtime_type), data_(data) {}

    const ScalarRuntimeType *runtime_type_;
    void *data_;
};

static_assert(sizeof(WiredScalar) == 2 * sizeof(void *));

} // namespace bg

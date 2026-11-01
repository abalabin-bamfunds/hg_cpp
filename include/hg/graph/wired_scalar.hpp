#pragma once

#include <bg/scalar/runtime_type.hpp>

namespace bg {

class GraphWiring;
class NodeTemplate;

class WiredScalar {
  public:
    const ScalarRuntimeType &runtime_type() const noexcept { return *runtime_type_; }

    const void *data() const noexcept { return data_; }

    bool has_value() const noexcept { return data_ != nullptr; }

  private:
    friend GraphWiring;
    friend NodeTemplate;

    WiredScalar(const ScalarRuntimeType &runtime_type, void *data) noexcept
        : runtime_type_(&runtime_type), data_(data) {}

    const ScalarRuntimeType *runtime_type_;
    void *data_;
};

static_assert(sizeof(WiredScalar) == 2 * sizeof(void *));

} // namespace bg

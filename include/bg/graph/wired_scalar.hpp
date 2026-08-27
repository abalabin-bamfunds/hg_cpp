#pragma once

#include <bg/scalar/storage_representation.hpp>

namespace bg {

class GraphWiring;

class WiredScalar {
  public:
    [[nodiscard]] const ScalarStorageRepresentation &storage_representation() const noexcept {
        return *storage_representation_;
    }

    [[nodiscard]] const void *data() const noexcept { return data_; }

    [[nodiscard]] bool has_value() const noexcept { return data_ != nullptr; }

  private:
    friend GraphWiring;

    WiredScalar(const ScalarStorageRepresentation &storage_representation, void *data) noexcept
        : storage_representation_(&storage_representation), data_(data) {}

    const ScalarStorageRepresentation *storage_representation_;
    void *data_;
};

static_assert(sizeof(WiredScalar) == 2 * sizeof(void *));

} // namespace bg

#pragma once

#include <bg/scalar/storage_representation.hpp>

namespace bg {

class ScalarListStorageRepresentation : public ScalarStorageRepresentation {
  public:
    [[nodiscard]] virtual const ScalarStorageRepresentation &item_storage_representation() const noexcept = 0;
};

} // namespace bg

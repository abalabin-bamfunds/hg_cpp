#pragma once

#include <bg/scalar/storage_representation.hpp>

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarSetStorageRepresentation : public ScalarStorageRepresentation {
  public:
    [[nodiscard]] virtual const ScalarStorageRepresentation &item_storage_representation() const noexcept = 0;
    [[nodiscard]] virtual std::size_t item_count(const void *storage) const noexcept = 0;
    [[nodiscard]] virtual bool contains(const void *storage, const ScalarValueInputView &value) const = 0;

    [[nodiscard]] virtual bool insert(void *storage, const ScalarValueInputView &value,
                                      std::pmr::memory_resource &memory) const = 0;
    [[nodiscard]] virtual bool erase(void *storage, const ScalarValueInputView &value,
                                     std::pmr::memory_resource &memory) const = 0;
    virtual void clear(void *storage, std::pmr::memory_resource &memory) const noexcept = 0;
};

} // namespace bg

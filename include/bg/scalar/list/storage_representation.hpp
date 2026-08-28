#pragma once

#include <bg/scalar/storage_representation.hpp>

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarListStorageRepresentation : public ScalarStorageRepresentation {
  public:
    [[nodiscard]] virtual const ScalarStorageRepresentation &item_storage_representation() const noexcept = 0;
    [[nodiscard]] virtual std::size_t item_count(const void *storage) const noexcept = 0;
    [[nodiscard]] virtual const void *item(const void *storage, std::size_t index) const noexcept = 0;

    virtual void append(void *storage, const ScalarValueInputView &value, std::pmr::memory_resource &memory) const = 0;
    virtual void set(void *storage, std::size_t index, const ScalarValueInputView &value) const = 0;
    virtual void erase(void *storage, std::size_t index, std::pmr::memory_resource &memory) const = 0;
    virtual void clear(void *storage, std::pmr::memory_resource &memory) const noexcept = 0;
};

} // namespace bg

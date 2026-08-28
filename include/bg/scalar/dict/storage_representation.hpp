#pragma once

#include <bg/scalar/storage_representation.hpp>

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarDictStorageRepresentation : public ScalarStorageRepresentation {
  public:
    struct Item {
        const ScalarStorageRepresentation &representation;
        void *data;
    };

    [[nodiscard]] virtual const ScalarStorageRepresentation &key_storage_representation() const noexcept = 0;
    [[nodiscard]] virtual const ScalarStorageRepresentation &mapped_storage_representation() const noexcept = 0;
    [[nodiscard]] virtual std::size_t item_count(const void *storage) const noexcept = 0;
    [[nodiscard]] virtual bool contains(const void *storage, const ScalarValueInputView &key) const = 0;
    [[nodiscard]] virtual Item at(void *storage, const ScalarValueInputView &key) const = 0;

    virtual void insert_or_assign(void *storage, const ScalarValueInputView &key, const ScalarValueInputView &value,
                                  std::pmr::memory_resource &memory) const = 0;
    [[nodiscard]] virtual bool erase(void *storage, const ScalarValueInputView &key,
                                     std::pmr::memory_resource &memory) const = 0;
    virtual void clear(void *storage, std::pmr::memory_resource &memory) const noexcept = 0;
};

} // namespace bg

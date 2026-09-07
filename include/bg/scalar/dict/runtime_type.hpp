#pragma once

#include <bg/scalar/runtime_type.hpp>

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarDictRuntimeType : public ScalarRuntimeType {
  protected:
    ScalarDictRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                          bool requires_finalization, bool supports_byte_copy_initialization,
                          const ScalarRuntimeType &key, const ScalarRuntimeType &mapped) noexcept
        : ScalarRuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization, supports_byte_copy_initialization},
          key_{&key}, mapped_{&mapped} {}

  public:
    struct Item {
        const ScalarRuntimeType &runtime_type;
        void *data;
    };

    struct ConstItem {
        const ScalarRuntimeType &runtime_type;
        const void *data;
    };

    struct Entry {
        ConstItem key;
        ConstItem value;
    };

    using EntryFunction = void (*)(void *, Entry);

    const ScalarRuntimeType &key_runtime_type() const noexcept { return *key_; }
    const ScalarRuntimeType &mapped_runtime_type() const noexcept { return *mapped_; }
    virtual std::size_t item_count(const void *storage) const noexcept = 0;
    virtual bool contains(const void *storage, const details::ScalarData &key) const = 0;
    virtual ConstItem at(const void *storage, const details::ScalarData &key) const = 0;
    virtual Item at(void *storage, const details::ScalarData &key) const = 0;
    virtual void for_each(const void *storage, void *context, EntryFunction function) const = 0;

    virtual void insert_or_assign(void *storage, const details::ScalarData &key, const details::ScalarData &value,
                                  std::pmr::memory_resource &memory) const = 0;
    [[nodiscard]] virtual bool erase(void *storage, const details::ScalarData &key,
                                     std::pmr::memory_resource &memory) const = 0;
    virtual void clear(void *storage, std::pmr::memory_resource &memory) const noexcept = 0;

  private:
    const ScalarRuntimeType *contained_runtime_type_for(const ScalarType &type) const noexcept override {
        if (const auto *runtime_type = key_->runtime_type_for(type)) {
            return runtime_type;
        }
        return mapped_->runtime_type_for(type);
    }

    const ScalarRuntimeType *key_;
    const ScalarRuntimeType *mapped_;
};

} // namespace bg

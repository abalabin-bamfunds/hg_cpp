#pragma once

#include <bg/scalar/runtime_type.hpp>

#include <cstddef>

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
    template <typename Key>
        requires requires(const Key &key) { details::scalar_data(key); }
    bool contains(const void *storage, const Key &key) const {
        return contains(storage, details::scalar_data(key));
    }
    virtual ConstItem at(const void *storage, const details::ScalarData &key) const = 0;
    template <typename Key>
        requires requires(const Key &key) { details::scalar_data(key); }
    ConstItem at(const void *storage, const Key &key) const {
        return at(storage, details::scalar_data(key));
    }
    virtual Item at(void *storage, const details::ScalarData &key) const = 0;
    template <typename Key>
        requires requires(const Key &key) { details::scalar_data(key); }
    Item at(void *storage, const Key &key) const {
        return at(storage, details::scalar_data(key));
    }
    virtual void for_each(const void *storage, void *context, EntryFunction function) const = 0;

    virtual void insert_or_assign(void *storage, const details::ScalarData &key, const details::ScalarData &value,
                                  allocator &memory) const = 0;
    template <typename Key, typename Value>
        requires requires(const Key &key, const Value &value) {
            details::scalar_data(key);
            details::scalar_data(value);
        }
    void insert_or_assign(void *storage, const Key &key, const Value &value, allocator &memory) const {
        insert_or_assign(storage, details::scalar_data(key), details::scalar_data(value), memory);
    }
    [[nodiscard]] virtual bool erase(void *storage, const details::ScalarData &key, allocator &memory) const = 0;
    template <typename Key>
        requires requires(const Key &key) { details::scalar_data(key); }
    [[nodiscard]] bool erase(void *storage, const Key &key, allocator &memory) const {
        return erase(storage, details::scalar_data(key), memory);
    }
    virtual void clear(void *storage, allocator &memory) const noexcept = 0;

  private:
    const ScalarRuntimeType *key_;
    const ScalarRuntimeType *mapped_;
};

} // namespace bg

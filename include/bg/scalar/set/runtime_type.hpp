#pragma once

#include <bg/scalar/construction.hpp>
#include <bg/scalar/runtime_type.hpp>

#include <cassert>
#include <cstddef>

namespace bg {

class ScalarSetRuntimeType : public ScalarRuntimeType {
  protected:
    ScalarSetRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                         bool requires_finalization, bool supports_byte_copy_initialization) noexcept
        : ScalarRuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization, supports_byte_copy_initialization} {}

    ScalarSetRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                         bool requires_finalization, bool supports_byte_copy_initialization,
                         const ScalarRuntimeType &item) noexcept
        : ScalarSetRuntimeType{type, size, alignment, uses_variable_memory, requires_finalization,
                               supports_byte_copy_initialization} {
        initialize_item_runtime_type(item);
    }

  public:
    struct Item {
        const ScalarRuntimeType &runtime_type;
        const void *data;
    };

    using ItemFunction = void (*)(void *, Item);

    const ScalarRuntimeType &item_runtime_type() const noexcept {
        assert(item_ != nullptr);
        return *item_;
    }
    void initialize_item_runtime_type(const ScalarRuntimeType &item) noexcept {
        assert(item_ == nullptr);
        item_ = &item;
    }
    virtual std::size_t item_count(const void *storage) const noexcept = 0;
    virtual bool contains(const void *storage, const details::ScalarData &value) const = 0;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    bool contains(const void *storage, const Value &value) const {
        return contains(storage, details::scalar_data(value));
    }
    virtual void for_each(const void *storage, void *context, ItemFunction function) const = 0;

    [[nodiscard]] virtual bool insert(void *storage, const details::ScalarData &value, allocator &memory) const = 0;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    [[nodiscard]] bool insert(void *storage, const Value &value, allocator &memory) const {
        return insert(storage, details::scalar_data(value), memory);
    }
    // Constructs the candidate in its final slot before hashing and duplicate lookup. On a duplicate, destroys the
    // candidate and returns false. On failure, preserves existing entries.
    [[nodiscard]] virtual bool insert_with(void *storage, allocator &memory, ScalarConstruction construction) const = 0;
    [[nodiscard]] virtual bool erase(void *storage, const details::ScalarData &value, allocator &memory) const = 0;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    [[nodiscard]] bool erase(void *storage, const Value &value, allocator &memory) const {
        return erase(storage, details::scalar_data(value), memory);
    }
    virtual void clear(void *storage, allocator &memory) const noexcept = 0;

  private:
    const ScalarRuntimeType *item_{};
};

} // namespace bg

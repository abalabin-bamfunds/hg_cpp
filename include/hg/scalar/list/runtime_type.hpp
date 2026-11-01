#pragma once

#include <bg/scalar/construction.hpp>
#include <bg/scalar/runtime_type.hpp>

#include <cassert>
#include <cstddef>

namespace bg {

class ScalarListRuntimeType : public ScalarRuntimeType {
  protected:
    ScalarListRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                          bool requires_finalization, bool supports_byte_copy_initialization) noexcept
        : ScalarRuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization, supports_byte_copy_initialization} {}

    ScalarListRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                          bool requires_finalization, bool supports_byte_copy_initialization,
                          const ScalarRuntimeType &item) noexcept
        : ScalarListRuntimeType{type, size, alignment, uses_variable_memory, requires_finalization,
                                supports_byte_copy_initialization} {
        initialize_item_runtime_type(item);
    }

  public:
    bool accepts_input(const ScalarRuntimeType &source) const noexcept override;

    const ScalarRuntimeType &item_runtime_type() const noexcept {
        assert(item_ != nullptr);
        return *item_;
    }
    void initialize_item_runtime_type(const ScalarRuntimeType &item) noexcept {
        assert(item_ == nullptr);
        item_ = &item;
    }
    virtual std::size_t item_count(const void *storage) const noexcept = 0;
    virtual const void *item(const void *storage, std::size_t index) const noexcept = 0;
    virtual void *item(void *storage, std::size_t index) const noexcept = 0;

    virtual void emplace_back_with(void *storage, allocator &memory, ScalarConstruction construction) const = 0;

    virtual void append(void *storage, const details::ScalarData &value, allocator &memory) const = 0;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    void append(void *storage, const Value &value, allocator &memory) const {
        append(storage, details::scalar_data(value), memory);
    }
    virtual void set(void *storage, std::size_t index, const details::ScalarData &value) const = 0;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    void set(void *storage, std::size_t index, const Value &value) const {
        set(storage, index, details::scalar_data(value));
    }
    virtual void erase(void *storage, std::size_t index, allocator &memory) const = 0;
    virtual void clear(void *storage, allocator &memory) const noexcept = 0;

  private:
    const ScalarRuntimeType *item_{};
};

} // namespace bg

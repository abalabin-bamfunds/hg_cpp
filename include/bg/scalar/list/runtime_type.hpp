#pragma once

#include <bg/scalar/runtime_type.hpp>

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarListRuntimeType : public ScalarRuntimeType {
  protected:
    ScalarListRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                          bool requires_finalization, bool supports_byte_copy_initialization,
                          const ScalarRuntimeType &item) noexcept
        : ScalarRuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization, supports_byte_copy_initialization},
          item_{&item} {}

  public:
    const ScalarRuntimeType &item_runtime_type() const noexcept { return *item_; }
    virtual std::size_t item_count(const void *storage) const noexcept = 0;
    virtual const void *item(const void *storage, std::size_t index) const noexcept = 0;
    virtual void *item(void *storage, std::size_t index) const noexcept = 0;

    virtual void append(void *storage, const details::ScalarData &value, std::pmr::memory_resource &memory) const = 0;
    virtual void set(void *storage, std::size_t index, const details::ScalarData &value) const = 0;
    virtual void erase(void *storage, std::size_t index, std::pmr::memory_resource &memory) const = 0;
    virtual void clear(void *storage, std::pmr::memory_resource &memory) const noexcept = 0;

  private:
    const ScalarRuntimeType *item_;
};

} // namespace bg

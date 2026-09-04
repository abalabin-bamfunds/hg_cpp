#pragma once

#include <bg/runtime_type.hpp>
#include <bg/type/scalar_type.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <span>

namespace bg {

namespace details {
class ScalarData;
enum class ScalarDataMode : std::uintptr_t;
} // namespace details

class ScalarRuntimeType : public RuntimeType {
  public:
    virtual ~ScalarRuntimeType() = default;

    [[nodiscard]] const ScalarType &type() const noexcept {
        return static_cast<const ScalarType &>(RuntimeType::type());
    }
    [[nodiscard]] virtual bool copy_inline(details::ScalarDataMode, std::uintptr_t, std::uintptr_t &) const noexcept {
        return false;
    }

    virtual void read_value(details::ScalarDataMode, std::uintptr_t, void *) const { std::terminate(); }
    [[nodiscard]] virtual std::size_t hash(details::ScalarDataMode, std::uintptr_t data) const noexcept {
        return hash(reinterpret_cast<const void *>(data));
    }
    [[nodiscard]] virtual bool equals(details::ScalarDataMode, std::uintptr_t data,
                                      const details::ScalarData &other) const noexcept {
        return equals(reinterpret_cast<const void *>(data), other);
    }

    [[nodiscard]] std::size_t size() const noexcept { return storage_size(); }
    [[nodiscard]] std::size_t alignment() const noexcept { return storage_alignment(); }
    [[nodiscard]] bool uses_variable_memory() const noexcept { return storage_uses_variable_memory(); }
    [[nodiscard]] bool requires_finalization() const noexcept { return storage_requires_finalization(); }
    [[nodiscard]] bool requires_destruction() const noexcept {
        return uses_variable_memory() || requires_finalization();
    }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept {
        return storage_supports_byte_copy_initialization();
    }
    [[nodiscard]] const ScalarRuntimeType *runtime_type_for(const ScalarType &type) const noexcept {
        return &this->type() == &type ? this : contained_runtime_type_for(type);
    }

    virtual void construct_initial_value(void *destination) const noexcept = 0;
    virtual void construct_runtime_value(void *destination, std::pmr::memory_resource &) const {
        construct_initial_value(destination);
    }
    virtual void construct_from_value(void *, const void *, std::pmr::memory_resource &) const { std::terminate(); }
    virtual void construct_from_input(void *destination, const details::ScalarData &value,
                                      std::pmr::memory_resource &memory) const = 0;
    virtual void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const = 0;
    virtual void assign_from_value(void *, const void *) const { std::terminate(); }
    virtual void assign_from_input(void *destination, const details::ScalarData &value) const = 0;
    [[nodiscard]] virtual std::size_t hash(const void *value) const noexcept = 0;
    [[nodiscard]] bool equals(const void *value, const details::ScalarData &other) const noexcept;
    virtual void destroy(void *value, std::pmr::memory_resource &memory) const noexcept = 0;

  protected:
    ScalarRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                      bool requires_finalization, bool supports_byte_copy_initialization) noexcept
        : RuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization, supports_byte_copy_initialization} {}

  private:
    [[nodiscard]] virtual const ScalarRuntimeType *contained_runtime_type_for(const ScalarType &) const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual bool equals_same_type(const void *value, const details::ScalarData &other) const noexcept = 0;
};

// ScalarData uses the three low bits of a runtime type pointer to record its representation and ownership.
static_assert(alignof(ScalarRuntimeType) >= 8);

[[nodiscard]] const ScalarRuntimeType &resolve_scalar_runtime_type(const ScalarType &type,
                                                                   std::span<const ScalarRuntimeType *const> available);

void register_scalar_runtime_type(const ScalarRuntimeType &runtime_type);

template <typename T> [[nodiscard]] const ScalarRuntimeType &cpp_scalar_runtime_type();

} // namespace bg

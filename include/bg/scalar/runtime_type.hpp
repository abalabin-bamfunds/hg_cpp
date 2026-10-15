#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/construction.hpp>
#include <bg/runtime_type.hpp>
#include <bg/type/scalar_type.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

namespace bg {

template <typename T> class Scalar;
template <typename T> class ScalarValue;
class ScalarData;
enum class ScalarDataMode : std::uintptr_t;

namespace details {
using ScalarData = ::bg::ScalarData;
using ScalarDataMode = ::bg::ScalarDataMode;

template <typename T> const ScalarData &scalar_data(const Scalar<T> &value) noexcept;
template <typename T> const ScalarData &scalar_data(const ScalarValue<T> &value) noexcept;
} // namespace details

// Defines scalar storage operations for extension and runtime implementers. Node definitions use Scalar and
// ScalarValue.
class ScalarRuntimeType : public RuntimeType {
  public:
    ~ScalarRuntimeType() override;

    const ScalarType &type() const noexcept { return static_cast<const ScalarType &>(RuntimeType::type()); }
    // Selects the owning-copy destination while preserving the source's actual semantic type.
    virtual const ScalarRuntimeType &runtime_type_for_copy(const ScalarData &) const {
        return *this;
    }
    [[nodiscard]] virtual bool copy_inline(ScalarDataMode, std::uintptr_t, std::uintptr_t &) const noexcept {
        return false;
    }

    virtual void read_value(ScalarDataMode, std::uintptr_t, void *) const { std::terminate(); }
    virtual std::size_t hash(ScalarDataMode, std::uintptr_t data) const {
        return hash(reinterpret_cast<const void *>(data));
    }
    virtual bool equals(ScalarDataMode, std::uintptr_t data, const ScalarData &other) const {
        return equals(reinterpret_cast<const void *>(data), other);
    }

    ScalarData borrow(const void *storage) const noexcept;
    [[nodiscard]] virtual const ScalarRuntimeType *
    retain_shared(const ScalarData &, std::uintptr_t &) const noexcept {
        return nullptr;
    }
    virtual void release_shared(std::uintptr_t) const noexcept { std::terminate(); }

    std::size_t size() const noexcept { return storage_size(); }
    std::size_t alignment() const noexcept { return storage_alignment(); }
    bool uses_variable_memory() const noexcept { return storage_uses_variable_memory(); }
    bool requires_finalization() const noexcept { return storage_requires_finalization(); }
    bool requires_destruction() const noexcept { return uses_variable_memory() || requires_finalization(); }
    bool supports_byte_copy_initialization() const noexcept { return storage_supports_byte_copy_initialization(); }
    virtual void construct_initial_value(void *destination) const noexcept = 0;
    virtual void construct_runtime_value(void *destination, allocator &) const {
        construct_initial_value(destination);
    }
    virtual void construct_from_value(void *, const void *, allocator &) const { std::terminate(); }
    // Reports whether this runtime type can copy the source into its storage representation.
    virtual bool accepts_input(const ScalarRuntimeType &source) const noexcept;
    void construct_from_input(void *destination, const ScalarData &value, allocator &memory) const;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    void construct_from_input(void *destination, const Value &value, allocator &memory) const {
        construct_from_input(destination, details::scalar_data(value), memory);
    }
    virtual void copy_construct(void *destination, const void *source, allocator &memory) const = 0;
    virtual void assign_from_value(void *, const void *) const { std::terminate(); }
    void assign_from_input(void *destination, const ScalarData &value) const;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    void assign_from_input(void *destination, const Value &value) const {
        assign_from_input(destination, details::scalar_data(value));
    }
    // Constructs a staged replacement before comparing or assigning it. Construction and equality failures leave
    // destination unchanged. Returns false when equality suppression skips assignment.
    [[nodiscard]] virtual bool assign_with(void *destination, ScalarConstruction construction,
                                           bool suppress_equal) const;
    virtual std::size_t hash(const void *value) const = 0;
    virtual bool equals(const void *value, const ScalarData &other) const;
    template <typename Value>
        requires requires(const Value &value) { details::scalar_data(value); }
    bool equals(const void *value, const Value &other) const {
        return equals(value, details::scalar_data(other));
    }
    virtual void destroy(void *value, allocator &memory) const noexcept = 0;

  protected:
    ScalarRuntimeType(const ScalarType &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                      bool requires_finalization, bool supports_byte_copy_initialization,
                      std::uint8_t additional_flags = 0) noexcept
        : RuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization,
              supports_byte_copy_initialization, additional_flags} {}

    virtual ScalarData borrow_shared(const void *storage) const noexcept;
    virtual void do_construct_from_input(void *destination, const ScalarData &value,
                                         allocator &memory) const = 0;
    virtual void do_assign_from_input(void *destination, const ScalarData &value) const = 0;
    static constexpr auto shared_handle_flag = first_additional_flag;

  private:
    virtual bool equals_same_type(const void *value, const ScalarData &other) const = 0;
};

// ScalarData uses the three low bits of a runtime type pointer to record its representation and ownership.
static_assert(alignof(ScalarRuntimeType) >= 8);

[[nodiscard]] const ScalarRuntimeType &resolve_scalar_runtime_type(const ScalarType &type,
                                                                   std::span<const ScalarRuntimeType *const> available);

void register_scalar_runtime_type(const ScalarRuntimeType &runtime_type);

template <typename T> const ScalarRuntimeType &cpp_scalar_runtime_type();

} // namespace bg

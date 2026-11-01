#pragma once

#include <bg/scalar/construction.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace bg {

class ScalarTupleRuntimeType : public ScalarRuntimeType {
  public:
    const TupleScalarType &type() const noexcept {
        return static_cast<const TupleScalarType &>(ScalarRuntimeType::type());
    }

    virtual void construct_fields_with(void *destination, allocator &memory,
                                       ScalarFieldConstruction construction) const = 0;
    virtual const ScalarRuntimeType &item_runtime_type(std::size_t index) const = 0;
    virtual details::ScalarData item(const details::ScalarData &value, std::size_t index) const = 0;

  protected:
    ScalarTupleRuntimeType(const TupleScalarType &type, std::size_t size, std::size_t alignment,
                           bool uses_variable_memory, bool requires_finalization,
                           bool supports_byte_copy_initialization,
                           std::uint8_t additional_flags = 0) noexcept
        : ScalarRuntimeType{type, size, alignment, uses_variable_memory, requires_finalization,
                            supports_byte_copy_initialization, additional_flags} {}
};

namespace details {

std::unique_ptr<ScalarTupleRuntimeType> make_fixed_tuple_runtime_type(const TupleScalarType &type);
void initialize_fixed_tuple_runtime_type(ScalarTupleRuntimeType &runtime_type,
                                           std::span<const ScalarRuntimeType *const> items);

} // namespace details

} // namespace bg

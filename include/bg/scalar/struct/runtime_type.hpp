#pragma once

#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>

#include <cstddef>

namespace bg {

class StructScalarRuntimeType : public ScalarRuntimeType {
  public:
    const StructScalarType &type() const noexcept {
        return static_cast<const StructScalarType &>(ScalarRuntimeType::type());
    }

  protected:
    StructScalarRuntimeType(const StructScalarType &type, std::size_t size, std::size_t alignment,
                            bool uses_variable_memory, bool requires_finalization,
                            bool supports_byte_copy_initialization) noexcept
        : ScalarRuntimeType{
              type, size, alignment, uses_variable_memory, requires_finalization, supports_byte_copy_initialization} {}

  private:
    friend class Scalar<STRUCT>;

    virtual details::ScalarData field(const details::ScalarData &value, std::size_t index) const = 0;
};

} // namespace bg

#pragma once

#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/runtime_type.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bg::details {
class BundleAllocationOwner;
class HierarchicalScalarValueFactory;
} // namespace bg::details

namespace bg {

class HierarchicalScalarRuntimeType : public ScalarRuntimeType {
  protected:
    HierarchicalScalarRuntimeType(const HierarchicalScalarType &type, std::size_t size, std::size_t alignment,
                                  bool uses_variable_memory, bool requires_finalization,
                                  bool supports_byte_copy_initialization, std::uint8_t additional_flags = 0) noexcept
        : ScalarRuntimeType{type,
                            size,
                            alignment,
                            uses_variable_memory,
                            requires_finalization,
                            supports_byte_copy_initialization,
                            additional_flags} {}

  public:
    const HierarchicalScalarType &type() const noexcept {
        return static_cast<const HierarchicalScalarType &>(ScalarRuntimeType::type());
    }

    virtual details::ScalarData field(const details::ScalarData &value, std::size_t index) const = 0;
    virtual bool field_present(const details::ScalarData &value, std::size_t index) const = 0;

  private:
    virtual std::uintptr_t construct_shared(std::span<const details::ScalarData *const> fields,
                                            details::BundleAllocationOwner &owner) const = 0;

    friend class details::HierarchicalScalarValueFactory;
};

namespace details {

std::unique_ptr<HierarchicalScalarRuntimeType>
make_composed_hierarchical_scalar_runtime_type(const HierarchicalScalarType &type);
void initialize_composed_hierarchical_scalar_runtime_type(HierarchicalScalarRuntimeType &runtime_type,
                                                          std::span<const ScalarRuntimeType *const> fields);

} // namespace details

} // namespace bg

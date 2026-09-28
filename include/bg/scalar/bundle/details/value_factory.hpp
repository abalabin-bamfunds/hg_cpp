#pragma once

#include <bg/scalar/bundle/details/allocation_owner.hpp>
#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <array>
#include <optional>
#include <span>
#include <stdexcept>

namespace bg::details {

OwnedScalarData construct_composed_hierarchical_scalar_value(
    const HierarchicalScalarType &type, std::span<const ScalarData *const> fields,
    BundleAllocationOwner &owner);

class HierarchicalScalarValueFactory {
  public:
    template <template <template <typename, auto...> typename> typename Schema>
    static ScalarValue<Bundle<Schema>> make(std::span<const std::optional<Scalar<>>> fields,
                                            allocator *upstream = nullptr) {
        auto *owner = upstream == nullptr ? current_bundle_allocation_owner() : nullptr;
        auto owns_owner = false;
        if (owner == nullptr) {
            if (upstream == nullptr) {
                throw std::logic_error{"hierarchical scalar construction requires an allocator or allocation context"};
            }

            owner = BundleAllocationOwner::create(*upstream);
            owns_owner = true;
        }
        auto release_owner = util::scope_exit{[&]() noexcept {
            if (owns_owner) {
                owner->release();
            }
        }};

        auto sources = std::array<const ScalarData *, hierarchical_scalar_schema_reflection<Schema>::field_count>{};
        for (auto index = std::size_t{}; index < fields.size(); ++index) {
            sources[index] = fields[index] ? &scalar_data(*fields[index]) : nullptr;
        }

        return ScalarValue<Bundle<Schema>>{construct_composed_hierarchical_scalar_value(
            static_cast<const HierarchicalScalarType &>(cpp_scalar_type<Bundle<Schema>>()), sources, *owner)};
    }
};

} // namespace bg::details

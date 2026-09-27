#pragma once

#include <bg/scalar/bundle/details/allocation_owner.hpp>
#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <optional>
#include <span>
#include <vector>

namespace bg::details {

class HierarchicalScalarValueFactory {
  public:
    template <template <template <typename, auto...> typename> typename Schema>
    static ScalarValue<Bundle<Schema>> make(std::span<const std::optional<Scalar<>>> fields,
                                            allocator *upstream = nullptr) {
        auto *owner = upstream == nullptr ? current_bundle_allocation_owner() : nullptr;
        auto owns_owner = false;
        if (owner == nullptr) {
            owner = BundleAllocationOwner::create(upstream == nullptr ? allocator{} : *upstream);
            owns_owner = true;
        }
        auto release_owner = util::scope_exit{[&]() noexcept {
            if (owns_owner) {
                owner->release();
            }
        }};

        auto sources = std::vector<const ScalarData *>{};
        sources.reserve(fields.size());
        for (const auto &field : fields) {
            sources.push_back(field ? &scalar_data(*field) : nullptr);
        }

        const auto &runtime_type =
            static_cast<const HierarchicalScalarRuntimeType &>(cpp_scalar_runtime_type<Bundle<Schema>>());
        const auto data = runtime_type.construct_shared(sources, *owner);
        return ScalarValue<Bundle<Schema>>{OwnedScalarData{&runtime_type, data, ScalarDataMode::shared_value}};
    }
};

} // namespace bg::details

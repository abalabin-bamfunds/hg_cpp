#pragma once

#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {

OwnedScalarData construct_composed_hierarchical_scalar_value(
    const ResolvedHierarchicalScalarType &type, std::span<const ScalarData *const> fields,
    ScalarAllocationPool &owner);
OwnedScalarData construct_composed_hierarchical_scalar_value(
    const ResolvedHierarchicalScalarType &type, ScalarFieldConstruction fields, ScalarAllocationPool &owner);

class HierarchicalScalarValueFactory {
  public:
    template <template <template <typename, auto...> typename> typename Schema>
        requires(!schema_variable_template<Schema>)
    static ScalarValue<Bundle<Schema>> make(std::span<const std::optional<Scalar<>>> fields,
                                            allocator *upstream = nullptr) {
        const auto &type = static_cast<const ResolvedHierarchicalScalarType &>(cpp_scalar_type<Bundle<Schema>>());

        auto sources = std::array<const ScalarData *, hierarchical_scalar_schema_reflection<Schema>::field_count>{};
        if (fields.size() != sources.size()) {
            throw std::invalid_argument{"hierarchical scalar field count does not match its schema"};
        }
        for (auto index = std::size_t{}; index < fields.size(); ++index) {
            sources[index] = fields[index] ? &scalar_data(*fields[index]) : nullptr;
        }

        return with_owner<ScalarValue<Bundle<Schema>>>(upstream, [&](ScalarAllocationPool &owner) {
            return ScalarValue<Bundle<Schema>>{construct_composed_hierarchical_scalar_value(type, sources, owner)};
        });
    }

    template <template <template <typename, auto...> typename> typename Schema>
        requires(!schema_variable_template<Schema>)
    static ScalarValue<Bundle<Schema>> make(ScalarFieldConstruction fields, allocator *upstream = nullptr) {
        const auto &type = static_cast<const ResolvedHierarchicalScalarType &>(cpp_scalar_type<Bundle<Schema>>());

        return with_owner<ScalarValue<Bundle<Schema>>>(upstream, [&](ScalarAllocationPool &owner) {
            return ScalarValue<Bundle<Schema>>{construct_composed_hierarchical_scalar_value(type, fields, owner)};
        });
    }

    static ScalarValue<Bundle<SCHEMA>> make(const Schema &schema, std::span<const std::optional<Scalar<>>> fields,
                                           allocator *upstream = nullptr) {
        const auto &type = hierarchical_scalar_type(schema);

        auto sources = std::vector<const ScalarData *>{};
        sources.reserve(fields.size());
        for (const auto &field : fields) {
            sources.push_back(field ? &scalar_data(*field) : nullptr);
        }

        return with_owner<ScalarValue<Bundle<SCHEMA>>>(upstream, [&](ScalarAllocationPool &owner) {
            return ScalarValue<Bundle<SCHEMA>>{construct_composed_hierarchical_scalar_value(type, sources, owner)};
        });
    }

  private:
    template <typename Value, typename Construct>
    static Value with_owner(allocator *upstream, Construct &&construct) {
        auto *owner = upstream == nullptr ? current_scalar_allocation_pool() : nullptr;
        auto owns_owner = false;
        if (owner == nullptr) {
            if (upstream == nullptr) {
                throw std::logic_error{"hierarchical scalar construction requires an allocator or allocation context"};
            }

            owner = ScalarAllocationPool::create(*upstream);
            owns_owner = true;
        }
        auto release_owner = util::scope_exit{[&]() noexcept {
            if (owns_owner) {
                owner->release();
            }
        }};

        return std::forward<Construct>(construct)(*owner);
    }

};

} // namespace bg::details

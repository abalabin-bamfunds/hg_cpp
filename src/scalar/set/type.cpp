#include <bg/scalar/set/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include "../type_registration.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

namespace bg {
namespace {

class ResolvedSetScalarType final : public SetScalarType {
  public:
    explicit ResolvedSetScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
    const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

} // namespace

bool SetScalarType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *set = type_cast<SetScalarType>(&supplied);
    if (set == nullptr || !set->is_resolved()) {
        return false;
    }
    if (item_type().is_resolved()) {
        return &item_type() == &set->item_type();
    }
    return item_type().match(set->item_type(), type_map);
}

const Type &GenericSetScalarType::resolve(const TypeMap &type_map) const {
    const auto &item = item_type_->resolve(type_map);
    const auto *scalar = type_cast<ScalarType>(&item);
    if (scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"Set"},
                                  "set item did not resolve to a scalar type"};
    }
    if (!scalar->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"Set"},
                                  "set item type does not support hashing"};
    }

    return set_scalar_type(*scalar);
}

const SetScalarType &set_scalar_type(const ScalarType &item_type) {
    static auto types = std::vector<std::unique_ptr<ResolvedSetScalarType>>{};

    const auto lock = std::lock_guard{details::semantic_type_registration_mutex()};
    const auto found = std::ranges::find_if(types, [&](const auto &type) { return &type->item_type() == &item_type; });
    if (found != types.end()) {
        return **found;
    }
    if (!item_type.supports_hash()) {
        throw std::invalid_argument{"set item type does not support hashing"};
    }

    types.push_back(std::make_unique<ResolvedSetScalarType>(item_type));
    const auto *result = types.back().get();
    if (auto *registration = details::SemanticTypeRegistration::current()) {
        try {
            registration->on_rollback([result] {
                std::erase_if(types, [result](const auto &type) { return type.get() == result; });
            });
            registration->on_validate([&item_type] {
                if (!item_type.supports_hash()) {
                    throw std::invalid_argument{"set item type does not support hashing"};
                }
            });
        } catch (...) {
            types.pop_back();
            throw;
        }
    }

    return *result;
}

} // namespace bg

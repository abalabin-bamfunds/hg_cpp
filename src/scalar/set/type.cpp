#include <bg/scalar/set/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace bg {
namespace {

class ResolvedSetScalarType final : public SetScalarType {
  public:
    explicit ResolvedSetScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
    [[nodiscard]] const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

} // namespace

bool GenericSetScalarType::do_match(const Type &supplied, TypeMap &bindings) const {
    const auto *set = dynamic_cast<const SetScalarType *>(&supplied);
    return set != nullptr && set->is_resolved() && item_type_->match(set->item_type(), bindings);
}

const Type &GenericSetScalarType::resolve(const TypeMap &bindings) const {
    const auto &item = item_type_->resolve(bindings);
    const auto *scalar = dynamic_cast<const ScalarType *>(&item);
    if (scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"Set"},
                                  "set item did not resolve to a scalar type"};
    }
    if (!scalar->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"Set"},
                                  "set item type does not support hashing"};
    }

    return set_scalar_type(*scalar);
}

const SetScalarType &set_scalar_type(const ScalarType &item_type) {
    static auto mutex = std::mutex{};
    static auto types = std::vector<std::unique_ptr<ResolvedSetScalarType>>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(types, [&](const auto &type) { return &type->item_type() == &item_type; });
    if (found != types.end()) {
        return **found;
    }
    if (!item_type.supports_hash()) {
        throw std::invalid_argument{"set item type does not support hashing"};
    }

    types.push_back(std::make_unique<ResolvedSetScalarType>(item_type));

    return *types.back();
}

} // namespace bg

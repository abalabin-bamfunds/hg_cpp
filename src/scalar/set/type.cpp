#include <bg/scalar/set/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include "../../type/type_registration.hpp"

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

void SetScalarType::format_name(std::string &result) const {
    result += "Set<";
    item_type().format_name(result);
    result += '>';
}

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
    static auto types = util::static_storage<std::vector<std::pair<const ScalarType *, const ResolvedSetScalarType *>>>{};
    auto &cache = types.get();

    const auto lock = std::lock_guard{details::semantic_type_registration_mutex()};
    const auto found = std::ranges::find_if(cache, [&](const auto &type) { return type.first == &item_type; });
    if (found != cache.end()) {
        return *found->second;
    }
    if (!item_type.supports_hash()) {
        throw std::invalid_argument{"set item type does not support hashing"};
    }

    cache.emplace_back(&item_type, nullptr);
    const ResolvedSetScalarType *registered_type = nullptr;
    try {
        const auto &result =
            TypeRegistry::instance().register_owned(std::make_unique<ResolvedSetScalarType>(item_type));
        registered_type = &result;
        cache.back().second = registered_type;
        if (auto *registration = details::SemanticTypeRegistration::current()) {
            registration->on_validate([&item_type] {
                if (!item_type.supports_hash()) {
                    throw std::invalid_argument{"set item type does not support hashing"};
                }
            });
            registration->on_rollback([result = registered_type] {
                std::erase_if(types.get(), [result](const auto &entry) { return entry.second == result; });
                TypeRegistry::instance().rollback(*result);
            });
        }
        return result;
    } catch (...) {
        cache.pop_back();
        if (registered_type != nullptr) {
            TypeRegistry::instance().rollback(*registered_type);
        }
        throw;
    }
}

} // namespace bg

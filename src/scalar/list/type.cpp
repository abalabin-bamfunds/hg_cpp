#include <bg/scalar/list/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include "../../type/type_registration.hpp"

#include <memory>
#include <unordered_map>

namespace bg {
namespace {

class ResolvedListScalarType final : public ListScalarType {
  public:
    explicit ResolvedListScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }

    const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

} // namespace

void ListScalarType::format_name(std::string &result) const {
    result += "List<";
    item_type().format_name(result);
    result += '>';
}

bool ListScalarType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *list = type_cast<ListScalarType>(&supplied);

    return list != nullptr && list->is_resolved() && item_type().match(list->item_type(), type_map);
}

const Type &GenericListScalarType::resolve(const TypeMap &type_map) const {
    const auto &item = item_type_->resolve(type_map);
    const auto *scalar = type_cast<ScalarType>(&item);
    if (scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"List"},
                                  "list item did not resolve to a scalar type"};
    }

    return list_scalar_type(*scalar);
}

const ListScalarType &list_scalar_type(const ScalarType &item_type) {
    static auto types = std::unordered_map<const ScalarType *, const ResolvedListScalarType *>{};

    const auto lock = std::lock_guard{details::semantic_type_registration_mutex()};
    if (const auto entry = types.find(&item_type); entry != types.end()) {
        return *entry->second;
    }

    auto entry = types.try_emplace(&item_type, nullptr).first;
    const ResolvedListScalarType *registered_type = nullptr;
    try {
        const auto &result =
            TypeRegistry::instance().register_owned(std::make_unique<ResolvedListScalarType>(item_type));
        registered_type = &result;
        entry->second = registered_type;
        if (auto *registration = details::SemanticTypeRegistration::current()) {
            registration->on_rollback([key = &item_type, result = &result] {
                types.erase(key);
                TypeRegistry::instance().rollback(*result);
            });
        }
        return result;
    } catch (...) {
        types.erase(&item_type);
        if (registered_type != nullptr) {
            TypeRegistry::instance().rollback(*registered_type);
        }
        throw;
    }
}

} // namespace bg

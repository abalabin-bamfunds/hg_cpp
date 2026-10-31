#include <bg/scalar/list/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

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
    static auto types = util::static_storage<std::unordered_map<const ScalarType *, const ResolvedListScalarType *>>{};
    auto &cache = types.get();

    if (const auto entry = cache.find(&item_type); entry != cache.end()) {
        return *entry->second;
    }

    const auto &result = static_cast<const ResolvedListScalarType &>(
        TypeRegistry::instance().register_owned(std::make_unique<ResolvedListScalarType>(item_type)));
    cache.emplace(&item_type, &result);
    return result;
}

} // namespace bg

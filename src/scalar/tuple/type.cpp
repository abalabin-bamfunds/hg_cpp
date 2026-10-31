#include <bg/scalar/tuple/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/util/symbol.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace bg {
namespace {

class ResolvedTupleScalarType final : public TupleScalarType {
  public:
    explicit ResolvedTupleScalarType(std::vector<const ScalarType *> item_types)
        : item_types_{std::move(item_types)} {}

    std::span<const ScalarType *const> item_types() const noexcept override { return item_types_; }
    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }

  private:
    std::vector<const ScalarType *> item_types_;
};

} // namespace

void TupleScalarType::format_name(std::string &result) const {
    result += "Tuple<";
    for (auto index = std::size_t{}; index < size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        item_type(index).format_name(result);
    }
    result += '>';
}

bool TupleScalarType::supports_equality() const noexcept {
    return std::ranges::all_of(item_types(), [](const auto *item) { return item->supports_equality(); });
}

bool TupleScalarType::supports_hash() const noexcept {
    return std::ranges::all_of(item_types(), [](const auto *item) { return item->supports_hash(); });
}

bool TupleScalarType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *tuple = type_cast<TupleScalarType>(&supplied);
    if (tuple == nullptr || !tuple->is_resolved() || size() != tuple->size()) {
        return false;
    }

    auto trial = type_map.overlay();
    for (auto index = std::size_t{}; index < size(); ++index) {
        if (!item_type(index).match(tuple->item_type(index), trial)) {
            return false;
        }
    }
    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTupleScalarType::resolve(const TypeMap &type_map) const {
    auto resolved = std::vector<const ScalarType *>{};
    resolved.reserve(item_types_.size());
    for (const auto *item : item_types_) {
        const auto *scalar = type_cast<ScalarType>(&item->resolve(type_map));
        if (scalar == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"Tuple"},
                                      "tuple item did not resolve to a scalar type"};
        }
        resolved.push_back(scalar);
    }
    return tuple_scalar_type(resolved);
}

const TupleScalarType &tuple_scalar_type(std::span<const ScalarType *const> item_types) {
    static auto types = util::static_storage<std::vector<const ResolvedTupleScalarType *>>{};
    auto &cache = types.get();

    const auto found = std::ranges::find_if(cache, [&](const auto *type) {
        return std::ranges::equal(type->item_types(), item_types);
    });
    if (found != cache.end()) {
        return **found;
    }

    auto new_type = std::make_unique<ResolvedTupleScalarType>(
        std::vector<const ScalarType *>{item_types.begin(), item_types.end()});
    const auto &registered = static_cast<const ResolvedTupleScalarType &>(
        TypeRegistry::instance().register_owned(std::move(new_type)));
    cache.push_back(&registered);
    return registered;
}

} // namespace bg

#include <bg/type/list_scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {
namespace {

class ResolvedListScalarType final : public ListScalarType {
  public:
    explicit ResolvedListScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override { return this == &supplied; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }

    [[nodiscard]] const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

} // namespace

bool GenericListScalarType::match(const Type &supplied, TypeMap &bindings) const {
    const auto *list = dynamic_cast<const ListScalarType *>(&supplied);

    return list != nullptr && list->is_resolved() && item_type_->match(list->item_type(), bindings);
}

const Type &GenericListScalarType::resolve(const TypeMap &bindings) const {
    const auto &item = item_type_->resolve(bindings);
    const auto *scalar = dynamic_cast<const ScalarType *>(&item);
    if (scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"List"},
                                  "list item did not resolve to a scalar type"};
    }

    return list_scalar_type(*scalar);
}

const ListScalarType &list_scalar_type(const ScalarType &item_type) {
    static auto mutex = std::mutex{};
    static auto types = std::unordered_map<const ScalarType *, std::unique_ptr<ResolvedListScalarType>>{};

    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = types.try_emplace(&item_type);
    if (inserted) {
        entry->second = std::make_unique<ResolvedListScalarType>(item_type);
    }

    return *entry->second;
}

} // namespace bg

#include <bg/type/type_of.hpp>
#include <bg/type/type_registry.hpp>

#include <memory>
#include <utility>

namespace bg {
namespace {

class ResolvedTypeScalarType final : public TypeScalarType {
  public:
    explicit ResolvedTypeScalarType(const Type &type) : TypeScalarType(type) {}

    bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

const TypeScalarType &type_of(const Type &type) {
    return TypeRegistry::instance().get_or_create<ResolvedTypeScalarType, std::hash<const Type *>>(
        &type, [&] { return std::make_unique<ResolvedTypeScalarType>(type); });
}

bool GenericTypeScalarType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *type_of = dynamic_cast<const TypeScalarType *>(&supplied);
    if (type_of == nullptr || !type_of->is_resolved()) {
        return false;
    }

    auto trial_type_map = type_map.overlay();
    if (!type().match(type_of->type(), trial_type_map)) {
        return false;
    }

    type_map.apply(std::move(trial_type_map));

    return true;
}

const Type &GenericTypeScalarType::resolve(const TypeMap &type_map) const {
    return type_of(type().resolve(type_map));
}

} // namespace bg

#include <bg/type/type_of.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace bg {
namespace {

class ResolvedTypeScalarType final : public TypeScalarType {
  public:
    explicit ResolvedTypeScalarType(const Type &type) : TypeScalarType(type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override { return this == &supplied; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

class TypeScalarTypes {
  public:
    [[nodiscard]] const TypeScalarType &get(const Type &type) {
        const std::lock_guard lock(mutex_);

        if (const auto found = types_.find(&type); found != types_.end()) {
            return *found->second;
        }

        auto type_of = std::make_unique<ResolvedTypeScalarType>(type);
        const auto *result = type_of.get();
        types_.emplace(&type, std::move(type_of));

        return *result;
    }

  private:
    std::mutex mutex_;
    std::unordered_map<const Type *, std::unique_ptr<ResolvedTypeScalarType>> types_;
};

TypeScalarTypes &type_scalar_types() {
    static TypeScalarTypes types;

    return types;
}

} // namespace

const TypeScalarType &type_of(const Type &type) {
    return type_scalar_types().get(type);
}

bool GenericTypeScalarType::match(const Type &supplied, TypeMap &bindings) const {
    const auto *type_of = dynamic_cast<const TypeScalarType *>(&supplied);
    if (type_of == nullptr || !type_of->is_resolved()) {
        return false;
    }

    auto trial_bindings = bindings.overlay();
    if (!type().match(type_of->type(), trial_bindings)) {
        return false;
    }

    bindings.apply(std::move(trial_bindings));

    return true;
}

const Type &GenericTypeScalarType::resolve(const TypeMap &bindings) const {
    return type_of(type().resolve(bindings));
}

} // namespace bg

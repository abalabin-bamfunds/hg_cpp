#include <bg/scalar/dict/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include "../type_registration.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg {
namespace {

class ResolvedDictScalarType final : public DictScalarType {
  public:
    ResolvedDictScalarType(const ScalarType &key_type, const ScalarType &mapped_type) noexcept
        : key_type_(&key_type), mapped_type_(&mapped_type) {}

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
    const ScalarType &key_type() const noexcept override { return *key_type_; }
    const ScalarType &mapped_type() const noexcept override { return *mapped_type_; }

  private:
    const ScalarType *key_type_;
    const ScalarType *mapped_type_;
};

} // namespace

bool DictScalarType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *dict = type_cast<DictScalarType>(&supplied);
    if (dict == nullptr || !dict->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    const auto key_matches = key_type().is_resolved()
                                 ? &key_type() == &dict->key_type()
                                 : key_type().match(dict->key_type(), trial);
    if (!key_matches || !mapped_type().match(dict->mapped_type(), trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericDictScalarType::resolve(const TypeMap &type_map) const {
    const auto &key = key_type_->resolve(type_map);
    const auto &mapped = mapped_type_->resolve(type_map);
    const auto *key_scalar = type_cast<ScalarType>(&key);
    const auto *mapped_scalar = type_cast<ScalarType>(&mapped);
    if (key_scalar == nullptr || mapped_scalar == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"Dict"},
                                  "dictionary components did not resolve to scalar types"};
    }
    if (!key_scalar->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"Dict"},
                                  "dictionary key type does not support hashing"};
    }

    return dict_scalar_type(*key_scalar, *mapped_scalar);
}

const DictScalarType &dict_scalar_type(const ScalarType &key_type, const ScalarType &mapped_type) {
    static auto types = std::vector<std::unique_ptr<ResolvedDictScalarType>>{};

    const auto lock = std::lock_guard{details::semantic_type_registration_mutex()};
    const auto found = std::ranges::find_if(types, [&](const auto &type) {
        return &type->key_type() == &key_type && &type->mapped_type() == &mapped_type;
    });
    if (found != types.end()) {
        return **found;
    }
    if (!key_type.supports_hash()) {
        throw std::invalid_argument{"dictionary key type does not support hashing"};
    }

    types.push_back(std::make_unique<ResolvedDictScalarType>(key_type, mapped_type));
    const auto *result = types.back().get();
    if (auto *registration = details::SemanticTypeRegistration::current()) {
        try {
            registration->on_rollback([result] {
                std::erase_if(types, [result](const auto &type) { return type.get() == result; });
            });
            registration->on_validate([&key_type] {
                if (!key_type.supports_hash()) {
                    throw std::invalid_argument{"dictionary key type does not support hashing"};
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

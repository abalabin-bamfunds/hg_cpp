#include <bg/scalar/dict/scalar_type.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/symbol.hpp>

#include "../../type/type_registration.hpp"

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

void DictScalarType::format_name(std::string &result) const {
    result += "Dict<";
    key_type().format_name(result);
    result += ", ";
    mapped_type().format_name(result);
    result += '>';
}

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
    static auto types = util::static_storage<std::vector<std::pair<std::pair<const ScalarType *, const ScalarType *>,
                                                                  const ResolvedDictScalarType *>>>{};
    auto &cache = types.get();

    const auto lock = std::lock_guard{details::semantic_type_registration_mutex()};
    const auto found = std::ranges::find_if(cache, [&](const auto &type) {
        return type.first.first == &key_type && type.first.second == &mapped_type;
    });
    if (found != cache.end()) {
        return *found->second;
    }
    if (!key_type.supports_hash()) {
        throw std::invalid_argument{"dictionary key type does not support hashing"};
    }

    cache.emplace_back(std::pair{&key_type, &mapped_type}, nullptr);
    const ResolvedDictScalarType *registered_type = nullptr;
    try {
        const auto &result = TypeRegistry::instance().register_owned(
            std::make_unique<ResolvedDictScalarType>(key_type, mapped_type));
        registered_type = &result;
        cache.back().second = registered_type;
        if (auto *registration = details::SemanticTypeRegistration::current()) {
            registration->on_validate([&key_type] {
                if (!key_type.supports_hash()) {
                    throw std::invalid_argument{"dictionary key type does not support hashing"};
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

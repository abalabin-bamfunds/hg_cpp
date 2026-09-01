#include <bg/type/time_series_dict_type.hpp>

#include <bg/type/time_series_set_type.hpp>
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

class ResolvedTimeSeriesDictionaryType final : public TimeSeriesDictionaryType {
  public:
    ResolvedTimeSeriesDictionaryType(const ScalarType &key_type, const TimeSeriesType &value_type,
                                     const TimeSeriesType &key_set_type, TimeSeriesGuarantee guarantee)
        : TimeSeriesDictionaryType(key_type, value_type, key_set_type, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

const TimeSeriesType &TimeSeriesDictionaryType::item_type(std::size_t index) const {
    if (index != 0) {
        throw std::out_of_range{"time series dictionary component index is out of range"};
    }

    return key_set_type();
}

const TimeSeriesType &TimeSeriesDictionaryType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_dictionary_type(key_type(), value_type(), guarantee);
}

bool GenericTimeSeriesDictionaryType::do_match(const Type &supplied, TypeMap &bindings) const {
    const auto *dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&supplied);
    if (dictionary == nullptr || !dictionary->is_resolved()) {
        return false;
    }

    auto trial = bindings.overlay();
    if (!key_type().match(dictionary->key_type(), trial) || !value_type().match(dictionary->value_type(), trial) ||
        !guarantee().match(*dictionary->guarantee().value(), trial)) {
        return false;
    }

    bindings.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesDictionaryType::resolve(const TypeMap &bindings) const {
    const auto &resolved_key = key_type().resolve(bindings);
    const auto &resolved_value = value_type().resolve(bindings);
    const auto *key = dynamic_cast<const ScalarType *>(&resolved_key);
    const auto *value = dynamic_cast<const TimeSeriesType *>(&resolved_value);
    if (key == nullptr || value == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"TSD"},
                                  "time series dictionary components did not resolve to the required types"};
    }
    if (!key->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, util::symbol{"TSD"},
                                  "time series dictionary key type does not support hashing"};
    }

    return time_series_dictionary_type(*key, *value, guarantee().resolve(bindings));
}

const TimeSeriesDictionaryType &time_series_dictionary_type(const ScalarType &key_type,
                                                            const TimeSeriesType &value_type,
                                                            TimeSeriesGuarantee guarantee) {
    struct Key {
        const ScalarType *key;
        const TimeSeriesType *value;
        TimeSeriesGuarantee guarantee;

        bool operator==(const Key &) const = default;
    };

    static auto mutex = std::mutex{};
    static auto types = std::vector<std::pair<Key, std::unique_ptr<ResolvedTimeSeriesDictionaryType>>>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = Key{&key_type, &value_type, guarantee};
    const auto found = std::ranges::find(types, key, &decltype(types)::value_type::first);
    if (found != types.end()) {
        return *found->second;
    }
    if (!key_type.supports_hash()) {
        throw std::invalid_argument{"time series dictionary key type does not support hashing"};
    }

    const auto &key_set = time_series_set_type(key_type, guarantee);
    types.emplace_back(key,
                       std::make_unique<ResolvedTimeSeriesDictionaryType>(key_type, value_type, key_set, guarantee));
    return *types.back().second;
}

} // namespace bg

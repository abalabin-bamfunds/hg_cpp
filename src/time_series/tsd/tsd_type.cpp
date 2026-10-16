#include <bg/type/time_series_dict_type.hpp>

#include <bg/type/time_series_set_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
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

    bool is_resolved() const noexcept override { return true; }
    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

void TimeSeriesDictionaryType::format_name(std::string &result) const {
    result += "TSD<";
    key_type().format_name(result);
    result += ", ";
    value_type().format_name(result);
    append_guarantee(result);
    result += '>';
}

const TimeSeriesType &TimeSeriesDictionaryType::reference_free_type() const {
    if (!is_resolved()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSD"},
                                  "an unresolved time series type cannot be made reference-free"};
    }
    const auto &value = value_type().reference_free_type();
    if (&value == &value_type()) {
        return *this;
    }

    return time_series_dictionary_type(key_type(), value, *guarantee().value());
}

const TimeSeriesType &TimeSeriesDictionaryType::item_type(std::size_t index) const {
    if (index != 0) {
        throw std::out_of_range{"time series dictionary component index is out of range"};
    }

    return key_set_type();
}

const TimeSeriesType &TimeSeriesDictionaryType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_dictionary_type(key_type(), value_type(), guarantee);
}

const TimeSeriesType &TimeSeriesDictionaryType::with_guarantee_limit(TimeSeriesGuarantee limit) const {
    if (!is_resolved()) {
        return TimeSeriesType::with_guarantee_limit(limit);
    }

    const auto current = *guarantee().value();
    const auto &limited_value = value_type().with_guarantee_limit(limit);
    const auto limited_guarantee = weakest(current, limit);
    if (&limited_value == &value_type() && limited_guarantee == current) {
        return *this;
    }

    return time_series_dictionary_type(key_type(), limited_value, limited_guarantee);
}

const TimeSeriesType *TimeSeriesDictionaryType::common_type(const TimeSeriesType &other) const {
    const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&other);
    if (dictionary == nullptr || !is_resolved() || !dictionary->is_resolved() ||
        &key_type() != &dictionary->key_type()) {
        return nullptr;
    }

    const auto *common_value = value_type().common_type(dictionary->value_type());
    if (common_value == nullptr) {
        return nullptr;
    }

    const auto common_guarantee = weakest(*guarantee().value(), *dictionary->guarantee().value());
    return &time_series_dictionary_type(key_type(), *common_value, common_guarantee);
}

bool TimeSeriesDictionaryType::requires_reference_representation_for(const TimeSeriesType &supplied) const noexcept {
    if (TimeSeriesType::requires_reference_representation_for(supplied)) {
        return true;
    }

    const auto *tsd = type_cast<TimeSeriesDictionaryType>(&supplied);
    if (tsd == nullptr) {
        return false;
    }

    return value_type().requires_reference_representation_for(tsd->value_type());
}

bool TimeSeriesDictionaryType::do_match(const Type &supplied, TypeMap &type_map) const {
    return do_match(supplied, type_map, std::nullopt);
}

bool TimeSeriesDictionaryType::do_match(const Type &supplied, TypeMap &type_map,
                                        std::optional<TimeSeriesGuarantee> reference_limit) const {
    const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&supplied);
    if (dictionary == nullptr || !dictionary->is_resolved()) {
        return false;
    }

    auto trial = type_map.overlay();
    const auto supplied_guarantee = *dictionary->guarantee().value();
    const auto observed_guarantee = reference_limit ? weakest(supplied_guarantee, *reference_limit)
                                                    : supplied_guarantee;
    if (!key_type().match(dictionary->key_type(), trial) ||
        !value_type().match(dictionary->value_type(), trial, reference_limit) ||
        !guarantee().match(observed_guarantee, trial)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GenericTimeSeriesDictionaryType::resolve(const TypeMap &type_map) const {
    const auto &resolved_key = key_type().resolve(type_map);
    const auto &resolved_value = value_type().resolve(type_map);
    const auto *key = type_cast<ScalarType>(&resolved_key);
    const auto *value = type_cast<TimeSeriesType>(&resolved_value);
    if (key == nullptr || value == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSD"},
                                  "time series dictionary components did not resolve to the required types"};
    }
    if (!key->supports_hash()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TSD"},
                                  "time series dictionary key type does not support hashing"};
    }

    return time_series_dictionary_type(*key, *value, guarantee().resolve(type_map));
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
    static auto types = std::vector<std::pair<Key, const ResolvedTimeSeriesDictionaryType *>>{};

    const auto &effective_value = value_type.with_guarantee(strongest(*value_type.guarantee().value(), guarantee));
    const auto lock = std::lock_guard{mutex};
    const auto key = Key{&key_type, &effective_value, guarantee};
    const auto found = std::ranges::find(types, key, &decltype(types)::value_type::first);
    if (found != types.end()) {
        return *found->second;
    }
    if (!key_type.supports_hash()) {
        throw std::invalid_argument{"time series dictionary key type does not support hashing"};
    }

    const auto &key_set = time_series_set_type(key_type, guarantee);
    types.emplace_back(key, nullptr);
    try {
        const auto &type = TypeRegistry::instance().register_owned(
            std::make_unique<ResolvedTimeSeriesDictionaryType>(key_type, effective_value, key_set, guarantee));
        types.back().second = &type;
        return type;
    } catch (...) {
        types.pop_back();
        throw;
    }
}

} // namespace bg

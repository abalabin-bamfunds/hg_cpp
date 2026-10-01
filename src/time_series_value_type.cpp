#include <bg/type/time_series_value_type.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace bg {
namespace {

class ResolvedTimeSeriesValueType final : public TimeSeriesValueType {
  public:
    ResolvedTimeSeriesValueType(const ScalarType &value_type, TimeSeriesGuarantee guarantee)
        : TimeSeriesValueType(value_type, guarantee) {}

    bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

struct TimeSeriesValueTypeKey {
    const ScalarType *value_type;
    TimeSeriesGuarantee guarantee;

    bool operator==(const TimeSeriesValueTypeKey &) const = default;
};

struct TimeSeriesValueTypeKeyHash {
    std::size_t operator()(const TimeSeriesValueTypeKey &key) const noexcept {
        const auto type = std::hash<const ScalarType *>{}(key.value_type);
        const auto guarantee = std::hash<int>{}(static_cast<int>(key.guarantee));

        return type ^ (guarantee << 1U);
    }
};

class TimeSeriesValueTypes {
  public:
    const TimeSeriesValueType &get(const ScalarType &value_type, TimeSeriesGuarantee guarantee) {
        const std::lock_guard lock(mutex_);
        const auto key = TimeSeriesValueTypeKey{&value_type, guarantee};

        if (const auto found = types_.find(key); found != types_.end()) {
            return *found->second;
        }

        auto type = std::make_unique<ResolvedTimeSeriesValueType>(value_type, guarantee);
        const auto *result = type.get();
        types_.emplace(key, std::move(type));

        return *result;
    }

  private:
    std::mutex mutex_;
    std::unordered_map<TimeSeriesValueTypeKey, std::unique_ptr<ResolvedTimeSeriesValueType>, TimeSeriesValueTypeKeyHash>
        types_;
};

TimeSeriesValueTypes &time_series_value_types() {
    static TimeSeriesValueTypes types;

    return types;
}

} // namespace

const TimeSeriesValueType &time_series_value_type(const ScalarType &value_type, TimeSeriesGuarantee guarantee) {
    if (guarantee == TimeSeriesGuarantee::deduplicated && !value_type.supports_equality()) {
        throw std::invalid_argument{"a deduplicated time series requires a scalar type that supports equality"};
    }

    return time_series_value_types().get(value_type, guarantee);
}

const TimeSeriesType &TimeSeriesValueType::with_guarantee(TimeSeriesGuarantee guarantee) const {
    return time_series_value_type(value_type(), guarantee);
}

bool TimeSeriesValueType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *time_series = type_cast<TimeSeriesValueType>(&supplied);
    if (time_series == nullptr || !time_series->is_resolved()) {
        return false;
    }

    auto trial_type_map = type_map.overlay();
    if (!value_type().match(time_series->value_type(), trial_type_map) ||
        !guarantee().match(*time_series->guarantee().value(), trial_type_map)) {
        return false;
    }

    type_map.apply(std::move(trial_type_map));
    return true;
}

const Type &GenericTimeSeriesValueType::resolve(const TypeMap &type_map) const {
    const auto &resolved_value_type = value_type().resolve(type_map);
    const auto *scalar_type = type_cast<ScalarType>(&resolved_value_type);
    if (scalar_type == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"TS"},
                                  "time series value type did not resolve to a scalar type"};
    }

    return time_series_value_type(*scalar_type, guarantee().resolve(type_map));
}

} // namespace bg

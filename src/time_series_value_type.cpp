#include <bg/type/time_series_value_type.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace bg {
namespace {

class ResolvedTimeSeriesValueType final : public TimeSeriesValueType {
  public:
    ResolvedTimeSeriesValueType(const ScalarType &value_type, TimeSeriesGuarantee guarantee)
        : TimeSeriesValueType(value_type, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override {
        return this == &supplied;
    }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

struct TimeSeriesValueTypeKey {
    const ScalarType *value_type;
    TimeSeriesGuarantee guarantee;

    bool operator==(const TimeSeriesValueTypeKey &) const = default;
};

struct TimeSeriesValueTypeKeyHash {
    [[nodiscard]] std::size_t operator()(const TimeSeriesValueTypeKey &key) const noexcept {
        const auto type = std::hash<const ScalarType *>{}(key.value_type);
        const auto guarantee = std::hash<int>{}(static_cast<int>(key.guarantee));

        return type ^ (guarantee << 1U);
    }
};

class TimeSeriesValueTypes {
  public:
    [[nodiscard]] const TimeSeriesValueType &get(const ScalarType &value_type,
                                                 TimeSeriesGuarantee guarantee) {
        const std::lock_guard lock(mutex_);
        const TimeSeriesValueTypeKey key{&value_type, guarantee};

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
    std::unordered_map<TimeSeriesValueTypeKey,
                       std::unique_ptr<ResolvedTimeSeriesValueType>,
                       TimeSeriesValueTypeKeyHash>
        types_;
};

TimeSeriesValueTypes &time_series_value_types() {
    static TimeSeriesValueTypes types;

    return types;
}

} // namespace

const TimeSeriesValueType &time_series_value_type(const ScalarType &value_type,
                                                  TimeSeriesGuarantee guarantee) {
    return time_series_value_types().get(value_type, guarantee);
}

bool GenericTimeSeriesValueType::match(const Type &supplied, TypeMap &bindings) const {
    const auto *time_series = dynamic_cast<const TimeSeriesValueType *>(&supplied);
    if (time_series == nullptr || !time_series->is_resolved()) {
        return false;
    }

    TypeMap trial_bindings = bindings.overlay();
    if (!value_type().match(time_series->value_type(), trial_bindings) ||
        !guarantee().match(*time_series->guarantee().value(), trial_bindings)) {
        return false;
    }

    bindings.apply(std::move(trial_bindings));

    return true;
}

const Type &GenericTimeSeriesValueType::resolve(const TypeMap &bindings) const {
    const Type &resolved_value_type = value_type().resolve(bindings);
    const auto *scalar_type = dynamic_cast<const ScalarType *>(&resolved_value_type);
    if (scalar_type == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding,
                                  util::symbol{"TS"},
                                  "time series value type did not resolve to a scalar type"};
    }

    return time_series_value_type(*scalar_type, guarantee().resolve(bindings));
}

} // namespace bg

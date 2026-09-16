#include <bg/time_series/ref/ref_runtime_type.hpp>

#include <bg/time_series/details/output_state.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {

void TimeSeriesReferenceRuntimeType::invalidate_output(const details::OutputState &output) const {
    *static_cast<Reference *>(output.mutable_data()) = nullptr;
    output.invalidate_value();
}

bool TimeSeriesReferenceRuntimeType::satisfies_reference_request(const TimeSeriesType &requested_type) const noexcept {
    const auto *requested = dynamic_cast<const TimeSeriesReferenceType *>(&requested_type);
    if (requested == nullptr) {
        return false;
    }

    const auto &provided = static_cast<const TimeSeriesReferenceType &>(type());
    return satisfies(*provided.guarantee().value(), *requested->guarantee().value()) &&
           requested->target_type().accepts(provided.target_type());
}

const TimeSeriesRuntimeType &time_series_reference_runtime_type(const TimeSeriesReferenceType &type) {
    static auto mutex = std::mutex{};
    static auto runtime_types =
        std::unordered_map<const TimeSeriesReferenceType *, std::unique_ptr<TimeSeriesReferenceRuntimeType>>{};

    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = runtime_types.try_emplace(&type);
    if (inserted) {
        entry->second = std::make_unique<TimeSeriesReferenceRuntimeType>(type);
    }
    return *entry->second;
}

} // namespace bg

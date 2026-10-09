#include <bg/time_series/ref/ref_runtime_type.hpp>

#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/node/details/output_factory.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {

void TimeSeriesReferenceRuntimeType::copy(const TimeSeriesInputData &input, OutputState &output) const {
    if (input.valid() && type_cast<TimeSeriesReferenceType>(&input.runtime_type().type()) == nullptr) {
        throw std::invalid_argument{"reference copy requires a reference input"};
    }

    auto source = In<REF<TS_T>>{input};
    auto target = details::output_factory::make<Out<REF<TS_T>>>(output);
    target.copy(source);
}

void TimeSeriesReferenceRuntimeType::invalidate_output(const OutputState &output) const {
    *static_cast<Reference *>(output.mutable_data()) = nullptr;
    output.invalidate_value();
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

#include <bg/time_series/ref/ref_runtime_type.hpp>

#include <bg/time_series/details/output_state.hpp>

namespace bg {

void TimeSeriesReferenceRuntimeType::invalidate_output(const details::OutputState &output) const {
    *static_cast<details::ReferenceStorage **>(output.mutable_data()) = nullptr;
    output.invalidate_value();
}

} // namespace bg

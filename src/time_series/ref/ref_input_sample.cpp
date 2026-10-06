#include <bg/time_series/ref/ref_input_sample.hpp>

#include "../tss/tss_reference_sample.hpp"

#include <cassert>

namespace bg::details {

TimeSeriesRuntimeType::ConstItem ReferenceInputSampleStorage::input() const noexcept {
    assert(set_sample() != nullptr);
    return set_sample_item(*set_sample());
}

ResolvedTimeSeries ReferenceInputSampleStorage::previous() const noexcept {
    assert(set_sample() != nullptr);
    return set_sample_previous(*set_sample());
}

void ReferenceInputSampleStorage::clear() noexcept {
    if (auto *sample = set_sample()) {
        clear_pointer();
        destroy_set_sample(sample);
    }
}

} // namespace bg::details

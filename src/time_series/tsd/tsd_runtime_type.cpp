#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

void TimeSeriesDictionaryRuntimeType::copy_tick(const TimeSeriesInputData &input,
                                                OutputState &output) const {
    if (input.valid() && type_cast<TimeSeriesDictionaryType>(&input.runtime_type().type()) == nullptr) {
        throw std::invalid_argument{"time series dictionary copy requires a dictionary input"};
    }

    auto source = In<TSD<SCALAR, TS_T>>{input};
    auto target = output_factory::make<Out<TSD<SCALAR, TS_T>>>(output);
    target.copy_tick(source);
}

void TimeSeriesDictionaryRuntimeType::copy(const TimeSeriesInputData &input, OutputState &output) const {
    if (input.valid() && type_cast<TimeSeriesDictionaryType>(&input.runtime_type().type()) == nullptr) {
        throw std::invalid_argument{"time series dictionary copy requires a dictionary input"};
    }

    auto source = In<TSD<SCALAR, TS_T>>{input};
    auto target = output_factory::make<Out<TSD<SCALAR, TS_T>>>(output);
    target.copy(source);
}

const TimeSeriesRuntimeType &
time_series_dictionary_runtime_type(const TimeSeriesDictionaryType &type, const ScalarRuntimeType &key,
                                    const TimeSeriesRuntimeType &item) {
    struct Entry {
        const TimeSeriesDictionaryType *type;
        const ScalarRuntimeType *key;
        const TimeSeriesRuntimeType *item;
        std::unique_ptr<TimeSeriesDictionaryRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(
        entries, [&](const auto &entry) { return entry.type == &type && entry.key == &key && entry.item == &item; });
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<TimeSeriesDictionaryRuntimeType>(type, key, item);
    auto &result = *runtime_type;
    entries.push_back({&type, &key, &item, std::move(runtime_type)});
    return result;
}

} // namespace bg::details

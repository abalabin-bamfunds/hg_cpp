#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/util/static_storage.hpp>

#include <memory>
#include <ranges>
#include <vector>

namespace bg::details {

void TimeSeriesDictionaryRuntimeType::invalidate_output(const OutputState &output) const {
    if (!output.valid()) {
        return;
    }

    auto memory = current_scalar_allocator();
    auto &storage = *static_cast<time_series_dict_storage *>(output.mutable_data());
    const auto evaluation_time = output.evaluation_time();
    storage.reset_change_tracking(evaluation_time, *key_, key_layout_, *item_, item_layout_, item_value_offset_,
                                  memory);
    storage.clear(*key_, key_layout_, *item_, item_layout_, item_value_offset_, memory);
    storage.key_storage().last_updated.invalidate(evaluation_time);
    output.invalidate_value();
}

bool TimeSeriesDictionaryRuntimeType::all_valid(const TimeSeriesInputData &input) const noexcept {
    return input.valid_items() ? input.valid() : all_valid(input.last_updated_ref(), input.data());
}

bool TimeSeriesDictionaryRuntimeType::all_valid(const TimeSeriesInputData &input,
                                                const TimeSeriesType &declared_type) const noexcept {
    return input.valid_items() ? input.valid() : all_valid(input.last_updated_ref(), input.data(), declared_type);
}

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

    static auto entries = util::static_storage<std::vector<Entry>>{};

    auto &cache = entries.get();
    const auto found = std::ranges::find_if(
        cache, [&](const auto &entry) { return entry.type == &type && entry.key == &key && entry.item == &item; });
    if (found != cache.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<TimeSeriesDictionaryRuntimeType>(type, key, item);
    auto &result = *runtime_type;
    cache.push_back({&type, &key, &item, std::move(runtime_type)});
    return result;
}

} // namespace bg::details

#include <bg/time_series/ref/ref_runtime_type.hpp>

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/util/static_storage.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {

namespace details {
namespace {

class RoutedTimeSeriesReferenceRuntimeType final : public TimeSeriesReferenceRuntimeType {
  public:
    explicit RoutedTimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(ItemReference), alignof(ItemReference)} {}

    Reference reference(const void *value) const override {
        return Reference{const_cast<ItemReference *>(static_cast<const ItemReference *>(value))};
    }
};

} // namespace
} // namespace details

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
    static auto mutex = util::static_storage<std::mutex>{};
    static auto runtime_types = util::static_storage<
        std::unordered_map<const TimeSeriesReferenceType *, std::unique_ptr<TimeSeriesReferenceRuntimeType>>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto [entry, inserted] = runtime_types.get().try_emplace(&type);
    if (inserted) {
        entry->second = std::make_unique<TimeSeriesReferenceRuntimeType>(type);
    }
    return *entry->second;
}

const TimeSeriesRuntimeType &details::routed_time_series_reference_runtime_type(const TimeSeriesReferenceType &type) {
    static auto mutex = util::static_storage<std::mutex>{};
    static auto runtime_types = util::static_storage<
        std::unordered_map<const TimeSeriesReferenceType *, std::unique_ptr<TimeSeriesRuntimeType>>>{};

    const auto lock = std::lock_guard{mutex.get()};
    auto &cache = runtime_types.get();
    const auto entry = cache.find(&type);
    if (entry != cache.end()) {
        return *entry->second;
    }

    auto runtime_type = std::make_unique<details::RoutedTimeSeriesReferenceRuntimeType>(type);
    auto &result = *runtime_type;
    cache.emplace(&type, std::move(runtime_type));
    return result;
}

} // namespace bg

#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/type/time_series_set_type.hpp>

#include <memory>

namespace bg::details {

class TimeSeriesSetRuntimeType final : public TimeSeriesRuntimeType {
  public:
    TimeSeriesSetRuntimeType(const TimeSeriesSetType &type, const ScalarRuntimeType &key) noexcept
        : TimeSeriesRuntimeType{type, sizeof(time_series_set_storage), alignof(time_series_set_storage),
                                true, key.requires_finalization(),     true},
          key_{&key} {}
    const ScalarRuntimeType &key_runtime_type() const noexcept { return *key_; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_set_storage *>(destination));
    }

    void invalidate_output(const details::OutputState &) const override {}

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<time_series_set_storage *>(value);
        const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
        storage->release(*key_, layout, memory, [](std::uint32_t) noexcept {});
        std::destroy_at(storage);
    }

  private:
    const ScalarRuntimeType *key_;
};

const TimeSeriesRuntimeType &time_series_set_runtime_type(const TimeSeriesSetType &type,
                                                          const ScalarRuntimeType &key);

template <typename K, auto... Properties> struct CppTimeSeriesRuntimeType<TSS<K, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return time_series_set_runtime_type(static_cast<const TimeSeriesSetType &>(type), cpp_scalar_runtime_type<K>());
    }
};

} // namespace bg::details

#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstdint>

namespace bg {

// Binds an input view to its runtime type, update time, and storage. It does not own the storage.
class TimeSeriesInputData {
  public:
    TimeSeriesInputData() = default;

    TimeSeriesInputData(const TimeSeriesRuntimeType &runtime_type, const TimeSeriesState &last_updated,
                        const void *data, bool modified = false, bool valid_items = false) noexcept
        : runtime_type_{&runtime_type, flags(modified, valid_items)},
          last_updated_{last_updated}, data_{data} {}

    bool valid() const noexcept { return last_updated_.valid(); }
    bool became_valid() const noexcept { return modified() && last_updated_.became_valid(); }
    bool became_invalid() const noexcept { return modified() && last_updated_.became_invalid(); }
    bool modified() const noexcept { return (runtime_type_.tag() & modified_flag) != 0; }
    bool valid_items() const noexcept { return (runtime_type_.tag() & valid_items_flag) != 0; }
    void set_valid_items(bool valid_items) noexcept {
        runtime_type_.reset_tag(static_cast<std::uint8_t>((runtime_type_.tag() & modified_flag) |
                                                          (valid_items ? valid_items_flag : 0)));
    }
    EvaluationTime last_updated() const noexcept { return last_updated_.time(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept {
        return *runtime_type_;
    }
    const TimeSeriesState &last_updated_ref() const noexcept { return last_updated_; }
    const void *data() const noexcept { return data_; }

    TimeSeriesInputData item(item_id_t index) const {
        if (data_ == nullptr) {
            return {runtime_type().item_runtime_type(index), initial_time_series_state, nullptr};
        }

        const auto child = runtime_type().item(data_, index);
        return {child.runtime_type, child.last_updated, child.value,
                modified() && child.last_updated.time() == last_updated()};
    }

  private:
    static constexpr std::uintptr_t modified_flag = 1;
    static constexpr std::uintptr_t valid_items_flag = 2;

    static constexpr std::uint8_t flags(bool modified, bool valid_items) noexcept {
        return static_cast<std::uint8_t>(modified | (valid_items << 1));
    }

    util::tagged_ptr<const TimeSeriesRuntimeType, 2, std::uint8_t> runtime_type_{};
    TimeSeriesState last_updated_{};
    const void *data_{};
};

static_assert(sizeof(TimeSeriesInputData) == 3 * sizeof(void *));
static_assert(alignof(TimeSeriesRuntimeType) >= 4);

} // namespace bg

namespace bg::details {

using ::bg::TimeSeriesInputData;

} // namespace bg::details

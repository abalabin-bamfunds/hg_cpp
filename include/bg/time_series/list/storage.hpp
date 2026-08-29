#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/time_series/list/type.hpp>
#include <bg/time_series/type.hpp>
#include <bg/util/index.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>

namespace bg::details {

template <std::size_t Size> class fixed_time_series_list_changes {
  public:
    [[nodiscard]] bool mark(std::size_t index, EvaluationTime evaluation_time) noexcept {
        if (time_ != evaluation_time) {
            updated_.clear();
            time_ = evaluation_time;
        }

        return updated_.set(static_cast<std::uint32_t>(index));
    }

    [[nodiscard]] bool contains(std::size_t index) const noexcept {
        return updated_.contains(static_cast<std::uint32_t>(index));
    }

    [[nodiscard]] bool contains(std::size_t index, EvaluationTime evaluation_time) const noexcept {
        return time_ == evaluation_time && contains(index);
    }

    [[nodiscard]] auto begin() const noexcept { return updated_.begin(); }
    [[nodiscard]] auto end() const noexcept { return updated_.end(); }

    [[nodiscard]] EvaluationTime time() const noexcept { return time_; }

  private:
    static_assert(Size < util::dynamic_index_extent);

    EvaluationTime time_{MIN_DT};
    util::index<static_cast<std::uint32_t>(Size)> updated_;
};

template <typename T> union time_series_value_slot {
    constexpr time_series_value_slot() noexcept {}
    ~time_series_value_slot() noexcept {}

    T value;
};

template <typename TimeSeries> struct time_series_value_storage;

template <typename T, auto... Properties> struct time_series_value_storage<TS<T, Properties...>> {
    using type = scalar_stored_t<T>;
};

template <typename Item, std::size_t Size> struct fixed_time_series_list_storage;

template <typename Item, std::size_t Size, auto... Properties>
struct time_series_value_storage<TSL<Item, Size, Properties...>> {
    using type = fixed_time_series_list_storage<Item, Size>;
};

template <typename TimeSeries> using time_series_value_storage_t = typename time_series_value_storage<TimeSeries>::type;

template <typename TimeSeries> struct contained_time_series_storage {
    EvaluationTime last_updated{MIN_DT};
    time_series_value_slot<time_series_value_storage_t<TimeSeries>> value;
};

template <typename Item, std::size_t Size> struct fixed_time_series_list_storage {
    fixed_time_series_list_changes<Size> changes;
    std::array<contained_time_series_storage<Item>, Size> items;

    [[nodiscard]] bool valid() const noexcept {
        return std::ranges::any_of(items, [](const auto &item) { return item.last_updated > MIN_DT; });
    }
};

} // namespace bg::details

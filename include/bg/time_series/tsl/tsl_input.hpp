#pragma once

#include <bg/node/details/input_properties.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/time_series/tsl/tsl_input_view.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <string_view>

namespace bg {

template <typename Item, std::size_t Size, auto... TimeSeriesProperties, auto... Properties>
class In<TSL<Item, Size, TimeSeriesProperties...>, Properties...> {
    using ParsedProperties = details::input_properties<Properties...>;
    static_assert(ParsedProperties::valid);
    static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");

  public:
    using storage_type = details::time_series_list_storage;

    In() noexcept : view_(prototype_) {}

    In(const EvaluationTime &last_updated, const storage_type *value) noexcept : In() {
        view_->bind(last_updated, value);
    }

    In(const EvaluationTime &last_updated, const void *value, const TimeSeriesInputView &prototype) noexcept
        : view_(View::copy(static_cast<const TimeSeriesListInputView &>(prototype))) {
        view_->bind(last_updated, value);
    }

    [[nodiscard]] bool valid() const noexcept { return view_->valid(); }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return view_->last_updated(); }

    [[nodiscard]] auto operator[](std::size_t index) const {
        const auto item = view_->item(index);

        return In<Item>{item.last_updated, item.value, item.prototype};
    }

    [[nodiscard]] bool item_updated(std::size_t index) const noexcept { return view_->item_updated(index); }

    template <typename Function> void for_each_updated(Function &&function) const {
        auto callback = [](void *context, std::size_t index) {
            (*static_cast<std::remove_reference_t<Function> *>(context))(index);
        };
        view_->for_each_updated(&function, callback);
    }

    [[nodiscard]] const TimeSeriesListInputView *operator->() const noexcept { return view_.operator->(); }

    using time_series_type = TSL<Item, Size, TimeSeriesProperties...>;
    using item_type = Item;

    static constexpr std::size_t size = Size;
    static constexpr std::string_view name = ParsedProperties::name;
    static constexpr InputActivity activity = ParsedProperties::activity;
    static constexpr InputValidity validity = ParsedProperties::validity;

  private:
    using View = util::polymorphic_value<TimeSeriesListInputView>;

    inline static const View prototype_ = View::copy(static_cast<const TimeSeriesListInputView &>(
        cpp_time_series_storage_representation<TSL<Item, Size, TimeSeriesProperties...>>().input_view_prototype()));
    View view_;
};

} // namespace bg

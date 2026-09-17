#include <bg/time_series/tsl/tsl_item_subscription.hpp>

#include "../ref/item_selection.hpp"

#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

namespace {

const TimeSeriesRuntimeType &item_reference_runtime_type(const TimeSeriesRuntimeType &tsl,
                                                         const TimeSeriesRuntimeType &item) {
    if (item.is_reference()) {
        return item;
    }

    const auto &list_type = static_cast<const TimeSeriesListType &>(tsl.type());
    return time_series_reference_runtime_type(time_series_reference_type(item.type(), *list_type.guarantee().value()));
}

class TslItemSubscriptions final : public ItemSelectionSubscriptions<TslItemSubscriptions, TimeSeriesRuntimeType> {
    using Base = ItemSelectionSubscriptions<TslItemSubscriptions, TimeSeriesRuntimeType>;
    friend Base;

  public:
    TslItemSubscriptions(ReferenceProvider &provider, allocator memory) : Base{provider, memory} {}

    ~TslItemSubscriptions() override { release_all(); }

    TslItemReferenceInput input(Reference reference, item_id_t index, ResolvedTimeSeries resolved,
                                const TimeSeriesRuntimeType &item_runtime_type) {
        const auto &reference_runtime_type = item_reference_runtime_type(*resolved.runtime_type, item_runtime_type);
        auto &tsl =
            require_collection(reference, *resolved.runtime_type, resolved.storage, item_runtime_type.is_reference());
        return Base::input<TslItemReferenceInput>(require_selection(tsl, index), reference_runtime_type);
    }

  private:
    static bool contains_item(const Collection &, item_id_t) noexcept { return true; }

    static void release_item_retention(Collection &, Selection &) noexcept {}
};

} // namespace

TslItemReferenceInput tsl_item_reference_input(Reference tsl, std::size_t index) {
    const auto resolved = ResolvedTimeSeries::resolve(tsl);
    if (resolved.runtime_type == nullptr || resolved.storage == nullptr) {
        return {&MIN_DT, nullptr, {}, false};
    }

    if (index >= resolved.runtime_type->item_count(resolved.storage)) {
        const auto &runtime_type = tsl_item_reference_runtime_type(*resolved.runtime_type);
        return {&MIN_DT, &runtime_type, {}, false};
    }

    const auto &item_runtime_type =
        resolved.runtime_type->item(resolved.storage, static_cast<item_id_t>(index)).runtime_type;
    auto &subscriptions = ReferenceProvider::require(tsl).require_component<TslItemSubscriptions>();
    return subscriptions.input(tsl, static_cast<item_id_t>(index), resolved, item_runtime_type);
}

const TimeSeriesRuntimeType &tsl_item_reference_runtime_type(const TimeSeriesRuntimeType &tsl) {
    const auto &list_type = static_cast<const TimeSeriesListType &>(tsl.type());
    const auto &item_runtime_type = resolve_time_series_runtime_type(list_type.item_type());
    return item_reference_runtime_type(tsl, item_runtime_type);
}

using TslObservedItems = ObservedItemCollection<TslItemSubscriptions, TimeSeriesRuntimeType>;
static_assert(sizeof(ItemSelection<TslObservedItems>) == 5 * sizeof(void *));
static_assert(sizeof(TslObservedItems) == 32 * sizeof(void *));
static_assert(sizeof(TslItemSubscriptions) == 14 * sizeof(void *));

} // namespace bg::details

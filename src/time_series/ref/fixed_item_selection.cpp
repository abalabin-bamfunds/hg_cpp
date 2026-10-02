#include <bg/time_series/ref/fixed_item_selection.hpp>

#include "item_selection.hpp"

#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

namespace {

const TimeSeriesRuntimeType &item_reference_runtime_type(const TimeSeriesRuntimeType &item) {
    if (item.is_reference()) {
        return item;
    }

    return time_series_reference_runtime_type(
        time_series_reference_type(item.type(), *item.type().guarantee().value()));
}

class FixedItemSubscriptions final
    : public ItemSelectionSubscriptions<FixedItemSubscriptions, TimeSeriesRuntimeType> {
    using Base = ItemSelectionSubscriptions<FixedItemSubscriptions, TimeSeriesRuntimeType>;

  public:
    FixedItemSubscriptions(ReferenceProvider &provider, allocator memory) : Base{provider, memory} {}

    ~FixedItemSubscriptions() override { release_all(); }

    FixedItemReferenceInput input(Reference reference, item_id_t id, ResolvedTimeSeries resolved,
                                  const TimeSeriesRuntimeType &item_runtime_type) {
        const auto &reference_runtime_type = item_reference_runtime_type(item_runtime_type);
        auto &collection = require_collection(reference, *resolved.runtime_type, resolved.storage);
        return Base::input<FixedItemReferenceInput>(
            require_selection(collection, id, item_runtime_type.is_reference()), reference_runtime_type);
    }

    static bool contains_item(const Collection &, item_id_t) noexcept { return true; }

    static void release_item_retention(Collection &, Selection &) noexcept {}
};

} // namespace

FixedItemReferenceInput fixed_item_reference_input(Reference collection, item_id_t id) {
    const auto resolved = ResolvedTimeSeries::resolve(collection);
    if (resolved.runtime_type == nullptr || resolved.storage == nullptr ||
        id >= resolved.runtime_type->item_count(resolved.storage)) {
        return {&initial_time_series_state, nullptr, {}, false};
    }

    const auto &item_runtime_type = resolved.runtime_type->item(resolved.storage, id).runtime_type;
    auto &subscriptions = ReferenceProvider::require(collection).require_component<FixedItemSubscriptions>();
    return subscriptions.input(collection, id, resolved, item_runtime_type);
}

const TimeSeriesRuntimeType &fixed_item_reference_runtime_type(const TimeSeriesType &item_type) {
    const auto &runtime_type = resolve_time_series_runtime_type(item_type);
    return item_reference_runtime_type(runtime_type);
}

using FixedObservedItems = ObservedItemCollection<FixedItemSubscriptions, TimeSeriesRuntimeType>;
static_assert(sizeof(ItemSelection<FixedObservedItems>) == 5 * sizeof(void *));
static_assert(sizeof(FixedObservedItems) == 32 * sizeof(void *));
static_assert(sizeof(FixedItemSubscriptions) == 14 * sizeof(void *));

} // namespace bg::details

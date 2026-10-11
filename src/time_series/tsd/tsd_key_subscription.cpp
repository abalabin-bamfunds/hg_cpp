#include <bg/time_series/tsd/tsd_key_subscription.hpp>

#include "../ref/item_selection.hpp"

#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/scope_exit.hpp>

namespace bg::details {

namespace {

const TimeSeriesReferenceType &resolved_tsd_item_reference_type(const TimeSeriesDictionaryRuntimeType &tsd) {
    const auto &dictionary_type = static_cast<const TimeSeriesDictionaryType &>(tsd.type());
    if (tsd.item_runtime_type().is_reference()) {
        return static_cast<const TimeSeriesReferenceType &>(dictionary_type.value_type());
    }

    return time_series_reference_type(dictionary_type.value_type(), *dictionary_type.guarantee().value());
}

class TsdKeySubscriptions final
    : public ItemSelectionSubscriptions<TsdKeySubscriptions, TimeSeriesDictionaryRuntimeType> {
    using Base = ItemSelectionSubscriptions<TsdKeySubscriptions, TimeSeriesDictionaryRuntimeType>;

  public:
    TsdKeySubscriptions(ReferenceProvider &provider, allocator memory) : Base{provider, memory} {}

    ~TsdKeySubscriptions() override { release_all(); }

    TsdItemReferenceInput input(Reference reference, Scalar<> key) {
        const auto resolved = ResolvedTimeSeries::resolve(reference);
        if (resolved.runtime_type == nullptr || resolved.storage == nullptr) {
            return {&initial_time_series_state, nullptr, {}, false};
        }
        const auto &view_runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*resolved.runtime_type);
        const auto retained = view_runtime_type.retain_key(const_cast<void *>(resolved.storage), scalar_data(key),
                                                           allocation());
        if (retained.runtime_type == nullptr) {
            return {&initial_time_series_state, nullptr, {}, false};
        }
        auto release_retention = util::scope_exit{
            [&]() noexcept { retained.runtime_type->release_key(retained.value, retained.id, allocation()); }};

        const auto &reference_runtime_type = time_series_reference_runtime_type(
            resolved_tsd_item_reference_type(view_runtime_type));
        const auto stores_reference = view_runtime_type.item_runtime_type().is_reference();
        auto &tsd = require_collection(reference, view_runtime_type, *retained.runtime_type, retained.value);
        auto &entry = require_selection(tsd, retained.id, stores_reference);
        if (!entry.retained()) {
            entry.set_retained(true);
            release_retention.release();
        }

        return Base::input<TsdItemReferenceInput>(entry, resolved, reference_runtime_type);
    }

    static bool retention_matches(const Collection &tsd, ResolvedTimeSeries resolved) noexcept {
        if (resolved.storage == nullptr) {
            return false;
        }
        const auto &view = static_cast<const TimeSeriesDictionaryRuntimeType &>(*resolved.runtime_type);
        const auto retention = view.retention_storage(const_cast<void *>(resolved.storage));
        return retention.runtime_type == tsd.runtime_type && retention.value == tsd.storage;
    }

    static bool contains_item(const Collection &tsd, ResolvedTimeSeries resolved, item_id_t id) noexcept {
        return tsd.view_runtime_type->key_live(resolved.storage, id);
    }

    void release_item_retention(Collection &tsd, Selection &entry) noexcept {
        if (!entry.retained()) {
            return;
        }

        tsd.runtime_type->release_key(const_cast<void *>(tsd.storage), entry.id, allocation());
        entry.set_retained(false);
    }
};

} // namespace

TsdItemReferenceInput tsd_item_reference_input(Reference tsd, Scalar<> key) {
    auto &subscriptions = ReferenceProvider::require(tsd).require_component<TsdKeySubscriptions>();
    return subscriptions.input(tsd, key);
}

const TimeSeriesRuntimeType &tsd_item_reference_runtime_type(const TimeSeriesRuntimeType &tsd) {
    return time_series_reference_runtime_type(
        resolved_tsd_item_reference_type(static_cast<const TimeSeriesDictionaryRuntimeType &>(tsd)));
}

using TsdObservedItems = ObservedItemCollection<TsdKeySubscriptions, TimeSeriesDictionaryRuntimeType>;
static_assert(sizeof(ItemSelection<TsdObservedItems>) == 5 * sizeof(void *));
static_assert(sizeof(TsdObservedItems) == 33 * sizeof(void *));
static_assert(sizeof(TsdKeySubscriptions) == 14 * sizeof(void *));

} // namespace bg::details

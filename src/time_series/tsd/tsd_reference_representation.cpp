#include "tsd_reference_representation.hpp"

#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {
namespace {

class TimeSeriesDictionaryRepresentationRuntimeType final : public TimeSeriesDictionaryRuntimeType {
  public:
    TimeSeriesDictionaryRepresentationRuntimeType(const TimeSeriesDictionaryType &type,
                                                  const ScalarRuntimeType &key,
                                                  const TimeSeriesRuntimeType &item)
        : TimeSeriesDictionaryRuntimeType{type, key, item, 0, 1, false, false, false} {}

    RetainedDictionaryKey retain_key(void *value, const ScalarData &key, allocator &memory) const override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        return base.storage == nullptr ? RetainedDictionaryKey{}
                                       : base_runtime_type(base).retain_key(const_cast<void *>(base.storage), key,
                                                                            memory);
    }

    void release_key(void *, item_id_t, allocator &) const noexcept override {
        std::terminate();
    }

    item_id_t find_key(const void *value, const ScalarData &key) const override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        return base.storage == nullptr ? util::occupancy_index::npos
                                       : base_runtime_type(base).find_key(base.storage, key);
    }

    bool key_live(const void *value, item_id_t id) const noexcept override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        return base.storage != nullptr && base_runtime_type(base).key_live(base.storage, id);
    }

    const void *key(const void *value, item_id_t id) const noexcept override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        return base.storage == nullptr ? nullptr : base_runtime_type(base).key(base.storage, id);
    }

    void for_each_added(const TimeSeriesState &last_updated, const void *value, void *context,
                        ItemIdFunction function) const override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        if (base.storage != nullptr) {
            base_runtime_type(base).for_each_added(last_updated, base.storage, context, function);
        }
    }

    void for_each_removed(const TimeSeriesState &last_updated, const void *value, void *context,
                          ItemIdFunction function) const override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        if (base.storage != nullptr) {
            base_runtime_type(base).for_each_removed(last_updated, base.storage, context, function);
        }
    }

    void for_each_removed_physical_id(const TimeSeriesState &last_updated, const void *value, void *context,
                                      ItemIdFunction function) const override {
        const auto base = representation(value).base_time_series();
        if (base.runtime_type != nullptr && base.storage != nullptr) {
            base_runtime_type(base).for_each_removed_physical_id(last_updated, base.storage, context, function);
        }
    }

    void for_each_modified(const TimeSeriesState &last_updated, const void *value, void *context,
                           ItemIdFunction function) const override {
        representation(value).for_each_modified(last_updated, context, function);
    }

    item_id_t next_item_id(const TimeSeriesState &last_updated, const void *value, ItemRange range,
                           item_id_t start) const override {
        const auto &view = representation(value);
        if (range == ItemRange::modified) {
            return view.next_modified_item_id(last_updated, start);
        }
        const auto base = view.base_time_series();
        return base.storage == nullptr
                   ? util::occupancy_index::npos
                   : base_runtime_type(base).next_item_id(last_updated, base.storage, range, start);
    }

    bool item_modified(const TimeSeriesState &last_updated, const void *value, item_id_t id) const override {
        return representation(value).item_modified(last_updated, id);
    }

    ItemLayout item_layout(item_id_t id) const override {
        (void)id;
        throw std::logic_error{"a TSD reference representation has no packed output layout"};
    }

    std::size_t item_count(const void *value) const override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        return base.storage == nullptr ? 0 : base_runtime_type(base).item_count(base.storage);
    }

    ConstItem item(const void *value, item_id_t id) const override {
        const auto &view = representation(value);
        if (id != key_set_item_id) {
            return view.item(id);
        }
        const auto base = view.base_time_series();
        return base.storage == nullptr ? ConstItem{key_set_runtime_type(), initial_time_series_state, nullptr}
                                       : base_runtime_type(base).item(base.storage, id);
    }

    MutableItem item(void *, item_id_t) const override {
        throw std::logic_error{"a TSD reference representation is immutable"};
    }

    bool mark_item_modified(void *, item_id_t, EvaluationTime, const TimeSeriesState &, allocator &) const
        noexcept override {
        std::terminate();
    }

    void copy_tick(const TimeSeriesInputData &, OutputState &) const override {
        throw std::logic_error{"a TSD reference representation cannot be an output"};
    }

    void copy(const TimeSeriesInputData &, OutputState &) const override {
        throw std::logic_error{"a TSD reference representation cannot be an output"};
    }

    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        if (base.storage != nullptr) {
            base_runtime_type(base).for_each_item_id(base.storage, context, function);
        }
    }

    bool has_item_id_remapping(const void *value) const override {
        const auto base = representation(value).base_time_series();
        return base.runtime_type != nullptr && base.storage != nullptr &&
               base_runtime_type(base).has_item_id_remapping(base.storage);
    }

    void for_each_item_id_remapping(const void *value, void *context,
                                    ItemIdRemappingFunction function) const override {
        const auto base = representation(value).base_time_series();
        if (base.runtime_type != nullptr && base.storage != nullptr) {
            base_runtime_type(base).for_each_item_id_remapping(base.storage, context, function);
        }
    }

    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t id) const override {
        return representation(value).item_updated(last_updated, id);
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        representation(value).for_each_updated_item(last_updated, context, function);
    }

    bool collection_valid(const void *value) const noexcept override {
        const auto &view = representation(value);
        const auto base = view.base_time_series();
        return base.storage != nullptr && base_runtime_type(base).collection_valid(base.storage);
    }

    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value,
                                EvaluationTime evaluation_time, void *context,
                                ModifiedItemFunction function) const override {
        representation(value).for_each_modified_item(last_updated, evaluation_time, context, function);
    }

    void construct_initial_value(void *) const noexcept override { std::terminate(); }
    void destroy_value(void *, allocator &) const noexcept override { std::terminate(); }

  protected:
    bool item_in_range(const TimeSeriesState &last_updated, const void *value, ItemRange range,
                       item_id_t id) const override {
        if (range == ItemRange::modified) {
            return representation(value).item_modified(last_updated, id);
        }
        return next_item_id(last_updated, value, range, id) == id;
    }

  private:
    static const TimeSeriesDictionaryRuntimeType &base_runtime_type(const ResolvedTimeSeries &base) noexcept {
        return static_cast<const TimeSeriesDictionaryRuntimeType &>(*base.runtime_type);
    }

    static const TimeSeriesDictionaryRepresentation &representation(const void *value) noexcept {
        return *static_cast<const TimeSeriesDictionaryRepresentation *>(value);
    }
};

class RoutedTsdItemReferenceRuntimeType final : public TimeSeriesReferenceRuntimeType {
  public:
    explicit RoutedTsdItemReferenceRuntimeType(const TimeSeriesReferenceType &type) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(ItemReference), alignof(ItemReference)} {}

    Reference reference(const void *value) const override {
        return Reference{const_cast<ItemReference *>(static_cast<const ItemReference *>(value))};
    }
};

bool representation_required(const TimeSeriesRuntimeType &provided, const TimeSeriesRuntimeType &requested) noexcept {
    const auto &provided_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(provided);
    const auto &requested_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(requested);
    if (provided_tsd.item_runtime_type().is_reference() != requested_tsd.item_runtime_type().is_reference()) {
        return true;
    }
    return ReferenceProvider::representation_required(provided_tsd.item_runtime_type(),
                                                      requested_tsd.item_runtime_type());
}

const TimeSeriesRuntimeType &representation_runtime_type(const TimeSeriesRuntimeType &provided,
                                                         const TimeSeriesRuntimeType &requested) {
    const auto &provided_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(provided);
    const auto &requested_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(requested);

    if (!provided_tsd.item_runtime_type().is_reference() && requested_tsd.item_runtime_type().is_reference()) {
        const auto &provided_type = static_cast<const TimeSeriesDictionaryType &>(provided_tsd.type());
        const auto item_guarantee = strongest(TimeSeriesGuarantee::final, *provided_type.guarantee().value());
        const auto &reference_type = time_series_reference_type(provided_tsd.item_runtime_type().type(),
                                                               item_guarantee);
        const auto &representation_type = time_series_dictionary_type(
            provided_type.key_type(), reference_type, *provided_type.guarantee().value());
        const auto &routed_item_type = routed_tsd_item_reference_runtime_type(reference_type);
        return time_series_dictionary_representation_runtime_type(representation_type, provided_tsd.key_runtime_type(),
                                                                  routed_item_type);
    }
    if (provided_tsd.item_runtime_type().is_reference() && !requested_tsd.item_runtime_type().is_reference()) {
        const auto &requested_type = static_cast<const TimeSeriesDictionaryType &>(requested_tsd.type());
        return time_series_dictionary_representation_runtime_type(requested_type, provided_tsd.key_runtime_type(),
                                                                  requested_tsd.item_runtime_type());
    }

    throw std::invalid_argument{"the requested TSD representation is not implemented"};
}

ReferenceRepresentation *create_representation(ReferenceProvider &provider, Reference base_reference,
                                               const TimeSeriesRuntimeType &provided,
                                               const TimeSeriesRuntimeType &requested, allocator &memory) {
    const auto &provided_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(provided);
    const auto &requested_tsd = static_cast<const TimeSeriesDictionaryRuntimeType &>(requested);
    const auto &requested_type = static_cast<const TimeSeriesDictionaryType &>(requested_tsd.type());
    if (!provided_tsd.item_runtime_type().is_reference() && requested_tsd.item_runtime_type().is_reference()) {
        const auto &reference_type = static_cast<const TimeSeriesReferenceType &>(
            requested_tsd.item_runtime_type().type());
        const auto &routed_item_type = routed_tsd_item_reference_runtime_type(reference_type);
        return make_tsd_item_reference_representation(provider, base_reference, requested_type, provided_tsd,
                                                      routed_item_type, memory);
    }
    if (provided_tsd.item_runtime_type().is_reference() && !requested_tsd.item_runtime_type().is_reference()) {
        return make_dereferenced_tsd_representation(provider, base_reference, requested_type, provided_tsd,
                                                    requested_tsd.item_runtime_type(), memory);
    }

    throw std::invalid_argument{"the requested TSD representation is not implemented"};
}

const ReferenceRepresentationFactory factory{&representation_required, &representation_runtime_type,
                                             &create_representation};

} // namespace

TimeSeriesDictionaryRepresentation::TimeSeriesDictionaryRepresentation(
    Reference base_reference, const TimeSeriesDictionaryType &type,
    const TimeSeriesDictionaryRuntimeType &base_runtime_type, const TimeSeriesRuntimeType &item_runtime_type)
    : ReferenceRepresentation{base_reference}, type_{&type}, item_runtime_type_{&item_runtime_type},
      runtime_type_{&time_series_dictionary_representation_runtime_type(
          type, base_runtime_type.key_runtime_type(), item_runtime_type)} {}

void TimeSeriesDictionaryRepresentation::refresh_runtime_type() {
    const auto base = base_time_series();
    if (base.runtime_type == nullptr) {
        return;
    }

    const auto &base_runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*base.runtime_type);
    if (&base_runtime_type.key_runtime_type() == &runtime_type_->key_runtime_type()) {
        return;
    }

    runtime_type_ = &time_series_dictionary_representation_runtime_type(
        *type_, base_runtime_type.key_runtime_type(), *item_runtime_type_);
}

const TimeSeriesDictionaryRuntimeType &time_series_dictionary_representation_runtime_type(
    const TimeSeriesDictionaryType &type, const ScalarRuntimeType &key, const TimeSeriesRuntimeType &item) {
    struct Entry {
        const TimeSeriesDictionaryType *type;
        const ScalarRuntimeType *key;
        const TimeSeriesRuntimeType *item;
        std::unique_ptr<TimeSeriesDictionaryRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.type == &type && entry.key == &key && entry.item == &item;
    });
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<TimeSeriesDictionaryRepresentationRuntimeType>(type, key, item);
    auto &result = *runtime_type;
    entries.push_back({&type, &key, &item, std::move(runtime_type)});
    return result;
}

const TimeSeriesRuntimeType &routed_tsd_item_reference_runtime_type(const TimeSeriesReferenceType &type) {
    struct Entry {
        const TimeSeriesReferenceType *type;
        std::unique_ptr<TimeSeriesRuntimeType> runtime_type;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find(entries, &type, &Entry::type);
    if (found != entries.end()) {
        return *found->runtime_type;
    }

    auto runtime_type = std::make_unique<RoutedTsdItemReferenceRuntimeType>(type);
    auto &result = *runtime_type;
    entries.push_back({&type, std::move(runtime_type)});
    return result;
}

const ReferenceRepresentationFactory *TimeSeriesDictionaryRuntimeType::reference_representation_factory()
    const noexcept {
    return &factory;
}

} // namespace bg::details

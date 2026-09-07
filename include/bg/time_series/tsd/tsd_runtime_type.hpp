#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/util/occupancy_index.hpp>

#include <memory>
#include <stdexcept>

namespace bg::details {

class TimeSeriesDictionaryRuntimeType : public TimeSeriesRuntimeType {
  public:
    TimeSeriesDictionaryRuntimeType(const TimeSeriesDictionaryType &type, const ScalarRuntimeType &key,
                                    const TimeSeriesRuntimeType &item)
        : TimeSeriesRuntimeType{type,
                                sizeof(time_series_dict_storage),
                                alignof(time_series_dict_storage),
                                true,
                                key.requires_finalization() || item.requires_finalization(),
                                true},
          key_{&key},
          keys_{&time_series_set_runtime_type(static_cast<const TimeSeriesSetType &>(type.key_set_type()), key)},
          item_{&item}, key_layout_{key.size(), key.alignment()}, item_layout_{contained_time_series_layout(item)},
          item_value_offset_{contained_time_series_value_offset(item)} {}

    const ScalarRuntimeType &key_runtime_type() const noexcept { return *key_; }
    const TimeSeriesRuntimeType &item_runtime_type() const noexcept { return *item_; }

    virtual TimeSeriesItemId find_item(const void *value, const ScalarData &key) const {
        const auto &keys = static_cast<const time_series_dict_storage *>(value)->key_storage().value;
        const auto id = keys.find_position(key, *key_, key_layout_);
        return id != util::occupancy_index::npos && keys.live().contains(id) ? id : util::occupancy_index::npos;
    }

    virtual const void *key(const void *value, TimeSeriesItemId id) const noexcept {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().value.key(id, key_layout_);
    }

    virtual void for_each_added(const EvaluationTime &last_updated, const void *value, void *context,
                                ItemIdFunction function) const {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (storage.change_time() != last_updated) {
            return;
        }
        for (const auto id : storage.key_storage().value.added()) {
            function(context, id);
        }
    }

    virtual void for_each_removed(const EvaluationTime &last_updated, const void *value, void *context,
                                  ItemIdFunction function) const {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (last_updated <= MIN_DT || storage.change_time() != last_updated) {
            return;
        }
        for (const auto id : storage.key_storage().value.removed()) {
            function(context, id);
        }
    }

    virtual void for_each_modified(const EvaluationTime &last_updated, const void *value, void *context,
                                   ItemIdFunction function) const {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (last_updated <= MIN_DT || storage.change_time() != last_updated) {
            return;
        }
        const auto &keys = storage.key_storage().value;
        for (const auto id : storage.modified()) {
            if (!keys.added().contains(id)) {
                function(context, id);
            }
        }
    }

    virtual bool item_modified(const EvaluationTime &last_updated, const void *value,
                                             TimeSeriesItemId id) const noexcept {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        return last_updated > MIN_DT && storage.change_time() == last_updated && storage.modified().contains(id) &&
               !storage.key_storage().value.added().contains(id);
    }

    ItemLayout item_layout(TimeSeriesItemId id) const override {
        if (id != key_set_item_id) {
            throw std::out_of_range{"time series dictionary item is not in packed output storage"};
        }
        return {*keys_, time_series_dict_storage::key_last_updated_offset(),
                time_series_dict_storage::key_value_offset()};
    }

    std::size_t item_count(const void *value) const override {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().value.size();
    }

    ConstItem item(const void *value, TimeSeriesItemId id) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (id == key_set_item_id) {
            return {*keys_, storage.key_storage().last_updated, &storage.key_storage().value};
        }
        return {*item_, storage.item_last_updated(id, item_layout_),
                storage.item_value(id, item_layout_, item_value_offset_)};
    }

    MutableItem item(void *value, TimeSeriesItemId id) const override {
        auto &storage = *static_cast<time_series_dict_storage *>(value);
        if (id == key_set_item_id) {
            return {*keys_, storage.key_storage().last_updated, &storage.key_storage().value};
        }
        return {*item_, storage.item_last_updated(id, item_layout_),
                storage.item_value(id, item_layout_, item_value_offset_)};
    }

    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override {
        const auto &keys = static_cast<const time_series_dict_storage *>(value)->key_storage().value;
        for (const auto id : keys.live()) {
            function(context, id);
        }
    }

    bool item_updated(const EvaluationTime &last_updated, const void *value,
                                    TimeSeriesItemId id) const noexcept override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (last_updated <= MIN_DT || storage.change_time() != last_updated) {
            return false;
        }
        if (id == key_set_item_id) {
            return storage.key_storage().last_updated == last_updated;
        }
        const auto &keys = storage.key_storage().value;
        return keys.added().contains(id) || keys.removed().contains(id) || storage.modified().contains(id);
    }

    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (last_updated <= MIN_DT || storage.change_time() != last_updated) {
            return;
        }

        const auto &keys = storage.key_storage().value;
        if (storage.key_storage().last_updated == last_updated) {
            function(context, key_set_item_id);
        }
        for (const auto id : keys.added()) {
            function(context, id);
        }
        for (const auto id : storage.modified()) {
            if (!keys.added().contains(id)) {
                function(context, id);
            }
        }
        for (const auto id : keys.removed()) {
            function(context, id);
        }
    }

    [[nodiscard]] bool mark_item_modified(void *value, TimeSeriesItemId id, EvaluationTime evaluation_time,
                                          EvaluationTime, std::pmr::memory_resource &memory) const noexcept override {
        if (id == key_set_item_id) {
            return true;
        }
        return static_cast<time_series_dict_storage *>(value)->mark_item_modified(id, evaluation_time, memory);
    }

    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().last_updated > MIN_DT;
    }

    void invalidate_output(const details::OutputState &) const override {}

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        const auto &keys = storage.key_storage().value;
        const auto publish_item = [&](std::uint32_t position, const EvaluationTime &item_last_updated) {
            function(context, {position, item_, &item_last_updated,
                               storage.item_value(position, item_layout_, item_value_offset_)});
        };

        if (last_updated <= MIN_DT) {
            function(context, {key_set_item_id, keys_, &MIN_DT, &storage.key_storage().value});
            for (const auto position : keys.removed()) {
                publish_item(position, MIN_DT);
            }
            return;
        }
        if (storage.change_time() != evaluation_time) {
            return;
        }

        if (storage.key_storage().last_updated == evaluation_time) {
            function(context,
                     {key_set_item_id, keys_, &storage.key_storage().last_updated, &storage.key_storage().value});
        }
        for (const auto position : keys.added()) {
            publish_item(position, storage.item_last_updated(position, item_layout_));
        }
        for (const auto position : storage.modified()) {
            if (!keys.added().contains(position)) {
                publish_item(position, storage.item_last_updated(position, item_layout_));
            }
        }
        for (const auto position : keys.removed()) {
            publish_item(position, MIN_DT);
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_dict_storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<time_series_dict_storage *>(value);
        storage->release(*key_, key_layout_, *item_, item_layout_, item_value_offset_, memory);
        std::destroy_at(storage);
    }

    [[nodiscard]] ReferenceRepresentation *
    create_reference_representation(ReferenceProvider &provider, ReferenceAddress source,
                                    const TimeSeriesType &requested_type,
                                    std::pmr::memory_resource &memory) const override;

  private:
    const ScalarRuntimeType *key_;
    const TimeSeriesRuntimeType *keys_;
    const TimeSeriesRuntimeType *item_;
    util::runtime_item_layout key_layout_;
    util::runtime_item_layout item_layout_;
    std::size_t item_value_offset_;
};

const TimeSeriesRuntimeType &
time_series_dictionary_runtime_type(const TimeSeriesDictionaryType &type, const ScalarRuntimeType &key,
                                    const TimeSeriesRuntimeType &item);

template <typename K, typename Item, auto... Properties> struct CppTimeSeriesRuntimeType<TSD<K, Item, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        const auto &dictionary = static_cast<const TimeSeriesDictionaryType &>(type);
        return time_series_dictionary_runtime_type(dictionary, cpp_scalar_runtime_type<K>(),
                                                   CppTimeSeriesRuntimeType<Item>::get(dictionary.value_type()));
    }
};

} // namespace bg::details

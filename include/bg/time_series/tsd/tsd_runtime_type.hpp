#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/util/occupancy_index.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace bg::details {

class TimeSeriesDictionaryRuntimeType;

struct DictionaryKeyStorage {
    const TimeSeriesDictionaryRuntimeType *runtime_type{};
    void *value{};
};

struct RetainedDictionaryKey {
    const TimeSeriesDictionaryRuntimeType *runtime_type{};
    void *value{};
    item_id_t id{util::occupancy_index::npos};
};

class TimeSeriesDictionaryRuntimeType : public TimeSeriesRuntimeType {
  public:
    enum class ItemRange : std::uint8_t { current, added, removed, modified };

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
    const TimeSeriesRuntimeType &key_set_runtime_type() const noexcept { return *keys_; }
    const TimeSeriesRuntimeType &item_runtime_type() const noexcept { return *item_; }

    virtual DictionaryKeyStorage retention_storage(void *value) const noexcept { return {this, value}; }

    virtual RetainedDictionaryKey retain_key(void *value, const ScalarData &key, allocator &memory) const {
        auto &keys = static_cast<time_series_dict_storage *>(value)->key_storage().value;
        return {this, value, keys.retain(key, *key_, key_layout_, memory)};
    }

    virtual void release_key(void *value, item_id_t id, allocator &memory) const noexcept {
        auto &keys = static_cast<time_series_dict_storage *>(value)->key_storage().value;
        keys.release_retention(id, memory);
    }

    virtual item_id_t find_key(const void *value, const ScalarData &key) const {
        const auto &keys = static_cast<const time_series_dict_storage *>(value)->key_storage().value;
        return keys.find_position(key, *key_, key_layout_);
    }

    virtual bool key_live(const void *value, item_id_t id) const noexcept {
        const auto &keys = static_cast<const time_series_dict_storage *>(value)->key_storage().value;
        return keys.live().contains(id);
    }

    virtual item_id_t find_item(const void *value, const ScalarData &key) const {
        const auto id = find_key(value, key);
        return id != util::occupancy_index::npos && key_live(value, id) ? id : util::occupancy_index::npos;
    }

    virtual item_id_t find_added(const TimeSeriesState &last_updated, const void *value, const ScalarData &key) const {
        return find_changed(last_updated, value, key, ItemRange::added);
    }

    virtual item_id_t find_removed(const TimeSeriesState &last_updated, const void *value,
                                   const ScalarData &key) const {
        return find_changed(last_updated, value, key, ItemRange::removed);
    }

    virtual item_id_t find_modified(const TimeSeriesState &last_updated, const void *value,
                                    const ScalarData &key) const {
        return find_changed(last_updated, value, key, ItemRange::modified);
    }

    virtual const void *key(const void *value, item_id_t id) const noexcept {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().value.key(id, key_layout_);
    }

    virtual void for_each_added(const TimeSeriesState &last_updated, const void *value, void *context,
                                ItemIdFunction function) const {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return;
        }
        for (const auto id : storage.key_storage().value.added()) {
            function(context, id);
        }
    }

    virtual void for_each_removed(const TimeSeriesState &last_updated, const void *value, void *context,
                                  ItemIdFunction function) const {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return;
        }
        for (const auto id : storage.key_storage().value.removed()) {
            function(context, id);
        }
    }

    virtual void for_each_modified(const TimeSeriesState &last_updated, const void *value, void *context,
                                   ItemIdFunction function) const {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return;
        }
        const auto &keys = storage.key_storage().value;
        for (const auto id : storage.modified()) {
            if (!keys.added().contains(id)) {
                function(context, id);
            }
        }
    }

    // Returns the first item ID at or after start. A sampled removed item retains
    // its existing previous-item flag so that key() and item() select its source.
    virtual item_id_t next_item_id(const TimeSeriesState &last_updated, const void *value, ItemRange range,
                                   item_id_t start) const noexcept {
        if (value == nullptr) {
            return util::occupancy_index::npos;
        }

        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        const auto &keys = storage.key_storage().value;
        const auto next = [&](const util::index<> &ids) {
            const auto id = ids.first_set(start);
            return id == ids.size() ? util::occupancy_index::npos : id;
        };

        if (range == ItemRange::current) {
            return next(keys.live());
        }
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return util::occupancy_index::npos;
        }
        if (range == ItemRange::added) {
            return next(keys.added());
        }
        if (range == ItemRange::removed) {
            return next(keys.removed());
        }

        auto id = next(storage.modified());
        while (id != util::occupancy_index::npos && keys.added().contains(id)) {
            const auto next_id = storage.modified().first_set(id + 1);
            id = next_id == storage.modified().size() ? util::occupancy_index::npos : next_id;
        }
        return id;
    }

    virtual bool item_modified(const TimeSeriesState &last_updated, const void *value, item_id_t id) const noexcept {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        return last_updated.valid() && storage.change_time() == last_updated.time() &&
               storage.modified().contains(id) &&
               !storage.key_storage().value.added().contains(id);
    }

    ItemLayout item_layout(item_id_t id) const override {
        if (id != key_set_item_id) {
            throw std::out_of_range{"time series dictionary item is not in packed output storage"};
        }
        return {*keys_, time_series_dict_storage::key_last_updated_offset(),
                time_series_dict_storage::key_value_offset()};
    }

    std::size_t item_count(const void *value) const override {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().value.size();
    }

    ConstItem item(const void *value, item_id_t id) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (id == key_set_item_id) {
            return {*keys_, storage.key_storage().last_updated, &storage.key_storage().value};
        }
        return {*item_, storage.item_last_updated(id, item_layout_),
                storage.item_value(id, item_layout_, item_value_offset_)};
    }

    MutableItem item(void *value, item_id_t id) const override {
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

    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t id) const noexcept override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return false;
        }
        if (id == key_set_item_id) {
            return storage.key_storage().last_updated.time() == last_updated.time();
        }
        const auto &keys = storage.key_storage().value;
        return keys.added().contains(id) || keys.removed().contains(id) || storage.modified().contains(id);
    }

    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return;
        }

        const auto &keys = storage.key_storage().value;
        if (storage.key_storage().last_updated.time() == last_updated.time()) {
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

    [[nodiscard]] bool mark_item_modified(void *value, item_id_t id, EvaluationTime evaluation_time,
                                          const TimeSeriesState &,
                                          allocator &memory) const noexcept override {
        if (id == key_set_item_id) {
            return true;
        }
        return static_cast<time_series_dict_storage *>(value)->mark_item_modified(id, evaluation_time, memory);
    }

    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const time_series_dict_storage *>(value)->key_storage().last_updated.valid();
    }

    void invalidate_output(const OutputState &) const override {}

    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        const auto &keys = storage.key_storage().value;
        const auto publish_item = [&](std::uint32_t position, const TimeSeriesState &item_last_updated) {
            function(context, {position, item_, &item_last_updated,
                               storage.item_value(position, item_layout_, item_value_offset_)});
        };

        if (!last_updated.valid()) {
            function(context, {key_set_item_id, keys_, &last_updated, &storage.key_storage().value});
            for (const auto position : keys.live()) {
                publish_item(position, last_updated);
            }
            for (const auto position : keys.removed()) {
                publish_item(position, last_updated);
            }
            return;
        }
        if (storage.change_time() != evaluation_time) {
            return;
        }

        if (storage.key_storage().last_updated.time() == evaluation_time) {
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
            publish_item(position, initial_time_series_state);
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_dict_storage *>(destination));
    }

    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto *storage = static_cast<time_series_dict_storage *>(value);
        storage->release(*key_, key_layout_, *item_, item_layout_, item_value_offset_, memory);
        std::destroy_at(storage);
    }

    const ReferenceRepresentationFactory *reference_representation_factory() const noexcept override;

  private:
    virtual bool item_in_range(const TimeSeriesState &last_updated, const void *value, ItemRange range,
                               item_id_t id) const noexcept {
        const auto &storage = *static_cast<const time_series_dict_storage *>(value);
        if (!last_updated.valid() || storage.change_time() != last_updated.time()) {
            return false;
        }

        const auto &keys = storage.key_storage().value;
        if (range == ItemRange::added) {
            return keys.added().contains(id);
        }
        if (range == ItemRange::removed) {
            return keys.removed().contains(id);
        }
        return storage.modified().contains(id) && !keys.added().contains(id);
    }

    item_id_t find_changed(const TimeSeriesState &last_updated, const void *value, const ScalarData &key,
                           ItemRange range) const {
        if (value == nullptr) {
            return util::occupancy_index::npos;
        }

        const auto id = find_key(value, key);
        return id != util::occupancy_index::npos && item_in_range(last_updated, value, range, id)
                   ? id
                   : util::occupancy_index::npos;
    }

    const ScalarRuntimeType *key_;
    const TimeSeriesRuntimeType *keys_;
    const TimeSeriesRuntimeType *item_;
    util::runtime_item_layout key_layout_;
    util::runtime_item_layout item_layout_;
    std::size_t item_value_offset_;
};

const TimeSeriesRuntimeType &time_series_dictionary_runtime_type(const TimeSeriesDictionaryType &type,
                                                                 const ScalarRuntimeType &key,
                                                                 const TimeSeriesRuntimeType &item);

template <typename K, typename Item, auto... Properties> struct CppTimeSeriesRuntimeType<TSD<K, Item, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        const auto &dictionary = static_cast<const TimeSeriesDictionaryType &>(type);
        return time_series_dictionary_runtime_type(dictionary, cpp_scalar_runtime_type<K>(),
                                                   ::bg::cpp_time_series_runtime_type<Item>(dictionary.value_type()));
    }
};

} // namespace bg::details

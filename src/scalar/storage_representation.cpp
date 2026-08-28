#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/dict/storage_representation.hpp>
#include <bg/scalar/list/storage_representation.hpp>
#include <bg/scalar/set/storage_representation.hpp>
#include <bg/scalar/view.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/polymorphic_value.hpp>
#include <bg/util/runtime_bunch.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg {
namespace {

class ComposedStorageRepresentation {
  protected:
    explicit ComposedStorageRepresentation(const ScalarType &type) noexcept : type_{&type} {}

  public:
    [[nodiscard]] const ScalarType &type() const noexcept { return *type_; }
    [[nodiscard]] bool uses_variable_memory() const noexcept { return true; }
    [[nodiscard]] bool requires_destruction() const noexcept { return true; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept { return true; }

    void construct_from_value(void *, const void *, std::pmr::memory_resource &) const { std::terminate(); }
    void assign_from_value(void *, const void *) const { std::terminate(); }

  private:
    const ScalarType *type_;
};

struct RuntimeListStorage {
    util::runtime_contiguous_bunch items;

    void swap(RuntimeListStorage &other) noexcept { items.swap(other.items); }
};

class ComposedListStorageRepresentation final : public ScalarListStorageRepresentation,
                                                private ComposedStorageRepresentation {
  public:
    ComposedListStorageRepresentation(const ListScalarType &type, const ScalarStorageRepresentation &item) noexcept
        : ComposedStorageRepresentation{type}, item_{&item}, item_layout_{item.size(), item.alignment()} {}

    [[nodiscard]] const ScalarType &type() const noexcept override { return ComposedStorageRepresentation::type(); }
    [[nodiscard]] std::size_t size() const noexcept override { return sizeof(RuntimeListStorage); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return alignof(RuntimeListStorage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override {
        return ComposedStorageRepresentation::uses_variable_memory();
    }
    [[nodiscard]] bool requires_destruction() const noexcept override {
        return ComposedStorageRepresentation::requires_destruction();
    }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override {
        return ComposedStorageRepresentation::supports_byte_copy_initialization();
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<RuntimeListStorage *>(destination));
    }

    void construct_from_value(void *destination, const void *value, std::pmr::memory_resource &memory) const override {
        ComposedStorageRepresentation::construct_from_value(destination, value, memory);
    }

    void construct_from_input(void *destination, const ScalarValueInputView &value,
                              std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        auto &source = static_cast<const ScalarListInputView &>(value);
        try {
            for (auto index = std::size_t{}; index < source.size(); ++index) {
                const auto item = source[index];
                append(destination, item.prototype, memory, item.data);
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        const auto &items = static_cast<const RuntimeListStorage *>(source)->items;
        try {
            for (auto index = std::uint32_t{}; index < items.size(); ++index) {
                storage(destination).items.emplace_back_with(item_layout_, memory, [&](void *item) {
                    item_->copy_construct(item, items.get(index, item_layout_), memory);
                });
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    void assign_from_value(void *destination, const void *value) const override {
        ComposedStorageRepresentation::assign_from_value(destination, value);
    }

    void assign_from_input(void *destination, const ScalarValueInputView &value) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto replacement = RuntimeListStorage{};
        const auto &source = static_cast<const ScalarListInputView &>(value);
        try {
            for (auto index = std::size_t{}; index < source.size(); ++index) {
                const auto item = source[index];
                append(&replacement, item.prototype, memory, item.data);
            }
        } catch (...) {
            clear(&replacement, memory);
            throw;
        }

        storage(destination).swap(replacement);
        clear(&replacement, memory);
    }

    [[nodiscard]] std::size_t hash(const void *value) const noexcept override {
        constexpr auto combine_constant = std::size_t{0x9e3779b9};
        const auto &items = storage(value).items;
        auto result = std::size_t{};
        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            result ^= item_->hash(items.get(index, item_layout_)) + combine_constant + (result << 6) + (result >> 2);
        }
        return result;
    }

    [[nodiscard]] bool equals(const void *value, const ScalarValueInputView &other) const noexcept override {
        const auto &source = static_cast<const ScalarListInputView &>(other);
        const auto &items = storage(value).items;
        if (items.size() != source.size()) {
            return false;
        }

        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            const auto source_item = source[index];
            auto bound = util::polymorphic_value<ScalarValueInputView>::copy(source_item.prototype);
            bound->bind(source_item.data);
            if (!item_->equals(items.get(index, item_layout_), bound.get())) {
                return false;
            }
        }
        return true;
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeListStorage *>(value));
    }

    [[nodiscard]] const ScalarStorageRepresentation &item_storage_representation() const noexcept override {
        return *item_;
    }

    [[nodiscard]] std::size_t item_count(const void *value) const noexcept override {
        return storage(value).items.size();
    }

    [[nodiscard]] const void *item(const void *value, std::size_t index) const noexcept override {
        return storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_);
    }

    void append(void *value, const ScalarValueInputView &item, std::pmr::memory_resource &memory) const override {
        append(value, item, memory, item.data());
    }

    void set(void *value, std::size_t index, const ScalarValueInputView &item) const override {
        item.assign(storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_), *item_);
    }

    void erase(void *value, std::size_t index, std::pmr::memory_resource &memory) const override {
        auto &items = storage(value).items;
        const auto position = static_cast<std::uint32_t>(index);
        auto replacement = RuntimeListStorage{};
        try {
            for (auto current = std::uint32_t{}; current < items.size(); ++current) {
                if (current == position) {
                    continue;
                }

                replacement.items.emplace_back_with(item_layout_, memory, [&](void *target) {
                    item_->copy_construct(target, items.get(current, item_layout_), memory);
                });
            }
        } catch (...) {
            clear(&replacement, memory);
            throw;
        }

        storage(value).swap(replacement);
        clear(&replacement, memory);
    }

    void clear(void *value, std::pmr::memory_resource &memory) const noexcept override {
        storage(value).items.release_with(item_layout_, memory,
                                          [&](void *item) noexcept { item_->destroy(item, memory); });
    }

  private:
    [[nodiscard]] static RuntimeListStorage &storage(void *value) noexcept {
        return *static_cast<RuntimeListStorage *>(value);
    }
    [[nodiscard]] static const RuntimeListStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeListStorage *>(value);
    }

    void append(void *value, const ScalarValueInputView &prototype, std::pmr::memory_resource &memory,
                const void *data) const {
        auto item = util::polymorphic_value<ScalarValueInputView>::copy(prototype);
        item->bind(data);
        storage(value).items.emplace_back_with(item_layout_, memory,
                                               [&](void *target) { item->copy_construct(target, *item_, memory); });
    }

    const ScalarStorageRepresentation *item_;
    util::runtime_item_layout item_layout_;
};

struct RuntimeSetStorage {
    util::occupancy_index positions;
    util::runtime_sparse_bunch items;
    util::linear_hash_table lookup;

    void swap(RuntimeSetStorage &other) noexcept {
        positions.swap(other.positions);
        items.swap(other.items);
        lookup.swap(other.lookup);
    }
};

class ComposedSetStorageRepresentation final : public ScalarSetStorageRepresentation,
                                               private ComposedStorageRepresentation {
  public:
    ComposedSetStorageRepresentation(const SetScalarType &type, const ScalarStorageRepresentation &item) noexcept
        : ComposedStorageRepresentation{type}, item_{&item}, item_layout_{item.size(), item.alignment()} {}

    [[nodiscard]] const ScalarType &type() const noexcept override { return ComposedStorageRepresentation::type(); }
    [[nodiscard]] std::size_t size() const noexcept override { return sizeof(RuntimeSetStorage); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return alignof(RuntimeSetStorage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return true; }
    [[nodiscard]] bool requires_destruction() const noexcept override { return true; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<RuntimeSetStorage *>(destination));
    }

    void construct_from_value(void *destination, const void *value, std::pmr::memory_resource &memory) const override {
        ComposedStorageRepresentation::construct_from_value(destination, value, memory);
    }

    void construct_from_input(void *destination, const ScalarValueInputView &value,
                              std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        const auto &source = static_cast<const ScalarSetInputView &>(value);
        auto copy = RuntimeSetCopy{this, destination, &memory};
        try {
            source.for_each(
                &copy, +[](void *context, ScalarSetInputView::Item item) {
                    auto &current = *static_cast<RuntimeSetCopy *>(context);
                    auto bound = util::polymorphic_value<ScalarValueInputView>::copy(item.prototype);
                    bound->bind(item.data);
                    (void)current.representation->insert(current.storage, bound.get(), *current.memory);
                });
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        try {
            for (const auto position : storage(source).positions) {
                insert_stored(destination, storage(source).items.get(position, item_layout_), memory);
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    void assign_from_value(void *destination, const void *value) const override {
        ComposedStorageRepresentation::assign_from_value(destination, value);
    }

    void assign_from_input(void *destination, const ScalarValueInputView &value) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto replacement = RuntimeSetStorage{};
        auto copy = RuntimeSetCopy{this, &replacement, &memory};
        try {
            static_cast<const ScalarSetInputView &>(value).for_each(
                &copy, +[](void *context, ScalarSetInputView::Item item) {
                    auto &current = *static_cast<RuntimeSetCopy *>(context);
                    auto bound = util::polymorphic_value<ScalarValueInputView>::copy(item.prototype);
                    bound->bind(item.data);
                    (void)current.representation->insert(current.storage, bound.get(), *current.memory);
                });
        } catch (...) {
            clear(&replacement, memory);
            throw;
        }

        storage(destination).swap(replacement);
        clear(&replacement, memory);
    }

    [[nodiscard]] std::size_t hash(const void *value) const noexcept override {
        auto result = std::size_t{};
        for (const auto position : storage(value).positions) {
            result ^= item_->hash(storage(value).items.get(position, item_layout_));
        }
        return result;
    }

    [[nodiscard]] bool equals(const void *value, const ScalarValueInputView &other) const noexcept override {
        const auto &source = static_cast<const ScalarSetInputView &>(other);
        if (item_count(value) != source.size()) {
            return false;
        }

        auto comparison = RuntimeSetComparison{this, value, true};
        source.for_each(
            &comparison, +[](void *context, ScalarSetInputView::Item item) {
                auto &current = *static_cast<RuntimeSetComparison *>(context);
                auto bound = util::polymorphic_value<ScalarValueInputView>::copy(item.prototype);
                bound->bind(item.data);
                current.equal &= current.representation->contains(current.storage, bound.get());
            });
        return comparison.equal;
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeSetStorage *>(value));
    }

    [[nodiscard]] const ScalarStorageRepresentation &item_storage_representation() const noexcept override {
        return *item_;
    }

    [[nodiscard]] std::size_t item_count(const void *value) const noexcept override {
        return storage(value).positions.size();
    }

    [[nodiscard]] bool contains(const void *value, const ScalarValueInputView &item) const override {
        return find(storage(value), item) != util::occupancy_index::npos;
    }

    [[nodiscard]] bool insert(void *value, const ScalarValueInputView &item,
                              std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        if (target.lookup.find(
                hash, [&](auto position) { return item_->equals(target.items.get(position, item_layout_), item); })) {
            return false;
        }

        const auto position = target.positions.acquire(memory);
        auto item_constructed = false;
        try {
            target.items.emplace_at_with(position, item_layout_, memory,
                                         [&](void *stored) { item.copy_construct(stored, *item_, memory); });
            item_constructed = true;
            target.lookup.insert(hash, position, memory);
        } catch (...) {
            if (item_constructed) {
                target.items.erase_at_with(position, item_layout_, memory,
                                           [&](void *stored) noexcept { item_->destroy(stored, memory); });
            }
            (void)target.positions.reset(position);
            target.positions.trim(memory);
            throw;
        }
        return true;
    }

    [[nodiscard]] bool erase(void *value, const ScalarValueInputView &item,
                             std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        const auto position = target.lookup.find(
            hash, [&](auto candidate) { return item_->equals(target.items.get(candidate, item_layout_), item); });
        if (!position) {
            return false;
        }

        (void)target.lookup.erase(hash, *position, memory);
        target.items.erase_at_with(*position, item_layout_, memory,
                                   [&](void *stored) noexcept { item_->destroy(stored, memory); });
        (void)target.positions.reset(*position);
        target.positions.trim(memory);
        return true;
    }

    void clear(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto &target = storage(value);
        target.lookup.release(memory);
        target.items.release_with(target.positions, item_layout_, memory,
                                  [&](void *item) noexcept { item_->destroy(item, memory); });
        target.positions.release(memory);
    }

  private:
    struct RuntimeSetCopy {
        const ComposedSetStorageRepresentation *representation;
        void *storage;
        std::pmr::memory_resource *memory;
    };

    struct RuntimeSetComparison {
        const ComposedSetStorageRepresentation *representation;
        const void *storage;
        bool equal;
    };

    [[nodiscard]] static RuntimeSetStorage &storage(void *value) noexcept {
        return *static_cast<RuntimeSetStorage *>(value);
    }
    [[nodiscard]] static const RuntimeSetStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeSetStorage *>(value);
    }

    [[nodiscard]] std::uint32_t find(const RuntimeSetStorage &source, const ScalarValueInputView &item) const noexcept {
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        const auto result = source.lookup.find(
            hash, [&](auto position) { return item_->equals(source.items.get(position, item_layout_), item); });
        return result.value_or(util::occupancy_index::npos);
    }

    void insert_stored(void *value, const void *item, std::pmr::memory_resource &memory) const {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(item_->hash(item));
        const auto position = target.positions.acquire(memory);
        auto item_constructed = false;
        try {
            target.items.emplace_at_with(position, item_layout_, memory,
                                         [&](void *stored) { item_->copy_construct(stored, item, memory); });
            item_constructed = true;
            target.lookup.insert(hash, position, memory);
        } catch (...) {
            if (item_constructed) {
                target.items.erase_at_with(position, item_layout_, memory,
                                           [&](void *stored) noexcept { item_->destroy(stored, memory); });
            }
            (void)target.positions.reset(position);
            target.positions.trim(memory);
            throw;
        }
    }

    const ScalarStorageRepresentation *item_;
    util::runtime_item_layout item_layout_;
};

struct RuntimeDictStorage {
    util::occupancy_index positions;
    util::runtime_sparse_bunch keys;
    util::runtime_sparse_bunch values;
    util::linear_hash_table lookup;

    void swap(RuntimeDictStorage &other) noexcept {
        positions.swap(other.positions);
        keys.swap(other.keys);
        values.swap(other.values);
        lookup.swap(other.lookup);
    }
};

class ComposedDictStorageRepresentation final : public ScalarDictStorageRepresentation,
                                                private ComposedStorageRepresentation {
  public:
    ComposedDictStorageRepresentation(const DictScalarType &type, const ScalarStorageRepresentation &key,
                                      const ScalarStorageRepresentation &mapped) noexcept
        : ComposedStorageRepresentation{type}, key_{&key}, mapped_{&mapped}, key_layout_{key.size(), key.alignment()},
          mapped_layout_{mapped.size(), mapped.alignment()} {}

    [[nodiscard]] const ScalarType &type() const noexcept override { return ComposedStorageRepresentation::type(); }
    [[nodiscard]] std::size_t size() const noexcept override { return sizeof(RuntimeDictStorage); }
    [[nodiscard]] std::size_t alignment() const noexcept override { return alignof(RuntimeDictStorage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return true; }
    [[nodiscard]] bool requires_destruction() const noexcept override { return true; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<RuntimeDictStorage *>(destination));
    }

    void construct_from_value(void *destination, const void *value, std::pmr::memory_resource &memory) const override {
        ComposedStorageRepresentation::construct_from_value(destination, value, memory);
    }

    void construct_from_input(void *destination, const ScalarValueInputView &value,
                              std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        auto copy = RuntimeDictCopy{this, destination, &memory};
        try {
            static_cast<const ScalarDictInputView &>(value).for_each(
                &copy, +[](void *context, ScalarDictInputView::Entry entry) {
                    auto &current = *static_cast<RuntimeDictCopy *>(context);
                    auto key = util::polymorphic_value<ScalarValueInputView>::copy(entry.key.prototype);
                    auto mapped = util::polymorphic_value<ScalarValueInputView>::copy(entry.value.prototype);
                    key->bind(entry.key.data);
                    mapped->bind(entry.value.data);
                    current.representation->insert_or_assign(current.storage, key.get(), mapped.get(), *current.memory);
                });
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        try {
            for (const auto position : storage(source).positions) {
                insert_stored(destination, storage(source).keys.get(position, key_layout_),
                              storage(source).values.get(position, mapped_layout_), memory);
            }
        } catch (...) {
            destroy(destination, memory);
            throw;
        }
    }

    void assign_from_value(void *destination, const void *value) const override {
        ComposedStorageRepresentation::assign_from_value(destination, value);
    }

    void assign_from_input(void *destination, const ScalarValueInputView &value) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto replacement = RuntimeDictStorage{};
        auto copy = RuntimeDictCopy{this, &replacement, &memory};
        try {
            static_cast<const ScalarDictInputView &>(value).for_each(
                &copy, +[](void *context, ScalarDictInputView::Entry entry) {
                    auto &current = *static_cast<RuntimeDictCopy *>(context);
                    auto key = util::polymorphic_value<ScalarValueInputView>::copy(entry.key.prototype);
                    auto mapped = util::polymorphic_value<ScalarValueInputView>::copy(entry.value.prototype);
                    key->bind(entry.key.data);
                    mapped->bind(entry.value.data);
                    current.representation->insert_or_assign(current.storage, key.get(), mapped.get(), *current.memory);
                });
        } catch (...) {
            clear(&replacement, memory);
            throw;
        }

        storage(destination).swap(replacement);
        clear(&replacement, memory);
    }

    [[nodiscard]] std::size_t hash(const void *value) const noexcept override {
        auto result = std::size_t{};
        for (const auto position : storage(value).positions) {
            result ^= key_->hash(storage(value).keys.get(position, key_layout_)) ^
                      (mapped_->hash(storage(value).values.get(position, mapped_layout_)) << 1);
        }
        return result;
    }

    [[nodiscard]] bool equals(const void *value, const ScalarValueInputView &other) const noexcept override {
        const auto &source = static_cast<const ScalarDictInputView &>(other);
        if (item_count(value) != source.size()) {
            return false;
        }

        auto comparison = RuntimeDictComparison{this, value, true};
        source.for_each(
            &comparison, +[](void *context, ScalarDictInputView::Entry entry) {
                auto &current = *static_cast<RuntimeDictComparison *>(context);
                auto key = util::polymorphic_value<ScalarValueInputView>::copy(entry.key.prototype);
                auto mapped = util::polymorphic_value<ScalarValueInputView>::copy(entry.value.prototype);
                key->bind(entry.key.data);
                mapped->bind(entry.value.data);
                const auto position = current.representation->find(storage(current.storage), key.get());
                current.equal &=
                    position != util::occupancy_index::npos &&
                    current.representation->mapped_->equals(
                        storage(current.storage).values.get(position, current.representation->mapped_layout_),
                        mapped.get());
            });
        return comparison.equal;
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeDictStorage *>(value));
    }

    [[nodiscard]] const ScalarStorageRepresentation &key_storage_representation() const noexcept override {
        return *key_;
    }

    [[nodiscard]] const ScalarStorageRepresentation &mapped_storage_representation() const noexcept override {
        return *mapped_;
    }

    [[nodiscard]] std::size_t item_count(const void *value) const noexcept override {
        return storage(value).positions.size();
    }

    [[nodiscard]] bool contains(const void *value, const ScalarValueInputView &key) const override {
        return find(storage(value), key) != util::occupancy_index::npos;
    }

    [[nodiscard]] ScalarDictStorageRepresentation::Item at(void *value,
                                                           const ScalarValueInputView &key) const override {
        const auto position = find(storage(value), key);
        if (position == util::occupancy_index::npos) {
            throw std::out_of_range{"dictionary key was not found"};
        }
        return {*mapped_, storage(value).values.get(position, mapped_layout_)};
    }

    void insert_or_assign(void *value, const ScalarValueInputView &key, const ScalarValueInputView &mapped,
                          std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        if (const auto position = target.lookup.find(
                hash, [&](auto candidate) { return key_->equals(target.keys.get(candidate, key_layout_), key); })) {
            mapped.assign(target.values.get(*position, mapped_layout_), *mapped_);
            return;
        }

        const auto position = target.positions.acquire(memory);
        auto key_constructed = false;
        auto value_constructed = false;
        try {
            target.keys.emplace_at_with(position, key_layout_, memory,
                                        [&](void *stored) { key.copy_construct(stored, *key_, memory); });
            key_constructed = true;
            target.values.emplace_at_with(position, mapped_layout_, memory,
                                          [&](void *stored) { mapped.copy_construct(stored, *mapped_, memory); });
            value_constructed = true;
            target.lookup.insert(hash, position, memory);
        } catch (...) {
            if (value_constructed) {
                target.values.erase_at_with(position, mapped_layout_, memory,
                                            [&](void *stored) noexcept { mapped_->destroy(stored, memory); });
            }
            if (key_constructed) {
                target.keys.erase_at_with(position, key_layout_, memory,
                                          [&](void *stored) noexcept { key_->destroy(stored, memory); });
            }
            (void)target.positions.reset(position);
            target.positions.trim(memory);
            throw;
        }
    }

    [[nodiscard]] bool erase(void *value, const ScalarValueInputView &key,
                             std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto position = target.lookup.find(
            hash, [&](auto candidate) { return key_->equals(target.keys.get(candidate, key_layout_), key); });
        if (!position) {
            return false;
        }

        (void)target.lookup.erase(hash, *position, memory);
        target.values.erase_at_with(*position, mapped_layout_, memory,
                                    [&](void *stored) noexcept { mapped_->destroy(stored, memory); });
        target.keys.erase_at_with(*position, key_layout_, memory,
                                  [&](void *stored) noexcept { key_->destroy(stored, memory); });
        (void)target.positions.reset(*position);
        target.positions.trim(memory);
        return true;
    }

    void clear(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto &target = storage(value);
        target.lookup.release(memory);
        target.values.release_with(target.positions, mapped_layout_, memory,
                                   [&](void *item) noexcept { mapped_->destroy(item, memory); });
        target.keys.release_with(target.positions, key_layout_, memory,
                                 [&](void *item) noexcept { key_->destroy(item, memory); });
        target.positions.release(memory);
    }

  private:
    struct RuntimeDictCopy {
        const ComposedDictStorageRepresentation *representation;
        void *storage;
        std::pmr::memory_resource *memory;
    };

    struct RuntimeDictComparison {
        const ComposedDictStorageRepresentation *representation;
        const void *storage;
        bool equal;
    };

    [[nodiscard]] static RuntimeDictStorage &storage(void *value) noexcept {
        return *static_cast<RuntimeDictStorage *>(value);
    }
    [[nodiscard]] static const RuntimeDictStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeDictStorage *>(value);
    }

    [[nodiscard]] std::uint32_t find(const RuntimeDictStorage &source, const ScalarValueInputView &key) const noexcept {
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto result = source.lookup.find(
            hash, [&](auto position) { return key_->equals(source.keys.get(position, key_layout_), key); });
        return result.value_or(util::occupancy_index::npos);
    }

    void insert_stored(void *value, const void *key, const void *mapped, std::pmr::memory_resource &memory) const {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(key_->hash(key));
        const auto position = target.positions.acquire(memory);
        auto key_constructed = false;
        auto value_constructed = false;
        try {
            target.keys.emplace_at_with(position, key_layout_, memory,
                                        [&](void *stored) { key_->copy_construct(stored, key, memory); });
            key_constructed = true;
            target.values.emplace_at_with(position, mapped_layout_, memory,
                                          [&](void *stored) { mapped_->copy_construct(stored, mapped, memory); });
            value_constructed = true;
            target.lookup.insert(hash, position, memory);
        } catch (...) {
            if (value_constructed) {
                target.values.erase_at_with(position, mapped_layout_, memory,
                                            [&](void *stored) noexcept { mapped_->destroy(stored, memory); });
            }
            if (key_constructed) {
                target.keys.erase_at_with(position, key_layout_, memory,
                                          [&](void *stored) noexcept { key_->destroy(stored, memory); });
            }
            (void)target.positions.reset(position);
            target.positions.trim(memory);
            throw;
        }
    }

    const ScalarStorageRepresentation *key_;
    const ScalarStorageRepresentation *mapped_;
    util::runtime_item_layout key_layout_;
    util::runtime_item_layout mapped_layout_;
};

struct StorageRepresentations {
    std::mutex mutex;
    std::vector<const ScalarStorageRepresentation *> native;
    std::vector<std::unique_ptr<ScalarStorageRepresentation>> composed;
};

[[nodiscard]] StorageRepresentations &storage_representations() {
    static auto value = StorageRepresentations{};
    return value;
}

[[nodiscard]] const ScalarStorageRepresentation *registered_representation(const ScalarType &type, bool native) {
    auto &representations = storage_representations();
    const auto lock = std::lock_guard{representations.mutex};
    if (native) {
        const auto found =
            std::ranges::find_if(representations.native, [&](const auto *item) { return &item->type() == &type; });
        return found == representations.native.end() ? nullptr : *found;
    }

    const auto found =
        std::ranges::find_if(representations.composed, [&](const auto &item) { return &item->type() == &type; });
    return found == representations.composed.end() ? nullptr : found->get();
}

} // namespace

void register_scalar_storage_representation(const ScalarStorageRepresentation &representation) noexcept {
    auto &representations = storage_representations();
    const auto lock = std::lock_guard{representations.mutex};
    if (std::ranges::find(representations.native, &representation) == representations.native.end()) {
        representations.native.push_back(&representation);
    }
}

const ScalarStorageRepresentation &
resolve_scalar_storage_representation(const ScalarType &type,
                                      std::span<const ScalarStorageRepresentation *const> available) {
    const auto supplied =
        std::ranges::find_if(available, [&](const auto *item) { return item != nullptr && &item->type() == &type; });
    if (supplied != available.end()) {
        return **supplied;
    }
    if (const auto *native = registered_representation(type, true)) {
        return *native;
    }
    if (const auto *composed = registered_representation(type, false)) {
        return *composed;
    }

    auto representation = std::unique_ptr<ScalarStorageRepresentation>{};
    if (const auto *list = dynamic_cast<const ListScalarType *>(&type)) {
        auto &item = resolve_scalar_storage_representation(list->item_type(), available);
        representation = std::make_unique<ComposedListStorageRepresentation>(*list, item);
    } else if (const auto *set = dynamic_cast<const SetScalarType *>(&type)) {
        auto &item = resolve_scalar_storage_representation(set->item_type(), available);
        representation = std::make_unique<ComposedSetStorageRepresentation>(*set, item);
    } else if (const auto *dict = dynamic_cast<const DictScalarType *>(&type)) {
        auto &key = resolve_scalar_storage_representation(dict->key_type(), available);
        auto &mapped = resolve_scalar_storage_representation(dict->mapped_type(), available);
        representation = std::make_unique<ComposedDictStorageRepresentation>(*dict, key, mapped);
    } else {
        throw std::logic_error{"resolved scalar type has no storage representation"};
    }

    auto &representations = storage_representations();
    const auto lock = std::lock_guard{representations.mutex};
    const auto existing =
        std::ranges::find_if(representations.composed, [&](const auto &item) { return &item->type() == &type; });
    if (existing != representations.composed.end()) {
        return **existing;
    }

    auto &result = *representation;
    representations.composed.push_back(std::move(representation));
    return result;
}

} // namespace bg

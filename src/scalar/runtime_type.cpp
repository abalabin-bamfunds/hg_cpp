#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/runtime_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/linear_hash_table.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/runtime_bunch.hpp>
#include <bg/util/scope_exit.hpp>

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

ScalarRuntimeType::~ScalarRuntimeType() = default;

namespace {

struct RuntimeListStorage {
    util::runtime_contiguous_bunch items;

    void swap(RuntimeListStorage &other) noexcept { items.swap(other.items); }
};

static_assert(std::is_trivially_copyable_v<RuntimeListStorage>);

class ComposedListRuntimeType final : public ScalarListRuntimeType {
  public:
    ComposedListRuntimeType(const ListScalarType &type, const ScalarRuntimeType &item) noexcept
        : ScalarListRuntimeType{type, sizeof(RuntimeListStorage),   alignof(RuntimeListStorage),
                                true, item.requires_finalization(), true,
                                item},
          item_layout_{item.size(), item.alignment()} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<RuntimeListStorage *>(destination));
    }

    void construct_from_value(void *, const void *, std::pmr::memory_resource &) const override { std::terminate(); }

    void construct_from_input(void *destination, const details::ScalarData &value,
                              std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        const auto source = Scalar<List<SCALAR>>::from(value);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (auto index = std::size_t{}; index < source.size(); ++index) {
            const auto item = source[index];
            append(destination, item.input_view(), memory);
        }
        rollback.release();
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        const auto &items = static_cast<const RuntimeListStorage *>(source)->items;
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            storage(destination).items.emplace_back_with(item_layout_, memory, [&](void *item) {
                item_runtime_type().copy_construct(item, items.get(index, item_layout_), memory);
            });
        }
        rollback.release();
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }

    void assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto replacement = RuntimeListStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        const auto source = Scalar<List<SCALAR>>::from(value);
        for (auto index = std::size_t{}; index < source.size(); ++index) {
            const auto item = source[index];
            append(&replacement, item.input_view(), memory);
        }

        storage(destination).swap(replacement);
    }

    std::size_t hash(const void *value) const noexcept override {
        constexpr auto combine_constant = std::size_t{0x9e3779b9};
        const auto &items = storage(value).items;
        auto result = std::size_t{};
        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            result ^= item_runtime_type().hash(items.get(index, item_layout_)) + combine_constant + (result << 6) +
                      (result >> 2);
        }
        return result;
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const noexcept override {
        const auto source = Scalar<List<SCALAR>>::from(other);
        const auto &items = storage(value).items;
        if (items.size() != source.size()) {
            return false;
        }

        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            const auto source_item = source[index];
            if (!item_runtime_type().equals(items.get(index, item_layout_), source_item.input_view())) {
                return false;
            }
        }
        return true;
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeListStorage *>(value));
    }

    std::size_t item_count(const void *value) const noexcept override {
        return storage(value).items.size();
    }

    const void *item(const void *value, std::size_t index) const noexcept override {
        return storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_);
    }

    void *item(void *value, std::size_t index) const noexcept override {
        return storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_);
    }

    void append(void *value, const details::ScalarData &item, std::pmr::memory_resource &memory) const override {
        append(value, item.runtime_type(), memory, item.data());
    }

    void set(void *value, std::size_t index, const details::ScalarData &item) const override {
        item.assign(storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_), item_runtime_type());
    }

    void erase(void *value, std::size_t index, std::pmr::memory_resource &memory) const override {
        auto &items = storage(value).items;
        const auto position = static_cast<std::uint32_t>(index);
        auto replacement = RuntimeListStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        for (auto current = std::uint32_t{}; current < items.size(); ++current) {
            if (current == position) {
                continue;
            }

            replacement.items.emplace_back_with(item_layout_, memory, [&](void *target) {
                item_runtime_type().copy_construct(target, items.get(current, item_layout_), memory);
            });
        }

        storage(value).swap(replacement);
    }

    void clear(void *value, std::pmr::memory_resource &memory) const noexcept override {
        storage(value).items.release_with(item_layout_, memory,
                                          [&](void *item) noexcept { item_runtime_type().destroy(item, memory); });
    }

  private:
    static RuntimeListStorage &storage(void *value) noexcept {
        return *static_cast<RuntimeListStorage *>(value);
    }
    static const RuntimeListStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeListStorage *>(value);
    }

    void append(void *value, const ScalarRuntimeType &runtime_type, std::pmr::memory_resource &memory,
                const void *data) const {
        const auto item = details::ScalarData{runtime_type, data};
        storage(value).items.emplace_back_with(
            item_layout_, memory, [&](void *target) { item.copy_construct(target, item_runtime_type(), memory); });
    }

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

static_assert(std::is_trivially_copyable_v<RuntimeSetStorage>);

class ComposedSetRuntimeType final : public ScalarSetRuntimeType {
  public:
    ComposedSetRuntimeType(const SetScalarType &type, const ScalarRuntimeType &item) noexcept
        : ScalarSetRuntimeType{type, sizeof(RuntimeSetStorage),    alignof(RuntimeSetStorage),
                               true, item.requires_finalization(), true,
                               item},
          item_layout_{item.size(), item.alignment()} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<RuntimeSetStorage *>(destination));
    }

    void construct_from_value(void *, const void *, std::pmr::memory_resource &) const override { std::terminate(); }

    void construct_from_input(void *destination, const details::ScalarData &value,
                              std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        const auto source = Scalar<Set<SCALAR>>::from(value);
        auto copy = RuntimeSetCopy{this, destination, &memory};
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        source.for_each([&](const Scalar<SCALAR> &item) {
            (void)copy.runtime_type->insert(copy.storage, item.input_view(), *copy.memory);
        });
        rollback.release();
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto position : storage(source).positions) {
            insert_stored(destination, storage(source).items.get(position, item_layout_), memory);
        }
        rollback.release();
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }

    void assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto replacement = RuntimeSetStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        auto copy = RuntimeSetCopy{this, &replacement, &memory};
        Scalar<Set<SCALAR>>::from(value).for_each([&](const Scalar<SCALAR> &item) {
            (void)copy.runtime_type->insert(copy.storage, item.input_view(), *copy.memory);
        });

        storage(destination).swap(replacement);
    }

    std::size_t hash(const void *value) const noexcept override {
        auto result = std::size_t{};
        for (const auto position : storage(value).positions) {
            result ^= item_runtime_type().hash(storage(value).items.get(position, item_layout_));
        }
        return result;
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const noexcept override {
        const auto source = Scalar<Set<SCALAR>>::from(other);
        if (item_count(value) != source.size()) {
            return false;
        }

        auto comparison = RuntimeSetComparison{this, value, true};
        source.for_each([&](const Scalar<SCALAR> &item) {
            comparison.equal &= comparison.runtime_type->contains(comparison.storage, item.input_view());
        });
        return comparison.equal;
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeSetStorage *>(value));
    }

    std::size_t item_count(const void *value) const noexcept override {
        return storage(value).positions.size();
    }

    bool contains(const void *value, const details::ScalarData &item) const override {
        return find(storage(value), item) != util::occupancy_index::npos;
    }

    void for_each(const void *value, void *context, ScalarSetRuntimeType::ItemFunction function) const override {
        for (const auto position : storage(value).positions) {
            function(context, {item_runtime_type(), storage(value).items.get(position, item_layout_)});
        }
    }

    [[nodiscard]] bool insert(void *value, const details::ScalarData &item,
                              std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        if (target.lookup.find(hash, [&](auto position) {
                return item_runtime_type().equals(target.items.get(position, item_layout_), item);
            })) {
            return false;
        }

        const auto position = target.positions.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept {
            (void)target.positions.reset(position);
            target.positions.trim(memory);
        }};
        target.items.emplace_at_with(position, item_layout_, memory,
                                     [&](void *stored) { item.copy_construct(stored, item_runtime_type(), memory); });
        auto release_item = util::scope_exit{[&]() noexcept {
            target.items.erase_at_with(position, item_layout_, memory,
                                       [&](void *stored) noexcept { item_runtime_type().destroy(stored, memory); });
        }};
        target.lookup.insert(hash, position, memory);
        release_item.release();
        release_position.release();
        return true;
    }

    [[nodiscard]] bool erase(void *value, const details::ScalarData &item,
                             std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        const auto position = target.lookup.find(hash, [&](auto candidate) {
            return item_runtime_type().equals(target.items.get(candidate, item_layout_), item);
        });
        if (!position) {
            return false;
        }

        (void)target.lookup.erase(hash, *position, memory);
        target.items.erase_at_with(*position, item_layout_, memory,
                                   [&](void *stored) noexcept { item_runtime_type().destroy(stored, memory); });
        (void)target.positions.reset(*position);
        target.positions.trim(memory);
        return true;
    }

    void clear(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto &target = storage(value);
        target.lookup.release(memory);
        target.items.release_with(target.positions, item_layout_, memory,
                                  [&](void *item) noexcept { item_runtime_type().destroy(item, memory); });
        target.positions.release(memory);
    }

  private:
    struct RuntimeSetCopy {
        const ComposedSetRuntimeType *runtime_type;
        void *storage;
        std::pmr::memory_resource *memory;
    };

    struct RuntimeSetComparison {
        const ComposedSetRuntimeType *runtime_type;
        const void *storage;
        bool equal;
    };

    static RuntimeSetStorage &storage(void *value) noexcept {
        return *static_cast<RuntimeSetStorage *>(value);
    }
    static const RuntimeSetStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeSetStorage *>(value);
    }

    std::uint32_t find(const RuntimeSetStorage &source, const details::ScalarData &item) const noexcept {
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        const auto result = source.lookup.find(hash, [&](auto position) {
            return item_runtime_type().equals(source.items.get(position, item_layout_), item);
        });
        return result.value_or(util::occupancy_index::npos);
    }

    void insert_stored(void *value, const void *item, std::pmr::memory_resource &memory) const {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(item_runtime_type().hash(item));
        const auto position = target.positions.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept {
            (void)target.positions.reset(position);
            target.positions.trim(memory);
        }};
        target.items.emplace_at_with(position, item_layout_, memory,
                                     [&](void *stored) { item_runtime_type().copy_construct(stored, item, memory); });
        auto release_item = util::scope_exit{[&]() noexcept {
            target.items.erase_at_with(position, item_layout_, memory,
                                       [&](void *stored) noexcept { item_runtime_type().destroy(stored, memory); });
        }};
        target.lookup.insert(hash, position, memory);
        release_item.release();
        release_position.release();
    }

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

static_assert(std::is_trivially_copyable_v<RuntimeDictStorage>);

class ComposedDictRuntimeType final : public ScalarDictRuntimeType {
  public:
    ComposedDictRuntimeType(const DictScalarType &type, const ScalarRuntimeType &key,
                            const ScalarRuntimeType &mapped) noexcept
        : ScalarDictRuntimeType{type,
                                sizeof(RuntimeDictStorage),
                                alignof(RuntimeDictStorage),
                                true,
                                key.requires_finalization() || mapped.requires_finalization(),
                                true,
                                key,
                                mapped},
          key_layout_{key.size(), key.alignment()}, mapped_layout_{mapped.size(), mapped.alignment()} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<RuntimeDictStorage *>(destination));
    }

    void construct_from_value(void *, const void *, std::pmr::memory_resource &) const override { std::terminate(); }

    void construct_from_input(void *destination, const details::ScalarData &value,
                              std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        auto copy = RuntimeDictCopy{this, destination, &memory};
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        Scalar<Dict<SCALAR, SCALAR>>::from(value).for_each(
            [&](const Scalar<SCALAR> &key, const Scalar<SCALAR> &mapped) {
                copy.runtime_type->insert_or_assign(copy.storage, key.input_view(), mapped.input_view(), *copy.memory);
            });
        rollback.release();
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto position : storage(source).positions) {
            insert_stored(destination, storage(source).keys.get(position, key_layout_),
                          storage(source).values.get(position, mapped_layout_), memory);
        }
        rollback.release();
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }

    void assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto &memory = details::current_scalar_memory_resource();
        auto replacement = RuntimeDictStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        auto copy = RuntimeDictCopy{this, &replacement, &memory};
        Scalar<Dict<SCALAR, SCALAR>>::from(value).for_each(
            [&](const Scalar<SCALAR> &key, const Scalar<SCALAR> &mapped) {
                copy.runtime_type->insert_or_assign(copy.storage, key.input_view(), mapped.input_view(), *copy.memory);
            });

        storage(destination).swap(replacement);
    }

    std::size_t hash(const void *value) const noexcept override {
        auto result = std::size_t{};
        for (const auto position : storage(value).positions) {
            result ^= key_runtime_type().hash(storage(value).keys.get(position, key_layout_)) ^
                      (mapped_runtime_type().hash(storage(value).values.get(position, mapped_layout_)) << 1);
        }
        return result;
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const noexcept override {
        const auto source = Scalar<Dict<SCALAR, SCALAR>>::from(other);
        if (item_count(value) != source.size()) {
            return false;
        }

        auto comparison = RuntimeDictComparison{this, value, true};
        source.for_each([&](const Scalar<SCALAR> &key, const Scalar<SCALAR> &mapped) {
            const auto position = comparison.runtime_type->find(storage(comparison.storage), key.input_view());
            comparison.equal &=
                position != util::occupancy_index::npos &&
                comparison.runtime_type->mapped_runtime_type().equals(
                    storage(comparison.storage).values.get(position, comparison.runtime_type->mapped_layout_),
                    mapped.input_view());
        });
        return comparison.equal;
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeDictStorage *>(value));
    }

    std::size_t item_count(const void *value) const noexcept override {
        return storage(value).positions.size();
    }

    bool contains(const void *value, const details::ScalarData &key) const override {
        return find(storage(value), key) != util::occupancy_index::npos;
    }

    ScalarDictRuntimeType::ConstItem at(const void *value,
                                                      const details::ScalarData &key) const override {
        const auto position = find(storage(value), key);
        if (position == util::occupancy_index::npos) {
            throw std::out_of_range{"dictionary key was not found"};
        }
        return {mapped_runtime_type(), storage(value).values.get(position, mapped_layout_)};
    }

    ScalarDictRuntimeType::Item at(void *value, const details::ScalarData &key) const override {
        const auto position = find(storage(value), key);
        if (position == util::occupancy_index::npos) {
            throw std::out_of_range{"dictionary key was not found"};
        }
        return {mapped_runtime_type(), storage(value).values.get(position, mapped_layout_)};
    }

    void for_each(const void *value, void *context, ScalarDictRuntimeType::EntryFunction function) const override {
        for (const auto position : storage(value).positions) {
            function(context, {{key_runtime_type(), storage(value).keys.get(position, key_layout_)},
                               {mapped_runtime_type(), storage(value).values.get(position, mapped_layout_)}});
        }
    }

    void insert_or_assign(void *value, const details::ScalarData &key, const details::ScalarData &mapped,
                          std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        if (const auto position = target.lookup.find(hash, [&](auto candidate) {
                return key_runtime_type().equals(target.keys.get(candidate, key_layout_), key);
            })) {
            mapped.assign(target.values.get(*position, mapped_layout_), mapped_runtime_type());
            return;
        }

        const auto position = target.positions.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept {
            (void)target.positions.reset(position);
            target.positions.trim(memory);
        }};
        target.keys.emplace_at_with(position, key_layout_, memory,
                                    [&](void *stored) { key.copy_construct(stored, key_runtime_type(), memory); });
        auto release_key = util::scope_exit{[&]() noexcept {
            target.keys.erase_at_with(position, key_layout_, memory,
                                      [&](void *stored) noexcept { key_runtime_type().destroy(stored, memory); });
        }};
        target.values.emplace_at_with(position, mapped_layout_, memory, [&](void *stored) {
            mapped.copy_construct(stored, mapped_runtime_type(), memory);
        });
        auto release_value = util::scope_exit{[&]() noexcept {
            target.values.erase_at_with(position, mapped_layout_, memory,
                                        [&](void *stored) noexcept { mapped_runtime_type().destroy(stored, memory); });
        }};
        target.lookup.insert(hash, position, memory);
        release_value.release();
        release_key.release();
        release_position.release();
    }

    [[nodiscard]] bool erase(void *value, const details::ScalarData &key,
                             std::pmr::memory_resource &memory) const override {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto position = target.lookup.find(hash, [&](auto candidate) {
            return key_runtime_type().equals(target.keys.get(candidate, key_layout_), key);
        });
        if (!position) {
            return false;
        }

        (void)target.lookup.erase(hash, *position, memory);
        target.values.erase_at_with(*position, mapped_layout_, memory,
                                    [&](void *stored) noexcept { mapped_runtime_type().destroy(stored, memory); });
        target.keys.erase_at_with(*position, key_layout_, memory,
                                  [&](void *stored) noexcept { key_runtime_type().destroy(stored, memory); });
        (void)target.positions.reset(*position);
        target.positions.trim(memory);
        return true;
    }

    void clear(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto &target = storage(value);
        target.lookup.release(memory);
        target.values.release_with(target.positions, mapped_layout_, memory,
                                   [&](void *item) noexcept { mapped_runtime_type().destroy(item, memory); });
        target.keys.release_with(target.positions, key_layout_, memory,
                                 [&](void *item) noexcept { key_runtime_type().destroy(item, memory); });
        target.positions.release(memory);
    }

  private:
    struct RuntimeDictCopy {
        const ComposedDictRuntimeType *runtime_type;
        void *storage;
        std::pmr::memory_resource *memory;
    };

    struct RuntimeDictComparison {
        const ComposedDictRuntimeType *runtime_type;
        const void *storage;
        bool equal;
    };

    static RuntimeDictStorage &storage(void *value) noexcept {
        return *static_cast<RuntimeDictStorage *>(value);
    }
    static const RuntimeDictStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeDictStorage *>(value);
    }

    std::uint32_t find(const RuntimeDictStorage &source, const details::ScalarData &key) const noexcept {
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto result = source.lookup.find(hash, [&](auto position) {
            return key_runtime_type().equals(source.keys.get(position, key_layout_), key);
        });
        return result.value_or(util::occupancy_index::npos);
    }

    void insert_stored(void *value, const void *key, const void *mapped, std::pmr::memory_resource &memory) const {
        auto &target = storage(value);
        const auto hash = util::linear_hash_table::hash_code(key_runtime_type().hash(key));
        const auto position = target.positions.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept {
            (void)target.positions.reset(position);
            target.positions.trim(memory);
        }};
        target.keys.emplace_at_with(position, key_layout_, memory,
                                    [&](void *stored) { key_runtime_type().copy_construct(stored, key, memory); });
        auto release_key = util::scope_exit{[&]() noexcept {
            target.keys.erase_at_with(position, key_layout_, memory,
                                      [&](void *stored) noexcept { key_runtime_type().destroy(stored, memory); });
        }};
        target.values.emplace_at_with(position, mapped_layout_, memory, [&](void *stored) {
            mapped_runtime_type().copy_construct(stored, mapped, memory);
        });
        auto release_value = util::scope_exit{[&]() noexcept {
            target.values.erase_at_with(position, mapped_layout_, memory,
                                        [&](void *stored) noexcept { mapped_runtime_type().destroy(stored, memory); });
        }};
        target.lookup.insert(hash, position, memory);
        release_value.release();
        release_key.release();
        release_position.release();
    }

    util::runtime_item_layout key_layout_;
    util::runtime_item_layout mapped_layout_;
};

struct ScalarRuntimeTypes {
    std::mutex mutex;
    std::vector<const ScalarRuntimeType *> native;
    std::vector<std::unique_ptr<ScalarRuntimeType>> composed;
};

ScalarRuntimeTypes &scalar_runtime_types() {
    static auto value = ScalarRuntimeTypes{};
    return value;
}

const ScalarRuntimeType *registered_runtime_type(const ScalarType &type, bool native) {
    auto &runtime_types = scalar_runtime_types();
    const auto lock = std::lock_guard{runtime_types.mutex};
    if (native) {
        const auto found =
            std::ranges::find_if(runtime_types.native, [&](const auto *item) { return &item->type() == &type; });
        return found == runtime_types.native.end() ? nullptr : *found;
    }

    const auto found =
        std::ranges::find_if(runtime_types.composed, [&](const auto &item) { return &item->type() == &type; });
    return found == runtime_types.composed.end() ? nullptr : found->get();
}

} // namespace

bool ScalarRuntimeType::equals(const void *value, const details::ScalarData &other) const noexcept {
    return &type() == &other.type() && equals_same_type(value, other);
}

namespace details {

void release_scalar_value(const ScalarData &value) noexcept {
    if (value.empty()) {
        return;
    }

    const auto mode = value.mode();
    if (mode == ScalarDataMode::stored_view || mode == ScalarDataMode::source_view ||
        mode == ScalarDataMode::inline_value) {
        return;
    }

    const auto &runtime_type = value.runtime_type();
    const auto value_offset = allocation_value_offset(runtime_type, mode);
    auto *allocation = reinterpret_cast<std::byte *>(value.data_word()) - value_offset;
    auto *memory = std::pmr::new_delete_resource();
    if (mode == ScalarDataMode::owned_explicit_resource) {
        memory = reinterpret_cast<const ScalarValueAllocation *>(allocation)->memory;
    } else if (mode == ScalarDataMode::owned_context_resource) {
#ifndef NDEBUG
        check_context_scalar_deallocation(reinterpret_cast<const void *>(value.data_word()));
#endif
        const auto *owner = current_scalar_allocation_owner();
        assert(owner != nullptr);
        memory = owner->memory;
    }

    runtime_type.destroy(reinterpret_cast<void *>(value.data_word()), *memory);
    memory->deallocate(allocation, value_offset + runtime_type.size(), allocation_alignment(runtime_type, mode));
}

} // namespace details

void register_scalar_runtime_type(const ScalarRuntimeType &runtime_type) {
    auto &runtime_types = scalar_runtime_types();
    const auto lock = std::lock_guard{runtime_types.mutex};
    if (std::ranges::find(runtime_types.native, &runtime_type) == runtime_types.native.end()) {
        runtime_types.native.push_back(&runtime_type);
    }
}

const ScalarRuntimeType &resolve_scalar_runtime_type(const ScalarType &type,
                                                     std::span<const ScalarRuntimeType *const> available) {
    const auto supplied =
        std::ranges::find_if(available, [&](const auto *item) { return item != nullptr && &item->type() == &type; });
    if (supplied != available.end()) {
        return **supplied;
    }
    if (const auto *native = registered_runtime_type(type, true)) {
        return *native;
    }
    if (const auto *composed = registered_runtime_type(type, false)) {
        return *composed;
    }

    auto runtime_type = std::unique_ptr<ScalarRuntimeType>{};
    type.visit(
        [&](const ListScalarType *list) {
            auto &item = resolve_scalar_runtime_type(list->item_type(), available);
            runtime_type = std::make_unique<ComposedListRuntimeType>(*list, item);
        },
        [&](const SetScalarType *set) {
            auto &item = resolve_scalar_runtime_type(set->item_type(), available);
            runtime_type = std::make_unique<ComposedSetRuntimeType>(*set, item);
        },
        [&](const DictScalarType *dict) {
            auto &key = resolve_scalar_runtime_type(dict->key_type(), available);
            auto &mapped = resolve_scalar_runtime_type(dict->mapped_type(), available);
            runtime_type = std::make_unique<ComposedDictRuntimeType>(*dict, key, mapped);
        },
        ddv::noop);
    if (runtime_type == nullptr) {
        throw std::logic_error{"resolved scalar type has no runtime type"};
    }

    auto &runtime_types = scalar_runtime_types();
    const auto lock = std::lock_guard{runtime_types.mutex};
    const auto existing =
        std::ranges::find_if(runtime_types.composed, [&](const auto &item) { return &item->type() == &type; });
    if (existing != runtime_types.composed.end()) {
        return **existing;
    }

    auto &result = *runtime_type;
    runtime_types.composed.push_back(std::move(runtime_type));
    return result;
}

} // namespace bg

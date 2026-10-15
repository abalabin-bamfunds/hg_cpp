#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/runtime_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/byte_copy.hpp>
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

bool ScalarRuntimeType::accepts_input(const ScalarRuntimeType &source) const noexcept {
    return &type() == &source.type();
}

bool ScalarListRuntimeType::accepts_input(const ScalarRuntimeType &source) const noexcept {
    if (type_cast<ListScalarType>(&source.type()) == nullptr) {
        return false;
    }

    const auto &source_list = static_cast<const ScalarListRuntimeType &>(source);
    return item_runtime_type().accepts_input(source_list.item_runtime_type());
}

bool ScalarRuntimeType::assign_with(void *destination, ScalarConstruction construction,
                                   bool suppress_equal) const {
    auto memory = details::current_scalar_allocator();
    auto *replacement = memory.allocate_bytes(size(), alignment());
    auto release_replacement = util::scope_exit{[&]() noexcept {
        memory.deallocate_bytes(replacement, size(), alignment());
    }};
    construction.construct(construction.context, *this, replacement, memory);
    auto destroy_replacement = util::scope_exit{[&]() noexcept { destroy(replacement, memory); }};
    const auto source = borrow(replacement);

    if (suppress_equal && equals(destination, source)) {
        return false;
    }

    assign_from_input(destination, source);
    return true;
}

bool ScalarDictRuntimeType::accepts_input(const ScalarRuntimeType &source) const noexcept {
    const auto *source_type = type_cast<DictScalarType>(&source.type());
    const auto &destination_type = static_cast<const DictScalarType &>(type());
    if (source_type == nullptr || &destination_type.key_type() != &source_type->key_type()) {
        return false;
    }

    const auto &source_dict = static_cast<const ScalarDictRuntimeType &>(source);
    return key_runtime_type().accepts_input(source_dict.key_runtime_type()) &&
           mapped_runtime_type().accepts_input(source_dict.mapped_runtime_type());
}

void ScalarRuntimeType::construct_from_input(void *destination, const details::ScalarData &value,
                                             allocator &memory) const {
    if (!accepts_input(value.runtime_type())) {
        throw std::invalid_argument{"incompatible scalar runtime representations"};
    }
    do_construct_from_input(destination, value, memory);
}

void ScalarRuntimeType::assign_from_input(void *destination, const details::ScalarData &value) const {
    if (!accepts_input(value.runtime_type())) {
        throw std::invalid_argument{"incompatible scalar runtime representations"};
    }
    do_assign_from_input(destination, value);
}

void ScalarRuntimeType::assign_from_owned(void *destination, details::ScalarData &value,
                                          std::optional<allocator> source_memory) const {
    if (!accepts_input(value.runtime_type())) {
        throw std::invalid_argument{"incompatible scalar runtime representations"};
    }

    const auto owns_storage = value.mode() == ScalarDataMode::owned_explicit_allocator ||
                              value.mode() == ScalarDataMode::owned_context_allocator ||
                              value.mode() == ScalarDataMode::owned_default_allocator;
    if (this == &value.runtime_type() && owns_storage && source_memory) {
        auto &destination_memory = details::current_scalar_allocator();
        if (try_assign_from_owned(destination, const_cast<void *>(value.data()), *source_memory,
                                  destination_memory)) {
            return;
        }
    }

    do_assign_from_input(destination, value);
}

details::ScalarData ScalarRuntimeType::borrow(const void *storage) const noexcept {
    if (has_runtime_type_flag(shared_handle_flag)) {
        return borrow_shared(storage);
    }
    return details::ScalarData{*this, storage};
}

details::ScalarData ScalarRuntimeType::borrow_shared(const void *) const noexcept {
    std::terminate();
}

namespace {

struct RuntimeListStorage {
    static constexpr bool supports_byte_copy_initialization = true;

    util::runtime_contiguous_bunch items;

    void swap(RuntimeListStorage &other) noexcept { items.swap(other.items); }
};

static_assert(util::byte_copy_initializable<RuntimeListStorage>);

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

    void construct_from_value(void *, const void *, allocator &) const override { std::terminate(); }

    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override {
        construct_initial_value(destination);
        const auto source = Scalar<List<SCALAR>>::from(value.runtime_type(), value.data());
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (auto index = std::size_t{}; index < source.size(); ++index) {
            const auto item = source[index];
            append(destination, details::scalar_data(item), memory);
        }
        rollback.release();
    }

    void copy_construct(void *destination, const void *source, allocator &memory) const override {
        construct_initial_value(destination);
        const auto &items = static_cast<const RuntimeListStorage *>(source)->items;
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            storage(destination).items.emplace_back_with(item_layout_, memory.memory_resource(), [&](void *item) {
                item_runtime_type().copy_construct(item, items.get(index, item_layout_), memory);
            });
        }
        rollback.release();
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }

    void do_assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto input = value;
        const auto construct = [](void *context, const ScalarRuntimeType &runtime_type, void *target,
                                  allocator &memory) {
            runtime_type.construct_from_input(target, *static_cast<const details::ScalarData *>(context), memory);
        };
        (void)assign_with(destination, {&input, construct}, false);
    }

    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override {
        auto memory = details::current_scalar_allocator();
        alignas(RuntimeListStorage) std::byte replacement_bytes[sizeof(RuntimeListStorage)];
        construction.construct(construction.context, *this, replacement_bytes, memory);
        auto *replacement = std::launder(reinterpret_cast<RuntimeListStorage *>(replacement_bytes));
        auto release_replacement = util::scope_exit{[&]() noexcept { destroy(replacement, memory); }};
        const auto source = details::ScalarData{*this, replacement};

        if (suppress_equal && equals(destination, source)) {
            return false;
        }

        storage(destination).swap(*replacement);
        return true;
    }

    bool try_assign_from_owned(void *destination, void *source, allocator &source_memory,
                               allocator &destination_memory) const noexcept override {
        if (!source_memory.memory_resource().is_equal(destination_memory.memory_resource())) {
            return false;
        }

        storage(destination).swap(*static_cast<RuntimeListStorage *>(source));
        return true;
    }

    std::size_t hash(const void *value) const override {
        constexpr auto combine_constant = std::size_t{0x9e3779b9};
        const auto &items = storage(value).items;
        auto result = std::size_t{};
        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            result ^= item_runtime_type().hash(items.get(index, item_layout_)) + combine_constant + (result << 6) +
                      (result >> 2);
        }
        return result;
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const override {
        const auto source = Scalar<List<SCALAR>>::from(other.runtime_type(), other.data());
        const auto &items = storage(value).items;
        if (items.size() != source.size()) {
            return false;
        }

        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            const auto source_item = source[index];
            if (!item_runtime_type().equals(items.get(index, item_layout_), details::scalar_data(source_item))) {
                return false;
            }
        }
        return true;
    }

    void destroy(void *value, allocator &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeListStorage *>(value));
    }

    std::size_t item_count(const void *value) const noexcept override { return storage(value).items.size(); }

    const void *item(const void *value, std::size_t index) const noexcept override {
        return storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_);
    }

    void *item(void *value, std::size_t index) const noexcept override {
        return storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_);
    }

    void emplace_back_with(void *value, allocator &memory, ScalarConstruction construction) const override {
        storage(value).items.emplace_back_with(item_layout_, memory.memory_resource(), [&](void *destination) {
            construction.construct(construction.context, item_runtime_type(), destination, memory);
        });
    }

    void append(void *value, const details::ScalarData &item, allocator &memory) const override {
        append(value, item.runtime_type(), memory, item.data());
    }

    void set(void *value, std::size_t index, const details::ScalarData &item) const override {
        item.assign(storage(value).items.get(static_cast<std::uint32_t>(index), item_layout_), item_runtime_type());
    }

    void erase(void *value, std::size_t index, allocator &memory) const override {
        auto &items = storage(value).items;
        const auto position = static_cast<std::uint32_t>(index);
        auto replacement = RuntimeListStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        for (auto current = std::uint32_t{}; current < items.size(); ++current) {
            if (current == position) {
                continue;
            }

            replacement.items.emplace_back_with(item_layout_, memory.memory_resource(), [&](void *target) {
                item_runtime_type().copy_construct(target, items.get(current, item_layout_), memory);
            });
        }

        storage(value).swap(replacement);
    }

    void clear(void *value, allocator &memory) const noexcept override {
        storage(value).items.release_with(item_layout_, memory.memory_resource(),
                                          [&](void *item) noexcept { item_runtime_type().destroy(item, memory); });
    }

  private:
    static RuntimeListStorage &storage(void *value) noexcept { return *static_cast<RuntimeListStorage *>(value); }
    static const RuntimeListStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeListStorage *>(value);
    }

    void append(void *value, const ScalarRuntimeType &runtime_type, allocator &memory, const void *data) const {
        const auto item = details::ScalarData{runtime_type, data};
        storage(value).items.emplace_back_with(item_layout_, memory.memory_resource(), [&](void *target) {
            item.copy_construct(target, item_runtime_type(), memory);
        });
    }

    util::runtime_item_layout item_layout_;
};

struct RuntimeSetStorage {
    static constexpr bool supports_byte_copy_initialization = true;

    util::occupancy_index positions;
    util::runtime_sparse_bunch items;
    util::linear_hash_table lookup;

    void swap(RuntimeSetStorage &other) noexcept {
        positions.swap(other.positions);
        items.swap(other.items);
        lookup.swap(other.lookup);
    }
};

static_assert(util::byte_copy_initializable<RuntimeSetStorage>);

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

    void construct_from_value(void *, const void *, allocator &) const override { std::terminate(); }

    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override {
        construct_initial_value(destination);
        const auto source = Scalar<Set<SCALAR>>::from(value.runtime_type(), value.data());
        auto copy = RuntimeSetCopy{this, destination, &memory};
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        source.for_each([&](const Scalar<SCALAR> &item) {
            (void)copy.runtime_type->insert(copy.storage, details::scalar_data(item), *copy.memory);
        });
        rollback.release();
    }

    void copy_construct(void *destination, const void *source, allocator &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto position : storage(source).positions) {
            insert_stored(destination, storage(source).items.get(position, item_layout_), memory);
        }
        rollback.release();
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }

    void do_assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto memory = details::current_scalar_allocator();
        auto replacement = RuntimeSetStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        auto copy = RuntimeSetCopy{this, &replacement, &memory};
        Scalar<Set<SCALAR>>::from(value.runtime_type(), value.data()).for_each([&](const Scalar<SCALAR> &item) {
            (void)copy.runtime_type->insert(copy.storage, details::scalar_data(item), *copy.memory);
        });

        storage(destination).swap(replacement);
    }

    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override {
        auto memory = details::current_scalar_allocator();
        alignas(RuntimeSetStorage) std::byte replacement_bytes[sizeof(RuntimeSetStorage)];
        construction.construct(construction.context, *this, replacement_bytes, memory);
        auto *replacement = std::launder(reinterpret_cast<RuntimeSetStorage *>(replacement_bytes));
        auto release_replacement = util::scope_exit{[&]() noexcept { destroy(replacement, memory); }};
        const auto source = borrow(replacement);

        if (suppress_equal && equals(destination, source)) {
            return false;
        }

        storage(destination).swap(*replacement);
        return true;
    }

    bool try_assign_from_owned(void *destination, void *source, allocator &source_memory,
                               allocator &destination_memory) const noexcept override {
        if (!source_memory.memory_resource().is_equal(destination_memory.memory_resource())) {
            return false;
        }

        storage(destination).swap(*static_cast<RuntimeSetStorage *>(source));
        return true;
    }

    std::size_t hash(const void *value) const override {
        auto result = std::size_t{};
        for (const auto position : storage(value).positions) {
            result ^= item_runtime_type().hash(storage(value).items.get(position, item_layout_));
        }
        return result;
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const override {
        const auto source = Scalar<Set<SCALAR>>::from(other.runtime_type(), other.data());
        if (item_count(value) != source.size()) {
            return false;
        }

        auto comparison = RuntimeSetComparison{this, value, true};
        source.for_each([&](const Scalar<SCALAR> &item) {
            comparison.equal &= comparison.runtime_type->contains(comparison.storage, details::scalar_data(item));
        });
        return comparison.equal;
    }

    void destroy(void *value, allocator &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeSetStorage *>(value));
    }

    std::size_t item_count(const void *value) const noexcept override { return storage(value).positions.size(); }

    bool contains(const void *value, const details::ScalarData &item) const override {
        return find(storage(value), item) != util::occupancy_index::npos;
    }

    void for_each(const void *value, void *context, ScalarSetRuntimeType::ItemFunction function) const override {
        for (const auto position : storage(value).positions) {
            function(context, {item_runtime_type(), storage(value).items.get(position, item_layout_)});
        }
    }

    [[nodiscard]] bool insert(void *value, const details::ScalarData &item, allocator &memory) const override {
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

    [[nodiscard]] bool insert_with(void *value, allocator &memory,
                                   ScalarConstruction construction) const override {
        auto &target = storage(value);
        const auto position = target.positions.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept {
            (void)target.positions.reset(position);
            target.positions.trim(memory);
        }};

        target.items.emplace_at_with(position, item_layout_, memory, [&](void *stored) {
            construction.construct(construction.context, item_runtime_type(), stored, memory);
        });
        auto release_item = util::scope_exit{[&]() noexcept {
            target.items.erase_at_with(position, item_layout_, memory,
                                       [&](void *stored) noexcept { item_runtime_type().destroy(stored, memory); });
        }};

        const auto *constructed_item = target.items.get(position, item_layout_);
        const auto hash = util::linear_hash_table::hash_code(item_runtime_type().hash(constructed_item));
        const auto candidate_data = item_runtime_type().borrow(constructed_item);
        if (target.lookup.find(hash, [&](auto existing_position) {
                return item_runtime_type().equals(target.items.get(existing_position, item_layout_), candidate_data);
            })) {
            return false;
        }

        target.lookup.insert(hash, position, memory);
        release_item.release();
        release_position.release();
        return true;
    }

    [[nodiscard]] bool erase(void *value, const details::ScalarData &item, allocator &memory) const override {
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

    void clear(void *value, allocator &memory) const noexcept override {
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
        allocator *memory;
    };

    struct RuntimeSetComparison {
        const ComposedSetRuntimeType *runtime_type;
        const void *storage;
        bool equal;
    };

    static RuntimeSetStorage &storage(void *value) noexcept { return *static_cast<RuntimeSetStorage *>(value); }
    static const RuntimeSetStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeSetStorage *>(value);
    }

    std::uint32_t find(const RuntimeSetStorage &source, const details::ScalarData &item) const {
        const auto hash = util::linear_hash_table::hash_code(item.hash());
        const auto result = source.lookup.find(hash, [&](auto position) {
            return item_runtime_type().equals(source.items.get(position, item_layout_), item);
        });
        return result.value_or(util::occupancy_index::npos);
    }

    void insert_stored(void *value, const void *item, allocator &memory) const {
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
    static constexpr bool supports_byte_copy_initialization = true;

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

static_assert(util::byte_copy_initializable<RuntimeDictStorage>);

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

    void construct_from_value(void *, const void *, allocator &) const override { std::terminate(); }

    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override {
        construct_initial_value(destination);
        auto copy = RuntimeDictCopy{this, destination, &memory};
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        Scalar<Dict<SCALAR, SCALAR>>::from(value.runtime_type(), value.data())
            .for_each([&](const Scalar<SCALAR> &key, const Scalar<SCALAR> &mapped) {
                copy.runtime_type->insert_or_assign(copy.storage, details::scalar_data(key),
                                                    details::scalar_data(mapped), *copy.memory);
            });
        rollback.release();
    }

    void copy_construct(void *destination, const void *source, allocator &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { destroy(destination, memory); }};
        for (const auto position : storage(source).positions) {
            insert_stored(destination, storage(source).keys.get(position, key_layout_),
                          storage(source).values.get(position, mapped_layout_), memory);
        }
        rollback.release();
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }

    void do_assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto memory = details::current_scalar_allocator();
        auto replacement = RuntimeDictStorage{};
        const auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        auto copy = RuntimeDictCopy{this, &replacement, &memory};
        Scalar<Dict<SCALAR, SCALAR>>::from(value.runtime_type(), value.data())
            .for_each([&](const Scalar<SCALAR> &key, const Scalar<SCALAR> &mapped) {
                copy.runtime_type->insert_or_assign(copy.storage, details::scalar_data(key),
                                                    details::scalar_data(mapped), *copy.memory);
            });

        storage(destination).swap(replacement);
    }

    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override {
        auto memory = details::current_scalar_allocator();
        alignas(RuntimeDictStorage) std::byte replacement_bytes[sizeof(RuntimeDictStorage)];
        construction.construct(construction.context, *this, replacement_bytes, memory);
        auto *replacement = std::launder(reinterpret_cast<RuntimeDictStorage *>(replacement_bytes));
        auto release_replacement = util::scope_exit{[&]() noexcept { destroy(replacement, memory); }};
        const auto source = borrow(replacement);

        if (suppress_equal && equals(destination, source)) {
            return false;
        }

        storage(destination).swap(*replacement);
        return true;
    }

    bool try_assign_from_owned(void *destination, void *source, allocator &source_memory,
                               allocator &destination_memory) const noexcept override {
        if (!source_memory.memory_resource().is_equal(destination_memory.memory_resource())) {
            return false;
        }

        storage(destination).swap(*static_cast<RuntimeDictStorage *>(source));
        return true;
    }

    std::size_t hash(const void *value) const override {
        auto result = std::size_t{};
        for (const auto position : storage(value).positions) {
            result ^= key_runtime_type().hash(storage(value).keys.get(position, key_layout_)) ^
                      (mapped_runtime_type().hash(storage(value).values.get(position, mapped_layout_)) << 1);
        }
        return result;
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const override {
        const auto source = Scalar<Dict<SCALAR, SCALAR>>::from(other.runtime_type(), other.data());
        if (item_count(value) != source.size()) {
            return false;
        }

        auto comparison = RuntimeDictComparison{this, value, true};
        source.for_each([&](const Scalar<SCALAR> &key, const Scalar<SCALAR> &mapped) {
            const auto position =
                comparison.runtime_type->find_position(storage(comparison.storage), details::scalar_data(key));
            comparison.equal &=
                position != util::occupancy_index::npos &&
                comparison.runtime_type->mapped_runtime_type().equals(
                    storage(comparison.storage).values.get(position, comparison.runtime_type->mapped_layout_),
                    details::scalar_data(mapped));
        });
        return comparison.equal;
    }

    void destroy(void *value, allocator &memory) const noexcept override {
        clear(value, memory);
        std::destroy_at(static_cast<RuntimeDictStorage *>(value));
    }

    std::size_t item_count(const void *value) const noexcept override { return storage(value).positions.size(); }

    bool contains(const void *value, const details::ScalarData &key) const override {
        return find(value, key).has_value();
    }

    std::optional<ScalarDictRuntimeType::ConstItem> find(const void *value,
                                                         const details::ScalarData &key) const override {
        const auto position = find_position(storage(value), key);
        if (position == util::occupancy_index::npos) {
            return std::nullopt;
        }

        return ScalarDictRuntimeType::ConstItem{mapped_runtime_type(),
                                                storage(value).values.get(position, mapped_layout_)};
    }

    ScalarDictRuntimeType::ConstItem at(const void *value, const details::ScalarData &key) const override {
        const auto item = find(value, key);
        if (!item) {
            throw std::out_of_range{"dictionary key was not found"};
        }
        return *item;
    }

    ScalarDictRuntimeType::Item at(void *value, const details::ScalarData &key) const override {
        const auto position = find_position(storage(value), key);
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

    std::unique_ptr<ScalarDictRuntimeType::Cursor> make_cursor(const void *value) const override {
        return std::make_unique<DictionaryCursor>(*this, storage(value));
    }

    void insert_or_assign(void *value, const details::ScalarData &key, const details::ScalarData &mapped,
                          allocator &memory) const override {
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

    [[nodiscard]] bool try_emplace_with(void *value, allocator &memory, ScalarConstruction key,
                                        ScalarConstruction mapped) const override {
        auto &target = storage(value);
        const auto position = target.positions.acquire(memory);
        auto release_position = util::scope_exit{[&]() noexcept {
            (void)target.positions.reset(position);
            target.positions.trim(memory);
        }};

        target.keys.emplace_at_with(position, key_layout_, memory, [&](void *destination) {
            key.construct(key.context, key_runtime_type(), destination, memory);
        });
        auto release_key = util::scope_exit{[&]() noexcept {
            target.keys.erase_at_with(position, key_layout_, memory,
                                      [&](void *stored) noexcept { key_runtime_type().destroy(stored, memory); });
        }};

        auto *candidate = target.keys.get(position, key_layout_);
        const auto hash = util::linear_hash_table::hash_code(key_runtime_type().hash(candidate));
        if (target.lookup.find(hash, [&](auto existing) {
                return key_runtime_type().equals(target.keys.get(existing, key_layout_),
                                                 details::ScalarData{key_runtime_type(), candidate});
            })) {
            return false;
        }

        target.values.emplace_at_with(position, mapped_layout_, memory, [&](void *destination) {
            mapped.construct(mapped.context, mapped_runtime_type(), destination, memory);
        });
        auto release_value = util::scope_exit{[&]() noexcept {
            target.values.erase_at_with(position, mapped_layout_, memory,
                                        [&](void *stored) noexcept { mapped_runtime_type().destroy(stored, memory); });
        }};
        target.lookup.insert(hash, position, memory);
        release_value.release();
        release_key.release();
        release_position.release();
        return true;
    }

    [[nodiscard]] bool erase(void *value, const details::ScalarData &key, allocator &memory) const override {
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

    void clear(void *value, allocator &memory) const noexcept override {
        auto &target = storage(value);
        target.lookup.release(memory);
        target.values.release_with(target.positions, mapped_layout_, memory,
                                   [&](void *item) noexcept { mapped_runtime_type().destroy(item, memory); });
        target.keys.release_with(target.positions, key_layout_, memory,
                                 [&](void *item) noexcept { key_runtime_type().destroy(item, memory); });
        target.positions.release(memory);
    }

  private:
    class DictionaryCursor final : public ScalarDictRuntimeType::Cursor {
      public:
        DictionaryCursor(const ComposedDictRuntimeType &runtime_type, const RuntimeDictStorage &storage)
            : runtime_type_{&runtime_type}, storage_{&storage}, position_{storage.positions.begin()},
              end_{storage.positions.end()} {}

        std::size_t next_batch(std::span<ScalarDictRuntimeType::BatchEntry> buffer) override {
            auto count = std::size_t{};
            while (count < buffer.size() && position_ != end_) {
                const auto position = *position_++;
                buffer[count++] = {storage_->keys.get(position, runtime_type_->key_layout_),
                                   storage_->values.get(position, runtime_type_->mapped_layout_)};
            }

            return count;
        }

      private:
        const ComposedDictRuntimeType *runtime_type_;
        const RuntimeDictStorage *storage_;
        util::occupancy_index::iterator position_;
        util::occupancy_index::iterator end_;
    };

    struct RuntimeDictCopy {
        const ComposedDictRuntimeType *runtime_type;
        void *storage;
        allocator *memory;
    };

    struct RuntimeDictComparison {
        const ComposedDictRuntimeType *runtime_type;
        const void *storage;
        bool equal;
    };

    static RuntimeDictStorage &storage(void *value) noexcept { return *static_cast<RuntimeDictStorage *>(value); }
    static const RuntimeDictStorage &storage(const void *value) noexcept {
        return *static_cast<const RuntimeDictStorage *>(value);
    }

    std::uint32_t find_position(const RuntimeDictStorage &source, const details::ScalarData &key) const {
        const auto hash = util::linear_hash_table::hash_code(key.hash());
        const auto result = source.lookup.find(hash, [&](auto position) {
            return key_runtime_type().equals(source.keys.get(position, key_layout_), key);
        });
        return result.value_or(util::occupancy_index::npos);
    }

    void insert_stored(void *value, const void *key, const void *mapped, allocator &memory) const {
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
    std::recursive_mutex mutex;
    std::vector<const ScalarRuntimeType *> native;
    std::vector<std::unique_ptr<ScalarRuntimeType>> composed;
};

ScalarRuntimeTypes &scalar_runtime_types() {
    static auto value = ScalarRuntimeTypes{};
    return value;
}

void validate_runtime_interface(const ScalarRuntimeType &runtime_type) {
    auto valid = true;
    runtime_type.type().visit(
        [&](const HierarchicalScalarType *) {
            valid = dynamic_cast<const HierarchicalScalarRuntimeType *>(&runtime_type) != nullptr;
        },
        [&](const StructScalarType *) {
            valid = dynamic_cast<const StructScalarRuntimeType *>(&runtime_type) != nullptr;
        },
        [&](const ListScalarType *) { valid = dynamic_cast<const ScalarListRuntimeType *>(&runtime_type) != nullptr; },
        [&](const SetScalarType *) { valid = dynamic_cast<const ScalarSetRuntimeType *>(&runtime_type) != nullptr; },
        [&](const DictScalarType *) { valid = dynamic_cast<const ScalarDictRuntimeType *>(&runtime_type) != nullptr; },
        ddv::noop);
    if (!valid) {
        throw std::invalid_argument{"scalar runtime type does not implement its semantic type interface"};
    }
}

const ScalarRuntimeType *registered_runtime_type(ScalarRuntimeTypes &runtime_types, const ScalarType &type,
                                                 bool native) {
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

namespace details {

class ScalarRuntimeTypeResolver {
  public:
    ScalarRuntimeTypeResolver(ScalarRuntimeTypes &runtime_types, std::span<const ScalarRuntimeType *const> available)
        : runtime_types_{runtime_types}, available_{available} {}

    ~ScalarRuntimeTypeResolver() {
        if (committed_) {
            return;
        }

        for (auto created = created_.rbegin(); created != created_.rend(); ++created) {
            const auto entry =
                std::ranges::find_if(runtime_types_.composed, [&](const auto &item) { return item.get() == *created; });
            if (entry != runtime_types_.composed.end()) {
                runtime_types_.composed.erase(entry);
            }
        }
    }

    const ScalarRuntimeType &resolve(const ScalarType &type) {
        const auto supplied = std::ranges::find_if(
            available_, [&](const auto *item) { return item != nullptr && &item->type() == &type; });
        if (supplied != available_.end()) {
            validate_runtime_interface(**supplied);
            return **supplied;
        }
        if (const auto *native = registered_runtime_type(runtime_types_, type, true)) {
            return *native;
        }
        if (const auto *composed = registered_runtime_type(runtime_types_, type, false)) {
            return *composed;
        }
        if (const auto *builtin = resolve_builtin(type)) {
            return *builtin;
        }

        const ScalarRuntimeType *result = nullptr;
        type.visit(
            [&](const HierarchicalScalarType *hierarchical) {
                if (!hierarchical->is_resolved()) {
                    throw std::invalid_argument{"an unresolved hierarchical scalar has no runtime type"};
                }
                result = &resolve_composed(static_cast<const ResolvedHierarchicalScalarType &>(*hierarchical));
            },
            [&](const ListScalarType *list) {
                auto runtime_type = std::make_unique<ComposedListRuntimeType>(*list, resolve(list->item_type()));
                result = &publish(std::move(runtime_type));
            },
            [&](const SetScalarType *set) {
                auto runtime_type = std::make_unique<ComposedSetRuntimeType>(*set, resolve(set->item_type()));
                result = &publish(std::move(runtime_type));
            },
            [&](const DictScalarType *dict) {
                const auto &key = resolve(dict->key_type());
                const auto &mapped = resolve(dict->mapped_type());
                auto runtime_type = std::make_unique<ComposedDictRuntimeType>(*dict, key, mapped);
                result = &publish(std::move(runtime_type));
            },
            ddv::noop);
        if (result == nullptr) {
            throw std::logic_error{"resolved scalar type has no runtime type"};
        }
        return *result;
    }

    const HierarchicalScalarRuntimeType &resolve_composed(const ResolvedHierarchicalScalarType &type) {
        if (const auto *existing = registered_runtime_type(runtime_types_, type, false)) {
            return static_cast<const HierarchicalScalarRuntimeType &>(*existing);
        }

        auto runtime_type = make_composed_hierarchical_scalar_runtime_type(type);
        auto *result = runtime_type.get();
        publish(std::move(runtime_type));

        auto fields = std::vector<const ScalarRuntimeType *>{};
        fields.reserve(type.field_count());
        for (const auto &field : type.fields()) {
            fields.push_back(&resolve(static_cast<const ScalarType &>(*field.type)));
        }
        initialize_composed_hierarchical_scalar_runtime_type(*result, fields);
        return *result;
    }

    void commit() noexcept { committed_ = true; }

  private:
    const ScalarRuntimeType *resolve_builtin(const ScalarType &type) {
        auto *result = static_cast<const ScalarRuntimeType *>(nullptr);
        auto match = [&]<typename Declaration>(tp::unit<Declaration>) {
            if (result == nullptr && &type == &cpp_scalar_type<Declaration>()) {
                result = &cpp_scalar_runtime_type<Declaration>();
            }
        };

        tp::for_each(builtin_scalar_declarations, match);
        match(tp::unit<int>{});
        match(tp::unit<float>{});
        return result;
    }

    ScalarRuntimeType &publish(std::unique_ptr<ScalarRuntimeType> runtime_type) {
        auto &result = *runtime_type;
        runtime_types_.composed.push_back(std::move(runtime_type));
        created_.push_back(&result);
        return result;
    }

    ScalarRuntimeTypes &runtime_types_;
    std::span<const ScalarRuntimeType *const> available_;
    std::vector<const ScalarRuntimeType *> created_;
    bool committed_{};
};

} // namespace details

bool ScalarRuntimeType::equals(const void *value, const details::ScalarData &other) const {
    return &type() == &other.type() && equals_same_type(value, other);
}

namespace details {

void release_scalar_value(const ScalarData &value) noexcept {
    if (value.empty()) {
        return;
    }

    const auto mode = value.mode();
    if (mode == ScalarDataMode::stored_view || mode == ScalarDataMode::source_view ||
        mode == ScalarDataMode::inline_value || mode == ScalarDataMode::shared_view) {
        return;
    }

    if (mode == ScalarDataMode::shared_value) {
        value.runtime_type().release_shared(value.data_word());
        return;
    }

    const auto &runtime_type = value.runtime_type();
    const auto value_offset = allocation_value_offset(runtime_type, mode);
    auto *allocation = reinterpret_cast<std::byte *>(value.data_word()) - value_offset;
    auto memory = allocator{};
    if (mode == ScalarDataMode::owned_explicit_allocator) {
        memory = reinterpret_cast<const ScalarValueAllocator *>(allocation)->memory;
    } else if (mode == ScalarDataMode::owned_context_allocator) {
#ifndef NDEBUG
        check_context_scalar_deallocation(reinterpret_cast<const void *>(value.data_word()));
#endif
        const auto *owner = current_scalar_allocation_owner();
        assert(owner != nullptr);
        memory = owner->memory;
    }

    runtime_type.destroy(reinterpret_cast<void *>(value.data_word()), memory);
    memory.deallocate_bytes(allocation, value_offset + runtime_type.size(), allocation_alignment(runtime_type, mode));
}

const HierarchicalScalarRuntimeType &
resolve_composed_hierarchical_scalar_runtime_type(const ResolvedHierarchicalScalarType &type) {
    auto &runtime_types = scalar_runtime_types();
    const auto lock = std::lock_guard{runtime_types.mutex};
    auto resolver = ScalarRuntimeTypeResolver{runtime_types, {}};
    const auto &result = resolver.resolve_composed(type);
    resolver.commit();
    return result;
}

} // namespace details

void register_scalar_runtime_type(const ScalarRuntimeType &runtime_type) {
    validate_runtime_interface(runtime_type);

    auto &runtime_types = scalar_runtime_types();
    const auto lock = std::lock_guard{runtime_types.mutex};
    if (std::ranges::find(runtime_types.native, &runtime_type) == runtime_types.native.end()) {
        runtime_types.native.push_back(&runtime_type);
    }
}

const ScalarRuntimeType &resolve_scalar_runtime_type(const ScalarType &type,
                                                     std::span<const ScalarRuntimeType *const> available) {
    auto &runtime_types = scalar_runtime_types();
    const auto lock = std::lock_guard{runtime_types.mutex};
    auto resolver = details::ScalarRuntimeTypeResolver{runtime_types, available};
    const auto &result = resolver.resolve(type);
    resolver.commit();
    return result;
}

} // namespace bg

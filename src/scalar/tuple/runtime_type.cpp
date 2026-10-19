#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/tuple/runtime_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace bg {
namespace {

class FixedTupleRuntimeType final : public ScalarTupleRuntimeType {
  public:
    explicit FixedTupleRuntimeType(const TupleScalarType &type) noexcept;

    void initialize(std::span<const ScalarRuntimeType *const> items);
    void construct_fields_with(void *destination, allocator &memory,
                               ScalarFieldConstruction construction) const override;
    const ScalarRuntimeType &item_runtime_type(std::size_t index) const override;
    details::ScalarData item(const details::ScalarData &value, std::size_t index) const override;
    bool accepts_input(const ScalarRuntimeType &source) const noexcept override;
    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override;
    bool try_assign_from_owned(void *destination, void *source, allocator &, allocator &) const noexcept override;

  private:
    struct Storage;

    struct FieldLayout {
        const ScalarRuntimeType *runtime_type;
        std::size_t offset;
    };

    struct CopyContext {
        const details::ScalarData *source;
    };

    details::ScalarData borrow_shared(const void *storage) const noexcept override;
    const ScalarRuntimeType *retain_shared(const details::ScalarData &, std::uintptr_t &) const noexcept override;
    void release_shared(std::uintptr_t data) const noexcept override;
    void construct_initial_value(void *destination) const noexcept override;
    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override;
    void copy_construct(void *destination, const void *source, allocator &memory) const override;
    void do_assign_from_input(void *destination, const details::ScalarData &value) const override;
    std::size_t hash(details::ScalarDataMode mode, std::uintptr_t data) const override;
    bool equals(details::ScalarDataMode mode, std::uintptr_t data,
                const details::ScalarData &other) const override;
    std::size_t hash(const void *value) const override;
    bool equals(const void *value, const details::ScalarData &other) const override;
    bool equals_same_type(const void *value, const details::ScalarData &other) const override;
    void destroy(void *value, allocator &memory) const noexcept override;

    static std::size_t align_up(std::size_t offset, std::size_t alignment);
    static bool copy_field(void *context, std::size_t index, const ScalarRuntimeType &runtime_type,
                           void *destination, allocator &memory);
    std::uintptr_t construct_shared(ScalarFieldConstruction construction, details::ScalarAllocationPool &pool,
                                    allocator &memory) const;
    Storage *storage(const details::ScalarData &value) const noexcept;
    static Storage *stored_handle(const void *value) noexcept;
    static void store_handle(void *destination, Storage *storage) noexcept;
    const void *field_storage(const Storage &storage, std::size_t index) const noexcept;
    void *field_storage(Storage &storage, std::size_t index) const noexcept;
    std::size_t hash_storage(const Storage &storage) const;
    void destroy_storage(Storage &storage) const noexcept;

    std::vector<FieldLayout> items_;
    std::size_t allocation_size_{};
    std::size_t allocation_alignment_{};
};

struct FixedTupleRuntimeType::Storage {
    explicit Storage(details::ScalarAllocationPool &allocation_pool) noexcept
        : references{1}, pool{&allocation_pool} {}

    std::atomic_size_t references;
    details::ScalarAllocationPool *pool;
};

FixedTupleRuntimeType::FixedTupleRuntimeType(const TupleScalarType &type) noexcept
    : ScalarTupleRuntimeType{type, sizeof(Storage *), alignof(Storage *), true, false, true, shared_handle_flag} {}

void FixedTupleRuntimeType::initialize(std::span<const ScalarRuntimeType *const> items) {
    if (items.size() != type().size()) {
        throw std::logic_error{"tuple scalar runtime item count does not match its semantic type"};
    }

    auto offset = sizeof(Storage);
    auto alignment = alignof(Storage);
    items_.reserve(items.size());
    for (const auto *item : items) {
        alignment = std::max(alignment, item->alignment());
        offset = align_up(offset, item->alignment());
        items_.push_back(FieldLayout{item, offset});
        if (item->size() > std::numeric_limits<std::size_t>::max() - offset) {
            throw std::length_error{"tuple scalar storage size overflows"};
        }
        offset += item->size();
    }

    allocation_alignment_ = alignment;
    allocation_size_ = align_up(offset, alignment);
}

std::size_t FixedTupleRuntimeType::align_up(std::size_t offset, std::size_t alignment) {
    const auto remainder = offset % alignment;
    if (remainder == 0) {
        return offset;
    }
    const auto padding = alignment - remainder;
    if (padding > std::numeric_limits<std::size_t>::max() - offset) {
        throw std::length_error{"tuple scalar storage size overflows"};
    }
    return offset + padding;
}

void FixedTupleRuntimeType::construct_fields_with(void *destination, allocator &memory,
                                                      ScalarFieldConstruction construction) const {
    auto *pool = static_cast<details::ScalarAllocationPool *>(nullptr);
    auto owns_pool = false;
    if (const auto *current = details::current_scalar_allocation_owner()) {
        auto pool_memory = current->scalar_allocation_pool->memory();
        if (&memory.memory_resource() == &current->memory.memory_resource() ||
            &memory.memory_resource() == &pool_memory.memory_resource()) {
            pool = current->scalar_allocation_pool;
        }
    }

    if (pool == nullptr) {
        pool = details::ScalarAllocationPool::create(memory);
        owns_pool = true;
    }
    auto release_pool = util::scope_exit{[&]() noexcept {
        if (owns_pool) {
            pool->release();
        }
    }};

    auto pool_memory = pool->memory();
    const auto handle = construct_shared(construction, *pool, pool_memory);
    store_handle(destination, reinterpret_cast<Storage *>(handle));
}

std::uintptr_t FixedTupleRuntimeType::construct_shared(ScalarFieldConstruction construction,
                                                          details::ScalarAllocationPool &pool,
                                                          allocator &memory) const {
    const auto *previous = details::current_scalar_allocation_owner();
    const auto graph = previous == nullptr ? nullptr : previous->graph;
    const auto node_index = previous == nullptr ? std::size_t{} : previous->node_index;
    const auto allocation_context = details::ScalarAllocationContext{memory, pool, graph, node_index};

    auto *bytes = static_cast<std::byte *>(memory.allocate_bytes(allocation_size_, allocation_alignment_));
    pool.retain();
    auto *stored = std::construct_at(reinterpret_cast<Storage *>(bytes), pool);
    auto constructed = std::size_t{};
    auto rollback = util::scope_exit{[&]() noexcept {
        while (constructed != 0) {
            --constructed;
            items_[constructed].runtime_type->destroy(field_storage(*stored, constructed), memory);
        }
        std::destroy_at(stored);
        memory.deallocate_bytes(bytes, allocation_size_, allocation_alignment_);
        pool.release();
    }};

    for (; constructed < items_.size(); ++constructed) {
        if (!construction.construct(construction.context, constructed, *items_[constructed].runtime_type,
                                    field_storage(*stored, constructed), memory)) {
            throw std::invalid_argument{"tuple scalar item was not supplied"};
        }
    }

    rollback.release();
    return reinterpret_cast<std::uintptr_t>(stored);
}

bool FixedTupleRuntimeType::copy_field(void *context, std::size_t index,
                                          const ScalarRuntimeType &runtime_type, void *destination,
                                          allocator &memory) {
    const auto &source = *static_cast<const CopyContext *>(context)->source;
    const auto &source_runtime = static_cast<const ScalarTupleRuntimeType &>(source.runtime_type());
    const auto source_item = source_runtime.item(source, index);
    runtime_type.construct_from_input(destination, source_item, memory);
    return true;
}

const ScalarRuntimeType &FixedTupleRuntimeType::item_runtime_type(std::size_t index) const {
    return *items_.at(index).runtime_type;
}

details::ScalarData FixedTupleRuntimeType::item(const details::ScalarData &value, std::size_t index) const {
    auto *stored = storage(value);
    if (stored == nullptr) {
        throw std::logic_error{"cannot read an empty tuple scalar"};
    }
    if (index >= items_.size()) {
        throw std::out_of_range{"tuple scalar item index"};
    }
    return items_[index].runtime_type->borrow(field_storage(*stored, index));
}

bool FixedTupleRuntimeType::accepts_input(const ScalarRuntimeType &source) const noexcept {
    if (&source == this) {
        return true;
    }

    const auto *source_type = type_cast<TupleScalarType>(&source.type());
    if (source_type == nullptr || type().size() != source_type->size()) {
        return false;
    }

    const auto &source_runtime = static_cast<const ScalarTupleRuntimeType &>(source);
    for (auto index = std::size_t{}; index < items_.size(); ++index) {
        if (!items_[index].runtime_type->accepts_input(source_runtime.item_runtime_type(index))) {
            return false;
        }
    }
    return true;
}

bool FixedTupleRuntimeType::assign_with(void *destination, ScalarConstruction construction,
                                           bool suppress_equal) const {
    auto memory = details::current_scalar_allocator();
    alignas(Storage *) std::byte replacement_bytes[sizeof(Storage *)];
    construction.construct(construction.context, *this, replacement_bytes, memory);
    auto *replacement = std::launder(reinterpret_cast<Storage **>(replacement_bytes));
    auto release_replacement = util::scope_exit{[&]() noexcept { destroy(replacement, memory); }};
    const auto source = borrow(replacement);

    if (suppress_equal && equals(destination, source)) {
        return false;
    }

    auto *previous = stored_handle(destination);
    store_handle(destination, stored_handle(replacement));
    store_handle(replacement, previous);
    return true;
}

bool FixedTupleRuntimeType::try_assign_from_owned(void *destination, void *source, allocator &,
                                                     allocator &) const noexcept {
    auto *previous = stored_handle(destination);
    store_handle(destination, stored_handle(source));
    store_handle(source, previous);
    return true;
}

details::ScalarData FixedTupleRuntimeType::borrow_shared(const void *value) const noexcept {
    auto *stored = stored_handle(value);
    if (stored == nullptr) {
        return details::ScalarData{*this, value};
    }
    return details::ScalarData{*this, reinterpret_cast<std::uintptr_t>(stored), details::ScalarDataMode::shared_view};
}

const ScalarRuntimeType *FixedTupleRuntimeType::retain_shared(const details::ScalarData &value,
                                                                std::uintptr_t &data) const noexcept {
    auto *stored = storage(value);
    if (stored == nullptr) {
        return nullptr;
    }
    stored->references.fetch_add(1, std::memory_order_relaxed);
    data = reinterpret_cast<std::uintptr_t>(stored);
    return this;
}

void FixedTupleRuntimeType::release_shared(std::uintptr_t data) const noexcept {
    auto *stored = reinterpret_cast<Storage *>(data);
    if (stored == nullptr || stored->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    destroy_storage(*stored);
}

void FixedTupleRuntimeType::construct_initial_value(void *destination) const noexcept { store_handle(destination, nullptr); }

void FixedTupleRuntimeType::do_construct_from_input(void *destination, const details::ScalarData &value,
                                                       allocator &memory) const {
    if (&value.runtime_type() == this) {
        auto *stored = storage(value);
        if (stored != nullptr) {
            stored->references.fetch_add(1, std::memory_order_relaxed);
        }
        store_handle(destination, stored);
        return;
    }

    auto context = CopyContext{&value};
    construct_fields_with(destination, memory, {&context, &FixedTupleRuntimeType::copy_field});
}

void FixedTupleRuntimeType::copy_construct(void *destination, const void *source, allocator &) const {
    auto *stored = stored_handle(source);
    if (stored != nullptr) {
        stored->references.fetch_add(1, std::memory_order_relaxed);
    }
    store_handle(destination, stored);
}

void FixedTupleRuntimeType::do_assign_from_input(void *destination, const details::ScalarData &value) const {
    auto source = value;
    const auto construct = [](void *context, const ScalarRuntimeType &runtime_type, void *target, allocator &memory) {
        runtime_type.construct_from_input(target, *static_cast<const details::ScalarData *>(context), memory);
    };
    (void)assign_with(destination, {&source, construct}, false);
}

std::size_t FixedTupleRuntimeType::hash(details::ScalarDataMode mode, std::uintptr_t data) const {
    auto *stored = storage(details::ScalarData{*this, data, mode});
    return stored == nullptr ? 0 : hash_storage(*stored);
}

bool FixedTupleRuntimeType::equals(details::ScalarDataMode mode, std::uintptr_t data,
                                     const details::ScalarData &other) const {
    if (&type() != &other.type()) {
        return false;
    }
    auto *left = storage(details::ScalarData{*this, data, mode});
    if (left == nullptr) {
        return false;
    }

    const auto &right_runtime = static_cast<const ScalarTupleRuntimeType &>(other.runtime_type());
    for (auto index = std::size_t{}; index < items_.size(); ++index) {
        if (!items_[index].runtime_type->equals(field_storage(*left, index), right_runtime.item(other, index))) {
            return false;
        }
    }
    return true;
}

std::size_t FixedTupleRuntimeType::hash(const void *value) const {
    auto *stored = stored_handle(value);
    return stored == nullptr ? 0 : hash_storage(*stored);
}

bool FixedTupleRuntimeType::equals(const void *value, const details::ScalarData &other) const {
    auto *stored = stored_handle(value);
    return stored != nullptr && equals(details::ScalarDataMode::shared_view,
                                       reinterpret_cast<std::uintptr_t>(stored), other);
}

bool FixedTupleRuntimeType::equals_same_type(const void *value, const details::ScalarData &other) const {
    return equals(value, other);
}

void FixedTupleRuntimeType::destroy(void *value, allocator &) const noexcept {
    auto *stored = stored_handle(value);
    store_handle(value, nullptr);
    if (stored != nullptr) {
        release_shared(reinterpret_cast<std::uintptr_t>(stored));
    }
}

FixedTupleRuntimeType::Storage *FixedTupleRuntimeType::storage(const details::ScalarData &value) const noexcept {
    if (value.mode() == details::ScalarDataMode::shared_view || value.mode() == details::ScalarDataMode::shared_value) {
        return reinterpret_cast<Storage *>(value.data_word());
    }
    return stored_handle(value.data());
}

FixedTupleRuntimeType::Storage *FixedTupleRuntimeType::stored_handle(const void *value) noexcept {
    auto handle = std::uintptr_t{};
    std::memcpy(&handle, value, sizeof(handle));
    return reinterpret_cast<Storage *>(handle);
}

void FixedTupleRuntimeType::store_handle(void *destination, Storage *storage) noexcept {
    const auto handle = reinterpret_cast<std::uintptr_t>(storage);
    std::memcpy(destination, &handle, sizeof(handle));
}

const void *FixedTupleRuntimeType::field_storage(const Storage &storage, std::size_t index) const noexcept {
    return reinterpret_cast<const std::byte *>(&storage) + items_[index].offset;
}

void *FixedTupleRuntimeType::field_storage(Storage &storage, std::size_t index) const noexcept {
    return reinterpret_cast<std::byte *>(&storage) + items_[index].offset;
}

std::size_t FixedTupleRuntimeType::hash_storage(const Storage &storage) const {
    auto result = std::hash<const TupleScalarType *>{}(&type());
    for (auto index = std::size_t{}; index < items_.size(); ++index) {
        result = details::combine_scalar_hash(result, items_[index].runtime_type->hash(field_storage(storage, index)));
    }
    return result;
}

void FixedTupleRuntimeType::destroy_storage(Storage &storage) const noexcept {
    auto memory = storage.pool->memory();
    for (auto index = items_.size(); index != 0; --index) {
        const auto field_index = index - 1;
        items_[field_index].runtime_type->destroy(field_storage(storage, field_index), memory);
    }
    auto *bytes = reinterpret_cast<std::byte *>(&storage);
    const auto pool = storage.pool;
    std::destroy_at(&storage);
    memory.deallocate_bytes(bytes, allocation_size_, allocation_alignment_);
    pool->release();
}

} // namespace

namespace details {

std::unique_ptr<ScalarTupleRuntimeType> make_fixed_tuple_runtime_type(const TupleScalarType &type) {
    return std::make_unique<FixedTupleRuntimeType>(type);
}

void initialize_fixed_tuple_runtime_type(ScalarTupleRuntimeType &runtime_type,
                                            std::span<const ScalarRuntimeType *const> items) {
    static_cast<FixedTupleRuntimeType &>(runtime_type).initialize(items);
}

} // namespace details
} // namespace bg

#include <bg/scalar/bundle/runtime_type.hpp>

#include <bg/scalar/bundle/details/allocation_owner.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <typeinfo>
#include <vector>

namespace bg {

namespace {

class ComposedHierarchicalScalarRuntimeType final : public HierarchicalScalarRuntimeType {
  public:
    explicit ComposedHierarchicalScalarRuntimeType(const ResolvedHierarchicalScalarType &type) noexcept;

    void initialize(std::span<const ScalarRuntimeType *const> fields);
    details::ScalarData field(const details::ScalarData &value, std::size_t index) const override;
    bool field_present(const details::ScalarData &value, std::size_t index) const override;
    bool accepts_input(const ScalarRuntimeType &source) const noexcept override;
    std::uintptr_t construct_shared(std::span<const details::ScalarData *const> fields,
                                    details::BundleAllocationOwner &owner) const;

  private:
    struct Storage;

    struct FieldLayout {
        const ScalarRuntimeType *runtime_type;
        std::uint32_t offset;
        std::uint32_t optional_bit;
    };

    static constexpr auto required_field = std::numeric_limits<std::uint32_t>::max();

    details::ScalarData borrow_shared(const void *storage) const noexcept override;
    const ScalarRuntimeType *retain_shared(const details::ScalarData &value,
                                           std::uintptr_t &data) const noexcept override;
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

    Storage *storage(const details::ScalarData &value) const noexcept;
    static Storage *stored_handle(const void *value) noexcept;
    static void store_handle(void *destination, Storage *storage) noexcept;
    static std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept;
    const void *field_storage(const Storage &storage, std::size_t index) const noexcept;
    void *field_storage(Storage &storage, std::size_t index) const noexcept;
    bool field_present(const Storage &storage, std::size_t index) const noexcept;
    void set_field_present(Storage &storage, std::size_t index) const noexcept;
    std::size_t hash_storage(const Storage &storage) const;
    void destroy_storage(Storage &storage) const noexcept;

    std::vector<FieldLayout> fields_;
    std::size_t allocation_size_{};
    std::size_t allocation_alignment_{alignof(void *)};
    std::size_t optional_words_{};
};

struct ComposedHierarchicalScalarRuntimeType::Storage {
    Storage(details::BundleAllocationOwner &allocation_owner,
            const ComposedHierarchicalScalarRuntimeType &actual_runtime_type) noexcept
        : references{1}, owner{&allocation_owner}, runtime_type{&actual_runtime_type} {}

    std::atomic_size_t references;
    details::BundleAllocationOwner *owner;
    const ComposedHierarchicalScalarRuntimeType *runtime_type;
};

std::size_t ComposedHierarchicalScalarRuntimeType::align_up(std::size_t offset, std::size_t alignment) noexcept {
    return (offset + alignment - 1) & ~(alignment - 1);
}

ComposedHierarchicalScalarRuntimeType::ComposedHierarchicalScalarRuntimeType(
    const ResolvedHierarchicalScalarType &type) noexcept
    : HierarchicalScalarRuntimeType{type, sizeof(Storage *), alignof(Storage *), true, false,
                                    true, shared_handle_flag} {}

void ComposedHierarchicalScalarRuntimeType::initialize(std::span<const ScalarRuntimeType *const> fields) {
    if (fields.size() != type().field_count()) {
        throw std::logic_error{"hierarchical scalar runtime field count does not match its semantic type"};
    }

    optional_words_ = (std::ranges::count_if(type().fields(), &SchemaField::optional) + 63) / 64;
    auto offset = sizeof(Storage) + optional_words_ * sizeof(std::uint64_t);
    auto alignment = alignof(Storage);
    auto optional_bit = std::uint32_t{};
    fields_.reserve(fields.size());
    for (auto index = std::size_t{}; index < fields.size(); ++index) {
        const auto &field = *fields[index];
        alignment = std::max(alignment, field.alignment());
        offset = align_up(offset, field.alignment());
        if (offset > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"hierarchical scalar storage exceeds 4 GiB"};
        }

        const auto bit = type().field(index).optional ? optional_bit++ : required_field;
        fields_.push_back(FieldLayout{&field, static_cast<std::uint32_t>(offset), bit});
        offset += field.size();
    }

    allocation_alignment_ = alignment;
    allocation_size_ = align_up(offset, alignment);
}

details::ScalarData ComposedHierarchicalScalarRuntimeType::field(const details::ScalarData &value,
                                                                 std::size_t index) const {
    auto *stored = storage(value);
    const auto &actual = *stored->runtime_type;
    if (!actual.field_present(*stored, index)) {
        throw std::bad_optional_access{};
    }

    const auto &layout = actual.fields_.at(index);
    return layout.runtime_type->borrow(actual.field_storage(*stored, index));
}

bool ComposedHierarchicalScalarRuntimeType::field_present(const details::ScalarData &value, std::size_t index) const {
    auto *stored = storage(value);
    return stored->runtime_type->field_present(*stored, index);
}

bool ComposedHierarchicalScalarRuntimeType::accepts_input(const ScalarRuntimeType &source) const noexcept {
    if (typeid(source) != typeid(ComposedHierarchicalScalarRuntimeType)) {
        return false;
    }

    return static_cast<const HierarchicalScalarRuntimeType &>(source).type().derives_from(type());
}

std::uintptr_t
ComposedHierarchicalScalarRuntimeType::construct_shared(std::span<const details::ScalarData *const> fields,
                                                        details::BundleAllocationOwner &owner) const {
    if (fields.size() != fields_.size()) {
        throw std::invalid_argument{"hierarchical scalar field count does not match its type"};
    }

    auto memory = owner.memory();
    auto *bytes = static_cast<std::byte *>(memory.allocate_bytes(allocation_size_, allocation_alignment_));
    owner.retain();
    auto *stored = std::construct_at(reinterpret_cast<Storage *>(bytes), owner, *this);
    std::fill_n(reinterpret_cast<std::uint64_t *>(bytes + sizeof(Storage)), optional_words_, std::uint64_t{});
    auto constructed = std::size_t{};
    auto rollback = util::scope_exit{[&]() noexcept {
        while (constructed != 0) {
            --constructed;
            if (field_present(*stored, constructed)) {
                fields_[constructed].runtime_type->destroy(field_storage(*stored, constructed), memory);
            }
        }
        std::destroy_at(stored);
        memory.deallocate_bytes(bytes, allocation_size_, allocation_alignment_);
        owner.release();
    }};

    for (; constructed < fields.size(); ++constructed) {
        const auto *source = fields[constructed];
        const auto &semantic_field = type().field(constructed);
        if (source == nullptr) {
            if (!semantic_field.optional) {
                throw std::invalid_argument{"required hierarchical scalar field is absent"};
            }
            continue;
        }
        if (!fields_[constructed].runtime_type->accepts_input(source->runtime_type())) {
            throw std::invalid_argument{"hierarchical scalar field has an incompatible runtime representation"};
        }

        fields_[constructed].runtime_type->construct_from_input(field_storage(*stored, constructed), *source, memory);
        set_field_present(*stored, constructed);
    }

    rollback.release();
    return reinterpret_cast<std::uintptr_t>(stored);
}

details::ScalarData ComposedHierarchicalScalarRuntimeType::borrow_shared(const void *value) const noexcept {
    auto *stored = stored_handle(value);
    if (stored == nullptr) {
        return details::ScalarData{*this, value};
    }
    return details::ScalarData{*stored->runtime_type, reinterpret_cast<std::uintptr_t>(stored),
                               details::ScalarDataMode::shared_view};
}

const ScalarRuntimeType *ComposedHierarchicalScalarRuntimeType::retain_shared(const details::ScalarData &value,
                                                                              std::uintptr_t &data) const noexcept {
    auto *stored = storage(value);
    if (stored == nullptr) {
        return nullptr;
    }

    stored->references.fetch_add(1, std::memory_order_relaxed);
    data = reinterpret_cast<std::uintptr_t>(stored);
    return stored->runtime_type;
}

void ComposedHierarchicalScalarRuntimeType::release_shared(std::uintptr_t data) const noexcept {
    auto *stored = reinterpret_cast<Storage *>(data);
    if (stored == nullptr || stored->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    stored->runtime_type->destroy_storage(*stored);
}

void ComposedHierarchicalScalarRuntimeType::construct_initial_value(void *destination) const noexcept {
    store_handle(destination, nullptr);
}

void ComposedHierarchicalScalarRuntimeType::do_construct_from_input(
    void *destination, const details::ScalarData &value, allocator &) const {
    auto *stored = storage(value);
    if (stored == nullptr) {
        throw std::invalid_argument{"cannot copy an empty hierarchical scalar"};
    }
    stored->references.fetch_add(1, std::memory_order_relaxed);
    store_handle(destination, stored);
}

void ComposedHierarchicalScalarRuntimeType::copy_construct(void *destination, const void *source, allocator &) const {
    auto *stored = stored_handle(source);
    if (stored != nullptr) {
        stored->references.fetch_add(1, std::memory_order_relaxed);
    }
    store_handle(destination, stored);
}

void ComposedHierarchicalScalarRuntimeType::do_assign_from_input(
    void *destination, const details::ScalarData &value) const {
    auto *replacement = storage(value);
    if (replacement != nullptr) {
        replacement->references.fetch_add(1, std::memory_order_relaxed);
    }

    auto *previous = stored_handle(destination);
    store_handle(destination, replacement);
    if (previous != nullptr) {
        previous->runtime_type->release_shared(reinterpret_cast<std::uintptr_t>(previous));
    }
}

std::size_t ComposedHierarchicalScalarRuntimeType::hash(details::ScalarDataMode mode,
                                                        std::uintptr_t data) const {
    auto *stored = storage(details::ScalarData{*this, data, mode});
    return stored == nullptr ? 0 : stored->runtime_type->hash_storage(*stored);
}

bool ComposedHierarchicalScalarRuntimeType::equals(details::ScalarDataMode mode, std::uintptr_t data,
                                                   const details::ScalarData &other) const {
    const auto value = details::ScalarData{*this, data, mode};
    auto *left = storage(value);
    if (left == nullptr || &left->runtime_type->type() != &other.type()) {
        return false;
    }

    const auto &actual = *left->runtime_type;
    const auto &right = static_cast<const HierarchicalScalarRuntimeType &>(other.runtime_type());
    for (auto index = std::size_t{}; index < actual.fields_.size(); ++index) {
        const auto left_present = actual.field_present(*left, index);
        if (left_present != right.field_present(other, index)) {
            return false;
        }
        if (left_present && !actual.fields_[index].runtime_type->equals(actual.field_storage(*left, index),
                                                                        right.field(other, index))) {
            return false;
        }
    }
    return true;
}

std::size_t ComposedHierarchicalScalarRuntimeType::hash(const void *value) const {
    auto *stored = stored_handle(value);
    return stored == nullptr ? 0 : stored->runtime_type->hash_storage(*stored);
}

std::size_t ComposedHierarchicalScalarRuntimeType::hash_storage(const Storage &storage) const {
    auto result = std::hash<const HierarchicalScalarType *>{}(&storage.runtime_type->type());
    for (auto index = std::size_t{}; index < fields_.size(); ++index) {
        const auto present = field_present(storage, index);
        if (fields_[index].optional_bit != required_field) {
            result = details::combine_scalar_hash(result, static_cast<std::size_t>(present));
        }
        if (present) {
            result =
                details::combine_scalar_hash(result, fields_[index].runtime_type->hash(field_storage(storage, index)));
        }
    }
    return result;
}

bool ComposedHierarchicalScalarRuntimeType::equals(const void *value, const details::ScalarData &other) const {
    auto *stored = stored_handle(value);
    if (stored == nullptr) {
        return false;
    }
    return stored->runtime_type->equals(details::ScalarDataMode::shared_view, reinterpret_cast<std::uintptr_t>(stored),
                                        other);
}

bool ComposedHierarchicalScalarRuntimeType::equals_same_type(const void *value,
                                                             const details::ScalarData &other) const {
    return equals(value, other);
}

void ComposedHierarchicalScalarRuntimeType::destroy(void *value, allocator &) const noexcept {
    auto *stored = stored_handle(value);
    store_handle(value, nullptr);
    if (stored != nullptr) {
        stored->runtime_type->release_shared(reinterpret_cast<std::uintptr_t>(stored));
    }
}

ComposedHierarchicalScalarRuntimeType::Storage *
ComposedHierarchicalScalarRuntimeType::storage(const details::ScalarData &value) const noexcept {
    if (value.mode() == details::ScalarDataMode::shared_view || value.mode() == details::ScalarDataMode::shared_value) {
        return reinterpret_cast<Storage *>(value.data_word());
    }
    return stored_handle(value.data());
}

ComposedHierarchicalScalarRuntimeType::Storage *
ComposedHierarchicalScalarRuntimeType::stored_handle(const void *value) noexcept {
    auto handle = std::uintptr_t{};
    std::memcpy(&handle, value, sizeof(handle));
    return reinterpret_cast<Storage *>(handle);
}

void ComposedHierarchicalScalarRuntimeType::store_handle(void *destination, Storage *storage) noexcept {
    const auto handle = reinterpret_cast<std::uintptr_t>(storage);
    std::memcpy(destination, &handle, sizeof(handle));
}

const void *ComposedHierarchicalScalarRuntimeType::field_storage(const Storage &storage,
                                                                 std::size_t index) const noexcept {
    return reinterpret_cast<const std::byte *>(&storage) + fields_[index].offset;
}

void *ComposedHierarchicalScalarRuntimeType::field_storage(Storage &storage, std::size_t index) const noexcept {
    return reinterpret_cast<std::byte *>(&storage) + fields_[index].offset;
}

bool ComposedHierarchicalScalarRuntimeType::field_present(const Storage &storage, std::size_t index) const noexcept {
    const auto bit = fields_[index].optional_bit;
    if (bit == required_field) {
        return true;
    }
    const auto *words =
        reinterpret_cast<const std::uint64_t *>(reinterpret_cast<const std::byte *>(&storage) + sizeof(Storage));
    return (words[bit / 64] & (std::uint64_t{1} << (bit % 64))) != 0;
}

void ComposedHierarchicalScalarRuntimeType::set_field_present(Storage &storage, std::size_t index) const noexcept {
    const auto bit = fields_[index].optional_bit;
    if (bit == required_field) {
        return;
    }
    auto *words = reinterpret_cast<std::uint64_t *>(reinterpret_cast<std::byte *>(&storage) + sizeof(Storage));
    words[bit / 64] |= std::uint64_t{1} << (bit % 64);
}

void ComposedHierarchicalScalarRuntimeType::destroy_storage(Storage &storage) const noexcept {
    auto memory = storage.owner->memory();
    for (auto index = fields_.size(); index != 0;) {
        --index;
        if (field_present(storage, index)) {
            fields_[index].runtime_type->destroy(field_storage(storage, index), memory);
        }
    }

    auto *owner = storage.owner;
    auto *bytes = reinterpret_cast<std::byte *>(&storage);
    std::destroy_at(&storage);
    memory.deallocate_bytes(bytes, allocation_size_, allocation_alignment_);
    owner->release();
}

} // namespace

namespace details {

OwnedScalarData construct_composed_hierarchical_scalar_value(
    const ResolvedHierarchicalScalarType &type, std::span<const ScalarData *const> fields,
    BundleAllocationOwner &owner) {
    const auto &runtime_type = static_cast<const ComposedHierarchicalScalarRuntimeType &>(
        resolve_composed_hierarchical_scalar_runtime_type(type));
    const auto data = runtime_type.construct_shared(fields, owner);

    return {&runtime_type, data, ScalarDataMode::shared_value};
}

std::unique_ptr<HierarchicalScalarRuntimeType>
make_composed_hierarchical_scalar_runtime_type(const ResolvedHierarchicalScalarType &type) {
    return std::make_unique<ComposedHierarchicalScalarRuntimeType>(type);
}

void initialize_composed_hierarchical_scalar_runtime_type(HierarchicalScalarRuntimeType &runtime_type,
                                                          std::span<const ScalarRuntimeType *const> fields) {
    static_cast<ComposedHierarchicalScalarRuntimeType &>(runtime_type).initialize(fields);
}

} // namespace details

} // namespace bg

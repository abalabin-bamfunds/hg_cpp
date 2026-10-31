#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/scalar/details/bytes_storage.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/bytes_builder.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace bg::details {

namespace {

std::size_t allocation_size(std::size_t capacity) {
    if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(BytesStorage)) {
        throw std::length_error{"byte storage exceeds the maximum allocation size"};
    }
    return sizeof(BytesStorage) + capacity;
}

} // namespace

BytesStorage::BytesStorage(std::pmr::memory_resource &memory, ScalarAllocationPool *owner, std::size_t size,
                           std::size_t capacity) noexcept
    : memory_{&memory}, owner_{owner}, size_{size}, capacity_{capacity} {}

BytesStorage *BytesStorage::allocate(std::size_t size, std::size_t capacity, allocator &memory,
                                     ScalarAllocationPool *owner) {
    if (size > capacity) {
        throw std::invalid_argument{"byte storage capacity is smaller than its size"};
    }

    if (owner == nullptr) {
        const auto *context = current_scalar_allocation_owner();
        if (context != nullptr && &memory.memory_resource() == &context->memory.memory_resource()) {
            owner = context->scalar_allocation_pool;
        }
    }

    auto *resource = owner == nullptr ? &memory.memory_resource() : &owner->memory().memory_resource();
    const auto total_size = allocation_size(capacity);
    auto *storage = static_cast<BytesStorage *>(resource->allocate(total_size, alignof(BytesStorage)));
    auto *result = ::new (storage) BytesStorage{*resource, owner, size, capacity};
    if (owner != nullptr) {
        owner->retain();
    }
    return result;
}

BytesStorage *BytesStorage::copy(std::span<const std::byte> value, allocator &memory) {
    if (value.empty()) {
        return nullptr;
    }

    auto *result = allocate(value.size(), value.size(), memory, nullptr);
    std::memcpy(result->mutable_data(), value.data(), value.size());
    return result;
}

BytesStorage *BytesStorage::copy(std::span<const std::byte> value) {
    if (value.empty()) {
        return nullptr;
    }

    auto memory = allocator{};
    if (const auto *context = current_scalar_allocation_owner()) {
        memory = context->memory;
    }
    return copy(value, memory);
}

BytesStorage *BytesStorage::from(const ScalarData &value) noexcept {
    if (&value.runtime_type() != &cpp_scalar_runtime_type<bytes>()) {
        return nullptr;
    }

    if (value.mode() == ScalarDataMode::shared_view || value.mode() == ScalarDataMode::shared_value) {
        return reinterpret_cast<BytesStorage *>(value.data_word());
    }

    if (value.mode() == ScalarDataMode::stored_view || value.mode() == ScalarDataMode::owned_explicit_allocator ||
        value.mode() == ScalarDataMode::owned_context_allocator ||
        value.mode() == ScalarDataMode::owned_default_allocator) {
        return *static_cast<BytesStorage *const *>(value.data());
    }

    return nullptr;
}

std::span<const std::byte> BytesStorage::value() const noexcept { return {data(), size_}; }

void BytesStorage::retain() noexcept {
    if (references_.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::terminate();
    }
}

void BytesStorage::release() noexcept {
    if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        deallocate(this);
    }
}

BytesStorage *BytesStorage::create_builder(std::size_t capacity, allocator &memory) {
    return allocate(0, capacity, memory, nullptr);
}

BytesStorage *BytesStorage::grow(BytesStorage &source, std::size_t capacity) {
    auto memory = allocator{*source.memory_};
    auto *result = allocate(source.size_, capacity, memory, source.owner_);
    std::memcpy(result->mutable_data(), source.data(), source.size_);
    return result;
}

void BytesStorage::deallocate(BytesStorage *storage) noexcept {
    auto *resource = storage->memory_;
    auto *owner = storage->owner_;
    const auto size = sizeof(BytesStorage) + storage->capacity_;
    std::destroy_at(storage);
    resource->deallocate(storage, size, alignof(BytesStorage));
    if (owner != nullptr) {
        owner->release();
    }
}

const std::byte *BytesStorage::data() const noexcept { return reinterpret_cast<const std::byte *>(this + 1); }

std::byte *BytesStorage::mutable_data() noexcept { return reinterpret_cast<std::byte *>(this + 1); }

void BytesStorage::set_size(std::size_t size) {
    if (size > capacity_) {
        throw std::length_error{"byte builder size exceeds capacity"};
    }
    size_ = size;
}

} // namespace bg::details

namespace bg {

BytesBuilder::BytesBuilder() {
    auto memory = allocator{};
    if (const auto *context = details::current_scalar_allocation_owner()) {
        memory = context->memory;
    }
    storage_ = details::BytesStorage::create_builder(64, memory);
}

BytesBuilder::BytesBuilder(allocator &memory, std::size_t initial_capacity)
    : storage_{details::BytesStorage::create_builder(initial_capacity, memory)} {}

BytesBuilder::~BytesBuilder() {
    if (storage_ != nullptr) {
        storage_->release();
    }
}

BytesBuilder::BytesBuilder(BytesBuilder &&other) noexcept : storage_{std::exchange(other.storage_, nullptr)} {}

BytesBuilder &BytesBuilder::operator=(BytesBuilder &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (storage_ != nullptr) {
        storage_->release();
    }
    storage_ = std::exchange(other.storage_, nullptr);
    return *this;
}

void BytesBuilder::reserve(std::size_t capacity) {
    if (capacity <= storage_->capacity()) {
        return;
    }

    const auto largest = std::numeric_limits<std::size_t>::max() - sizeof(details::BytesStorage);
    if (capacity > largest) {
        throw std::length_error{"byte builder exceeds the maximum allocation size"};
    }

    const auto current = std::max<std::size_t>(1, storage_->capacity());
    const auto doubled = current > largest / 2 ? largest : current * 2;
    auto *replacement = details::BytesStorage::grow(*storage_, std::max(capacity, doubled));
    auto *previous = std::exchange(storage_, replacement);
    previous->release();
}

std::span<std::byte> BytesBuilder::writable_tail() noexcept {
    if (storage_ == nullptr) {
        return {};
    }
    auto *begin = storage_->mutable_data() + storage_->size();
    return {begin, storage_->capacity() - storage_->size()};
}

void BytesBuilder::commit(std::size_t size) {
    if (size > storage_->capacity() - storage_->size()) {
        throw std::length_error{"byte builder commit exceeds writable capacity"};
    }
    storage_->set_size(storage_->size() + size);
}

void BytesBuilder::append(std::span<const std::byte> value) {
    if (value.empty()) {
        return;
    }

    const auto length = storage_->size();
    if (value.size() > std::numeric_limits<std::size_t>::max() - length) {
        throw std::length_error{"byte builder exceeds the maximum size"};
    }

    const auto source = reinterpret_cast<std::uintptr_t>(value.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(storage_->mutable_data());
    const auto source_offset = source >= begin ? source - begin : std::uintptr_t{};
    const auto aliases_storage = source >= begin && source_offset <= length && value.size() <= length - source_offset;

    reserve(length + value.size());
    if (aliases_storage) {
        value = {storage_->mutable_data() + source_offset, value.size()};
    }
    std::memcpy(storage_->mutable_data() + length, value.data(), value.size());
    storage_->set_size(length + value.size());
}

std::span<const std::byte> BytesBuilder::view() const noexcept {
    return storage_ == nullptr ? std::span<const std::byte>{} : storage_->value();
}

ScalarValue<bytes> BytesBuilder::finish() && {
    if (storage_ == nullptr || storage_->size() == 0) {
        if (storage_ != nullptr) {
            storage_->release();
            storage_ = nullptr;
        }
        return ScalarValue<bytes>{std::span<const std::byte>{}};
    }

    auto *handle = storage_;
    const auto value = Scalar<bytes>::from(cpp_scalar_runtime_type<bytes>(), &handle);
    auto result = ScalarValue<bytes>{value};
    storage_ = nullptr;
    handle->release();
    return result;
}

} // namespace bg

namespace bg::details {

CppScalarRuntimeType<bytes>::CppScalarRuntimeType() : Base{cpp_scalar_type<bytes>(), shared_handle_flag} {}

ScalarData CppScalarRuntimeType<bytes>::borrow_shared(const void *source) const noexcept {
    const auto *slot = static_cast<BytesStorage *const *>(source);
    return {*this, reinterpret_cast<std::uintptr_t>(*slot), ScalarDataMode::shared_view};
}

const ScalarRuntimeType *CppScalarRuntimeType<bytes>::retain_shared(const ScalarData &source,
                                                                     std::uintptr_t &data) const noexcept {
    if (&source.runtime_type() != this || source.mode() == ScalarDataMode::source_view ||
        source.mode() == ScalarDataMode::inline_value) {
        return nullptr;
    }

    auto *storage = BytesStorage::from(source);
    if (storage != nullptr) {
        storage->retain();
    }
    data = reinterpret_cast<std::uintptr_t>(storage);
    return this;
}

void CppScalarRuntimeType<bytes>::release_shared(std::uintptr_t data) const noexcept {
    if (auto *storage = reinterpret_cast<BytesStorage *>(data); storage != nullptr) {
        storage->release();
    }
}

void CppScalarRuntimeType<bytes>::read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const {
    std::construct_at(static_cast<std::span<const std::byte> *>(destination),
                      scalar_data_value<bytes>(ScalarData{*this, data, mode}));
}

void CppScalarRuntimeType<bytes>::do_construct_from_input(void *destination, const ScalarData &source,
                                                           allocator &memory) const {
    auto *storage = BytesStorage::from(source);
    if (storage != nullptr || (&source.runtime_type() == this && source.mode() != ScalarDataMode::source_view)) {
        if (storage != nullptr) {
            storage->retain();
        }
        std::construct_at(static_cast<BytesStorage **>(destination), storage);
        return;
    }

    scalar_storage_traits<bytes>::construct(static_cast<BytesStorage **>(destination),
                                            scalar_data_value<bytes>(source), memory);
}

void CppScalarRuntimeType<bytes>::do_assign_from_input(void *destination, const ScalarData &source) const {
    auto *replacement = BytesStorage::from(source);
    if (replacement != nullptr) {
        replacement->retain();
    } else if (&source.runtime_type() != this || source.mode() == ScalarDataMode::source_view) {
        replacement = BytesStorage::copy(scalar_data_value<bytes>(source));
    }

    auto *previous = std::exchange(*static_cast<BytesStorage **>(destination), replacement);
    if (previous != nullptr) {
        previous->release();
    }
}

} // namespace bg::details

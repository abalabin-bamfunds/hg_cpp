#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/string_builder.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace bg::details {

namespace {

constexpr auto maximum_string_size = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

std::size_t allocation_size(std::size_t capacity) {
    if (capacity > maximum_string_size) {
        throw std::length_error{"string storage exceeds the 4 GiB limit"};
    }
    if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(StringStorage) - 1) {
        throw std::length_error{"string storage exceeds the maximum allocation size"};
    }
    return sizeof(StringStorage) + capacity + 1;
}

} // namespace

StringStorage::StringStorage(std::pmr::memory_resource &memory, ScalarAllocationPool *owner, std::size_t length,
                             std::size_t capacity) noexcept
    : memory_{&memory}, owner_{owner}, length_{static_cast<std::uint32_t>(length)},
      capacity_{static_cast<std::uint32_t>(capacity)} {}

StringStorage *StringStorage::allocate(std::size_t length, std::size_t capacity, allocator &memory,
                                       ScalarAllocationPool *owner) {
    if (length > maximum_string_size || capacity > maximum_string_size) {
        throw std::length_error{"string storage exceeds the 4 GiB limit"};
    }
    if (length > capacity) {
        throw std::invalid_argument{"string storage capacity is smaller than its length"};
    }

    if (owner == nullptr) {
        const auto *context = current_scalar_allocation_owner();
        if (context != nullptr && &memory.memory_resource() == &context->memory.memory_resource()) {
            owner = context->scalar_allocation_pool;
        }
    }

    auto *resource = owner == nullptr ? &memory.memory_resource() : &owner->memory().memory_resource();
    const auto size = allocation_size(capacity);
    auto *storage = static_cast<StringStorage *>(resource->allocate(size, alignof(StringStorage)));
    auto *result = ::new (storage) StringStorage{*resource, owner, length, capacity};
    if (owner != nullptr) {
        owner->retain();
    }
    result->mutable_data()[length] = '\0';
    return result;
}

StringStorage *StringStorage::copy(std::string_view value, allocator &memory) {
    if (value.empty()) {
        return nullptr;
    }

    auto *result = allocate(value.size(), value.size(), memory, nullptr);
    std::memcpy(result->mutable_data(), value.data(), value.size());
    result->mutable_data()[value.size()] = '\0';
    return result;
}

StringStorage *StringStorage::copy(std::string_view value) {
    if (value.empty()) {
        return nullptr;
    }

    auto memory = allocator{};
    if (const auto *context = current_scalar_allocation_owner()) {
        memory = context->memory;
    }
    return copy(value, memory);
}

StringStorage *StringStorage::from(const ScalarData &value) noexcept {
    if (&value.runtime_type() != &cpp_scalar_runtime_type<string>()) {
        return nullptr;
    }

    if (value.mode() == ScalarDataMode::shared_view || value.mode() == ScalarDataMode::shared_value) {
        return reinterpret_cast<StringStorage *>(value.data_word());
    }

    if (value.mode() == ScalarDataMode::stored_view || value.mode() == ScalarDataMode::owned_explicit_allocator ||
        value.mode() == ScalarDataMode::owned_context_allocator ||
        value.mode() == ScalarDataMode::owned_default_allocator) {
        return *static_cast<StringStorage *const *>(value.data());
    }

    return nullptr;
}

std::string_view StringStorage::value() const noexcept { return {data(), length_}; }

const char *StringStorage::data() const noexcept { return reinterpret_cast<const char *>(this + 1); }

StringStorageFinalReleaseHook *
StringStorage::install_final_release_hook(StringStorageFinalReleaseHook *hook) noexcept {
    auto *installed = static_cast<StringStorageFinalReleaseHook *>(nullptr);
    if (final_release_hook_.compare_exchange_strong(installed, hook, std::memory_order_release,
                                                     std::memory_order_acquire)) {
        return hook;
    }
    return installed;
}

bool StringStorage::try_retain() noexcept {
    auto count = references_.load(std::memory_order_acquire);
    while (count != 0) {
        if (references_.compare_exchange_weak(count, count + 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void StringStorage::retain() noexcept {
    if (references_.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::terminate();
    }
}

void StringStorage::release() noexcept {
    if (references_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    if (const auto *hook = final_release_hook_.load(std::memory_order_acquire); hook != nullptr) {
        hook->callback(hook->context, this);
    }
    deallocate(this);
}

StringStorage *StringStorage::create_builder(std::size_t capacity, allocator &memory) {
    return allocate(0, capacity, memory, nullptr);
}

StringStorage *StringStorage::grow(StringStorage &source, std::size_t capacity) {
    auto memory = allocator{*source.memory_};
    auto *result = allocate(source.length_, capacity, memory, source.owner_);
    std::memcpy(result->mutable_data(), source.data(), source.length_);
    result->mutable_data()[source.length_] = '\0';
    return result;
}

void StringStorage::deallocate(StringStorage *storage) noexcept {
    auto *resource = storage->memory_;
    auto *owner = storage->owner_;
    const auto size = sizeof(StringStorage) + storage->capacity_ + 1;
    std::destroy_at(storage);
    resource->deallocate(storage, size, alignof(StringStorage));
    if (owner != nullptr) {
        owner->release();
    }
}

char *StringStorage::mutable_data() noexcept { return reinterpret_cast<char *>(this + 1); }

void StringStorage::set_size(std::size_t size) {
    if (size > capacity_) {
        throw std::length_error{"string builder size exceeds capacity"};
    }
    length_ = static_cast<std::uint32_t>(size);
    mutable_data()[size] = '\0';
}

} // namespace bg::details

namespace bg {

StringBuilder::StringBuilder() {
    auto memory = allocator{};
    if (const auto *context = details::current_scalar_allocation_owner()) {
        memory = context->memory;
    }
    storage_ = details::StringStorage::create_builder(64, memory);
}

StringBuilder::StringBuilder(allocator &memory, std::size_t initial_capacity)
    : storage_{details::StringStorage::create_builder(initial_capacity, memory)} {}

StringBuilder::~StringBuilder() {
    if (storage_ != nullptr) {
        storage_->release();
    }
}

StringBuilder::StringBuilder(StringBuilder &&other) noexcept : storage_{std::exchange(other.storage_, nullptr)} {}

StringBuilder &StringBuilder::operator=(StringBuilder &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (storage_ != nullptr) {
        storage_->release();
    }
    storage_ = std::exchange(other.storage_, nullptr);
    return *this;
}

void StringBuilder::reserve(std::size_t capacity) {
    if (capacity <= storage_->capacity()) {
        return;
    }

    if (capacity > details::maximum_string_size) {
        throw std::length_error{"string builder exceeds the 4 GiB limit"};
    }

    const auto largest = details::maximum_string_size;
    const auto current = std::max<std::size_t>(1, storage_->capacity());
    const auto doubled = current > largest / 2 ? largest : current * 2;
    auto *replacement = details::StringStorage::grow(*storage_, std::max(capacity, doubled));
    auto *previous = std::exchange(storage_, replacement);
    previous->release();
}

void StringBuilder::append(std::string_view value) {
    if (value.empty()) {
        return;
    }

    const auto length = storage_->size();
    if (value.size() > std::numeric_limits<std::size_t>::max() - length) {
        throw std::length_error{"string builder exceeds the maximum size"};
    }

    const auto source = reinterpret_cast<std::uintptr_t>(value.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(storage_->data());
    const auto source_offset = source >= begin ? source - begin : std::uintptr_t{};
    const auto aliases_storage = source >= begin && source_offset <= length && value.size() <= length - source_offset;

    reserve(length + value.size());
    if (aliases_storage) {
        value = {storage_->data() + source_offset, value.size()};
    }
    std::memcpy(storage_->mutable_data() + length, value.data(), value.size());
    storage_->set_size(length + value.size());
}

void StringBuilder::append(char value) { append(std::string_view{&value, 1}); }

std::string_view StringBuilder::view() const noexcept { return storage_->value(); }

ScalarValue<string> StringBuilder::finish() && {
    if (storage_ == nullptr) {
        return ScalarValue<string>{std::string_view{}};
    }

    if (storage_->size() == 0) {
        storage_->release();
        storage_ = nullptr;
        return ScalarValue<string>{std::string_view{}};
    }

    auto *handle = storage_;
    const auto view = Scalar<string>::from(cpp_scalar_runtime_type<string>(), &handle);
    auto result = ScalarValue<string>{view};
    storage_ = nullptr;
    handle->release();
    return result;
}

} // namespace bg

namespace bg::details {

CppScalarRuntimeType<string>::CppScalarRuntimeType()
    : Base{cpp_scalar_type<string>(), shared_handle_flag} {}

ScalarData CppScalarRuntimeType<string>::borrow_shared(const void *source) const noexcept {
    const auto *slot = static_cast<StringStorage *const *>(source);
    return {*this, reinterpret_cast<std::uintptr_t>(*slot), ScalarDataMode::shared_view};
}

const ScalarRuntimeType *CppScalarRuntimeType<string>::retain_shared(const ScalarData &source,
                                                                     std::uintptr_t &data) const noexcept {
    if (&source.runtime_type() != this || source.mode() == ScalarDataMode::source_view ||
        source.mode() == ScalarDataMode::inline_value) {
        return nullptr;
    }

    auto *storage = StringStorage::from(source);
    if (storage != nullptr) {
        storage->retain();
    }
    data = reinterpret_cast<std::uintptr_t>(storage);
    return this;
}

void CppScalarRuntimeType<string>::release_shared(std::uintptr_t data) const noexcept {
    if (auto *storage = reinterpret_cast<StringStorage *>(data); storage != nullptr) {
        storage->release();
    }
}

void CppScalarRuntimeType<string>::read_value(ScalarDataMode mode, std::uintptr_t data,
                                              void *destination) const {
    std::construct_at(static_cast<std::string_view *>(destination),
                      scalar_data_value<string>(ScalarData{*this, data, mode}));
}

void CppScalarRuntimeType<string>::do_construct_from_input(void *destination, const ScalarData &source,
                                                           allocator &memory) const {
    auto *storage = StringStorage::from(source);
    if (storage != nullptr || (&source.runtime_type() == this && source.mode() != ScalarDataMode::source_view)) {
        if (storage != nullptr) {
            storage->retain();
        }
        std::construct_at(static_cast<StringStorage **>(destination), storage);
        return;
    }

    scalar_storage_traits<string>::construct(static_cast<StringStorage **>(destination),
                                             scalar_data_value<string>(source), memory);
}

void CppScalarRuntimeType<string>::do_assign_from_input(void *destination, const ScalarData &source) const {
    auto *replacement = StringStorage::from(source);
    if (replacement != nullptr) {
        replacement->retain();
    } else if (&source.runtime_type() != this || source.mode() == ScalarDataMode::source_view) {
        replacement = StringStorage::copy(scalar_data_value<string>(source));
    }

    auto *previous = std::exchange(*static_cast<StringStorage **>(destination), replacement);
    if (previous != nullptr) {
        previous->release();
    }
}

} // namespace bg::details
